// Exercise production metadata, layout limits and texture access classification without a GPU.
#include "GarnetDevice.m"

@interface RDNA4QueryDimensionsProbe : NSObject
@property NSUInteger width, height, depth, arrayLength, mipmapLevelCount, sampleCount;
@end
@implementation RDNA4QueryDimensionsProbe
@end

static unsigned checks, creates, queries;
static unsigned createAttempts, createFailAt, destroyedLayouts;
static uint32_t layoutLimit = 1000000;
static BOOL layoutSupported = YES;
static BOOL fixedLayoutSupported = YES;
static VkShaderStageFlags layoutStages[3];
static unsigned layoutStageMask;
static void require(BOOL ok, const char *message) {
    checks++;
    if (!ok) { fprintf(stderr, "ARGUMENT_TABLES_FAIL %s\n", message); exit(1); }
}
static void resetLayoutStages(void) {
    memset(layoutStages, 0, sizeof layoutStages);
    layoutStageMask = 0;
}
static void VKAPI_CALL layoutSupport(VkDevice device, const VkDescriptorSetLayoutCreateInfo *info,
    VkDescriptorSetLayoutSupport *support) {
    (void)device;
    queries++;
    require(info->bindingCount == 1, "layout query binding count");
    if (support->pNext) {
        require(info->pBindings[0].descriptorCount == 1, "query minimum variable binding");
        support->supported = layoutSupported;
        ((VkDescriptorSetVariableDescriptorCountLayoutSupport *)support->pNext)->maxVariableDescriptorCount = layoutLimit;
    } else {
        require(info->pBindings[0].descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER &&
            info->pBindings[0].descriptorCount == 94, "query full fixed sampler binding");
        support->supported = fixedLayoutSupported;
    }
}
static VkResult VKAPI_CALL createLayout(VkDevice device, const VkDescriptorSetLayoutCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkDescriptorSetLayout *layout) {
    (void)device; (void)allocator;
    ++createAttempts;
    if (createFailAt && createAttempts == createFailAt) {
        createFailAt = 0;
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    if (info->bindingCount) {
        const VkDescriptorSetLayoutBindingFlagsCreateInfo *flags = info->pNext;
        BOOL variable = (flags->pBindingFlags[0] & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT) != 0;
        unsigned category = info->pBindings[0].descriptorType ==
                VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ? 0u :
            info->pBindings[0].descriptorType ==
                VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ? 1u : 2u;
        require(category < 3 &&
            info->pBindings[0].descriptorType ==
                (category == 0 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE :
                 category == 1 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE :
                                 VK_DESCRIPTOR_TYPE_SAMPLER),
            "layout descriptor category");
        layoutStages[category] = info->pBindings[0].stageFlags;
        layoutStageMask |= 1u << category;
        require(info->bindingCount == 1 && info->pBindings[0].binding == 0 &&
            info->pBindings[0].stageFlags != 0 &&
            (variable ? info->pBindings[0].descriptorCount <= layoutLimit : info->pBindings[0].descriptorCount == 94) &&
            info->flags == VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT && flags &&
            flags->bindingCount == 1 && flags->pBindingFlags[0] == ((variable ? VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT : 0) |
                VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT), "variable layout flags/limit");
    } else require(!info->flags, "empty category layout flags");
    *layout = (VkDescriptorSetLayout)(uintptr_t)++creates;
    return VK_SUCCESS;
}
static void VKAPI_CALL destroyLayout(VkDevice device, VkDescriptorSetLayout layout,
    const VkAllocationCallbacks *allocator) {
    (void)device; (void)allocator; (void)layout;
    ++destroyedLayouts;
}
static NSDictionary *metadata(id writable) {
    return @{ @"argument_resource_tables": @{ @"version": @1, @"table_binding": @672,
        @"roots": @[@{@"buffer_index": @3, @"binding": @707}] },
        @"bindings": @[@{ @"kind": @"EmbeddedArgBufferTexture",
            @"texture_shape": @{@"writable": writable},
            @"descriptor": @{@"set": @2, @"binding": @0, @"count": @0},
            @"embedded_source": @{@"buffer_index": @3, @"field_offset": @16} }] };
}
static void checkMetadata(RDNA4sRefl *reflection, int stageSet, NSDictionary *meta, BOOL expected) {
    NSError *error = nil;
    BOOL accepted = rdna4_argument_table_metadata(reflection, stageSet, meta, &error);
    if (accepted != expected || (expected ? error != nil : error == nil))
        fprintf(stderr, "metadata expected=%d accepted=%d stage=%d error=%s input=%s\n",
            expected, accepted, stageSet, error.description.UTF8String,
            meta.description.UTF8String);
    require(accepted == expected &&
        (expected ? !error : error != nil), "metadata acceptance/error");
}
static NSDictionary *metadata9(NSArray *categories, NSArray *shapes) {
    return @{ @"trimmed_from_reflection_version": @65,
        @"normalized_sampler_coordinates": @YES,
        @"argument_resource_tables": @{ @"version": @9,
            @"table_binding": @672, @"categories": categories,
            @"comparison_sampler": @NO, @"null_shapes": shapes },
        @"bindings": @[] };
}
static NSDictionary *metadata10(NSArray *categories, NSArray *shapes) {
    return @{ @"trimmed_from_reflection_version": @66,
        @"normalized_sampler_coordinates": @YES,
        @"argument_resource_tables": @{ @"version": @10,
            @"table_binding": @672, @"categories": categories,
            @"comparison_sampler": @NO,
            @"roots": @[@{@"buffer_index": @3, @"binding": @707}],
            @"null_shapes": shapes },
        @"bindings": @[] };
}
static void setLimits(void) {
    RDNA4R.descriptorLimits = (VkPhysicalDeviceVulkan12Properties){
        .maxPerStageDescriptorUpdateAfterBindSampledImages = 1000000,
        .maxPerStageDescriptorUpdateAfterBindStorageImages = 1000000,
        .maxPerStageDescriptorUpdateAfterBindSamplers = 96,
        .maxDescriptorSetUpdateAfterBindSampledImages = 1000000,
        .maxDescriptorSetUpdateAfterBindStorageImages = 1000000,
        .maxDescriptorSetUpdateAfterBindSamplers = 96,
        .maxPerStageUpdateAfterBindResources = 1000000 };
    RDNA4R.lim.maxBoundDescriptorSets = 8;
    RDNA4R.lim.maxStorageBufferRange = UINT32_MAX;
    RDNA4R.lim.maxPerStageDescriptorSampledImages = 1000000;
    RDNA4R.lim.maxPerStageDescriptorStorageImages = 1000000;
    RDNA4R.lim.maxPerStageDescriptorSamplers = 96;
    RDNA4R.lim.maxDescriptorSetSampledImages = 1000000;
    RDNA4R.lim.maxDescriptorSetStorageImages = 1000000;
    RDNA4R.lim.maxDescriptorSetSamplers = 96;
    RDNA4R.lim.maxPerStageResources = 1000000;
    RDNA4R.lim.maxPerStageDescriptorUniformBuffers = 1000000;
    RDNA4R.lim.maxPerStageDescriptorStorageBuffers = 1000000;
    RDNA4R.lim.maxPerStageDescriptorInputAttachments = 1000000;
    RDNA4R.lim.maxDescriptorSetUniformBuffers = 1000000;
    RDNA4R.lim.maxDescriptorSetUniformBuffersDynamic = 16;
    RDNA4R.lim.maxDescriptorSetStorageBuffers = 1000000;
    RDNA4R.lim.maxDescriptorSetStorageBuffersDynamic = 8;
    RDNA4R.lim.maxDescriptorSetInputAttachments = 1000000;
    RDNA4R.lim.maxFragmentCombinedOutputResources = 1000000;
    RDNA4R.lim.maxColorAttachments = 8;
    RDNA4R.lim.maxFragmentOutputAttachments = 8;
    RDNA4R.descriptorLimits.maxPerStageDescriptorUpdateAfterBindUniformBuffers = 1000000;
    RDNA4R.descriptorLimits.maxPerStageDescriptorUpdateAfterBindStorageBuffers = 1000000;
    RDNA4R.descriptorLimits.maxPerStageDescriptorUpdateAfterBindInputAttachments = 1000000;
    RDNA4R.descriptorLimits.maxDescriptorSetUpdateAfterBindUniformBuffers = 1000000;
    RDNA4R.descriptorLimits.maxDescriptorSetUpdateAfterBindUniformBuffersDynamic = 16;
    RDNA4R.descriptorLimits.maxDescriptorSetUpdateAfterBindStorageBuffers = 1000000;
    RDNA4R.descriptorLimits.maxDescriptorSetUpdateAfterBindStorageBuffersDynamic = 8;
    RDNA4R.descriptorLimits.maxDescriptorSetUpdateAfterBindInputAttachments = 1000000;
    RDNA4R.maxPerSetDescriptors = 1000000;
}
int main(void) {
    @autoreleasepool {
        RDNA4sRefl *v = calloc(1, sizeof *v), *f = calloc(1, sizeof *f);
        require(v && f, "reflection allocation");
        RDNA4R.gpuArgumentTables = YES;
        v->maxset = 2;
        v->bind[0][672] = (RDNA4sBind){.used = 1, .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .count = 1};
        v->bind[0][707] = v->bind[0][672];
        v->bind[2][0] = (RDNA4sBind){.used = 1, .type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .runtime = 1};
        checkMetadata(v, 0, metadata(@NO), YES);
        for (id value in @[[NSNull null], @0, @1, @0.5, @"false", @[], @{}]) checkMetadata(v, 0, metadata(value), NO);
        checkMetadata(v, 0, metadata(@YES), NO);
        v->bind[2][0].runtime = 0; checkMetadata(v, 0, metadata(@NO), NO); v->bind[2][0].runtime = 1;
        v->bind[2][1] = v->bind[2][0]; checkMetadata(v, 0, metadata(@NO), NO); v->bind[2][1] = (RDNA4sBind){0};
        v->bind[0][707].count = 2; checkMetadata(v, 0, metadata(@NO), NO); v->bind[0][707].count = 1;
        v->bind[1][672] = v->bind[0][672]; v->bind[1][707] = v->bind[0][707];
        checkMetadata(v, 1, metadata(@NO), YES);
        RDNA4R.gpuArgumentTables = NO; checkMetadata(v, 0, metadata(@NO), NO); RDNA4R.gpuArgumentTables = YES;

        NSDictionary *graph4 = @{ @"argument_resource_tables": @{ @"version": @4,
            @"table_binding": @672, @"categories": @[@0],
            @"roots": @[@{@"buffer_index": @3, @"binding": @707}] },
            @"bindings": metadata(@NO)[@"bindings"] };
        checkMetadata(v, 0, graph4, YES);
        NSMutableDictionary *graph5 = [graph4 mutableCopy];
        NSMutableDictionary *table5 = [graph4[@"argument_resource_tables"] mutableCopy];
        table5[@"version"] = @5;
        graph5[@"argument_resource_tables"] = table5;
        checkMetadata(v, 0, graph5, YES);
        table5[@"version"] = @6;
        checkMetadata(v, 0, graph5, YES);
        table5[@"version"] = @7;
        checkMetadata(v, 0, graph5, NO);
        table5[@"comparison_sampler"] = @NO;
        checkMetadata(v, 0, graph5, YES);
        for (id value in @[@0, @1, @0.5, @"false", [NSNull null]]) {
            table5[@"comparison_sampler"] = value;
            checkMetadata(v, 0, graph5, NO);
        }
        table5[@"comparison_sampler"] = @YES;
        checkMetadata(v, 0, graph5, NO);
        table5[@"comparison_sampler"] = @NO;
        table5[@"version"] = @8;
        checkMetadata(v, 0, graph5, NO);
        table5[@"version"] = @6;
        checkMetadata(v, 0, graph5, NO);
        checkMetadata(v, 0, @{ @"argument_resource_tables": @{ @"version": @4,
            @"table_binding": @672, @"categories": @[@0] }, @"bindings": metadata(@NO)[@"bindings"] }, NO);
        checkMetadata(v, 0, @{ @"argument_resource_tables": @{ @"version": @4,
            @"table_binding": @672, @"categories": @[] }, @"bindings": @[] }, NO);

        *f = *v;
        v->bind[0][707] = (RDNA4sBind){0};
        v->bind[3][0] = (RDNA4sBind){.used = 1,
            .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .runtime = 1};
        v->argumentNullShapes[0] = 0x001;
        v->bind[0][674] = (RDNA4sBind){.used = 1,
            .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .count = 1};
        v->argumentNullShapes[1] = 0x101;
        v->argumentNullShapeCount = 2;
        checkMetadata(v, 0, metadata9(@[@0, @1], @[@0x001, @0x101]), YES);
        checkMetadata(v, 0, metadata9(@[@0, @1], @[@0x101, @0x001]), NO);
        checkMetadata(v, 0, metadata9(@[@0, @1], @[@0x001, @0x001]), NO);
        checkMetadata(v, 0, metadata9(@[@0], @[@0x001, @0x101]), NO);
        v->bind[2][1] = (RDNA4sBind){.used = 1,
            .type = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, .runtime = 1,
            .texelComponentKinds = 1u << RDNA4NS_COMP_FLOAT};
        v->argumentNullShapes[2] = 0x204;
        v->argumentNullShapeCount = 3;
        checkMetadata(v, 0, metadata9(@[@0, @1], @[@0x001, @0x101, @0x204]), YES);
        v->bind[2][1] = (RDNA4sBind){0};
        v->argumentNullShapeCount = 2;
        checkMetadata(v, 0, metadata9(@[@0, @1], @[@0x001, @0x101]), YES);
        RDNA4sRefl shapeOnly = {0};
        shapeOnly.maxset = 2;
        shapeOnly.bind[0][674] = (RDNA4sBind){.used = 1,
            .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .count = 1};
        shapeOnly.bind[2][0] = (RDNA4sBind){.used = 1,
            .type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .runtime = 1};
        shapeOnly.argumentNullShapes[0] = 0x041;
        shapeOnly.argumentNullShapeCount = 1;
        checkMetadata(&shapeOnly, 0, metadata9(@[@0], @[@0x041]), YES);
        shapeOnly.bind[0][674] = (RDNA4sBind){0};
        checkMetadata(&shapeOnly, 0, metadata9(@[@0], @[@0x041]), NO);
        RDNA4sRefl texelGraph = {0};
        texelGraph.maxset = 2;
        texelGraph.bind[0][672] = (RDNA4sBind){.used = 1,
            .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .count = 1};
        texelGraph.bind[0][674] = texelGraph.bind[0][672];
        texelGraph.bind[0][707] = texelGraph.bind[0][672];
        texelGraph.bind[2][1] = (RDNA4sBind){.used = 1,
            .type = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
            .runtime = 1, .texelComponentKinds = 1u << RDNA4NS_COMP_FLOAT};
        texelGraph.argumentNullShapes[0] = 0x204;
        texelGraph.argumentNullShapeCount = 1;
        NSDictionary *texel10 = metadata10(@[@3], @[@0x204]);
        checkMetadata(&texelGraph, 0, texel10, YES);
        RDNA4ArgumentTables texelTables = {0}; NSError *error = nil;
        require(rdna4_argument_table_metadata_apply(&texelTables,
            &texelGraph, 0, texel10, &error) && texelTables.shapeRegistry &&
            texelTables.abi10 && texelTables.dynamicTexel[0] &&
            !texelTables.dynamicTexel[1],
            "ABI 10 accepts and records live uniform texel categories");
        RDNA4sRefl badTexelGraph = texelGraph;
        badTexelGraph.bind[2][1].type = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
        checkMetadata(&badTexelGraph, 0, texel10, NO);
        badTexelGraph = texelGraph;
        badTexelGraph.bind[2][1].runtime = 0;
        checkMetadata(&badTexelGraph, 0, texel10, NO);
        badTexelGraph = texelGraph;
        badTexelGraph.bind[2][1].texelComponentKinds = 0;
        checkMetadata(&badTexelGraph, 0, texel10, NO);
        checkMetadata(&texelGraph, 0, metadata10(@[@4], @[@0x204]), NO);
        checkMetadata(&texelGraph, 0, metadata10(@[@3], @[@0x314]), NO);
        NSMutableDictionary *unknownTexel10 = [texel10 mutableCopy];
        NSMutableDictionary *unknownTexelTable =
            [texel10[@"argument_resource_tables"] mutableCopy];
        unknownTexelTable[@"version"] = @11;
        unknownTexel10[@"argument_resource_tables"] = unknownTexelTable;
        checkMetadata(&texelGraph, 0, unknownTexel10, NO);
        RDNA4sRefl storageTexelGraph = texelGraph;
        storageTexelGraph.maxset = 3;
        storageTexelGraph.bind[2][1] = (RDNA4sBind){0};
        storageTexelGraph.bind[3][1] = (RDNA4sBind){.used = 1,
            .type = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,
            .runtime = 1, .texelComponentKinds = 1u << RDNA4NS_COMP_UINT};
        storageTexelGraph.argumentNullShapes[0] = 0x314;
        checkMetadata(&storageTexelGraph, 0,
            metadata10(@[@4], @[@0x314]), YES);
        *v = *f;

        *f = *v; f->bind[0][707] = (RDNA4sBind){0}; f->bind[1][707] = (RDNA4sBind){0};
        NSDictionary *graph = @{ @"argument_resource_tables": @{ @"version": @2,
            @"table_binding": @672, @"categories": @[@0] }, @"bindings": @[] };
        checkMetadata(f, 0, graph, YES); checkMetadata(f, 1, graph, YES);
        for (id categories in @[@[], @[@0, @0], @[@2, @0], @[@3], @[@YES], @[@0.5], @"0", [NSNull null]]) {
            checkMetadata(f, 0, @{ @"argument_resource_tables": @{ @"version": @2,
                @"table_binding": @672, @"categories": categories }, @"bindings": @[] }, NO);
        }
        checkMetadata(f, 0, @{ @"argument_resource_tables": @{ @"version": @2,
            @"table_binding": @672, @"categories": @[@0], @"roots": @[] }, @"bindings": @[] }, NO);
        checkMetadata(f, 0, @{ @"argument_resource_tables": graph[@"argument_resource_tables"],
            @"bindings": metadata(@NO)[@"bindings"] }, NO);
        f->bind[0][707] = v->bind[0][707]; checkMetadata(f, 0, graph, NO);

        vkCreateDescriptorSetLayout = createLayout;
        vkGetDescriptorSetLayoutSupport = layoutSupport;
        setLimits();
        v->bind[0][32] = (RDNA4sBind){.used = 1, .type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .count = 5};
        v->bind[0][160] = (RDNA4sBind){.used = 1, .type = VK_DESCRIPTOR_TYPE_SAMPLER, .count = 2};
        v->bind[3][0] = (RDNA4sBind){.used = 1, .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .runtime = 1};
        v->bind[4][0] = (RDNA4sBind){.used = 1, .type = VK_DESCRIPTOR_TYPE_SAMPLER, .runtime = 1};
        v->argumentNullImages = (1u << 2) | (1u << 10);
        RDNA4ArgumentTables tables = {0}; error = nil;
        resetLayoutStages();
        require(rdna4_argument_table_layouts(&tables, v, NULL, YES, &error) && !error && tables.active, "compute layouts");
        require(layoutStageMask == 7u &&
            layoutStages[0] == VK_SHADER_STAGE_COMPUTE_BIT &&
            layoutStages[1] == VK_SHADER_STAGE_COMPUTE_BIT &&
            layoutStages[2] == VK_SHADER_STAGE_COMPUTE_BIT,
            "compute table bindings use only the compute stage bit");
        require(creates == 3 && queries == 3 && tables.capacity[0] > 256 && tables.capacity[1] > 256 &&
            tables.capacity[2] == 94 && tables.nullImages == v->argumentNullImages &&
            (uint64_t)tables.capacity[0] + tables.capacity[1] == 999993,
            "backend UAB resource budget excludes sampler descriptors");
        uint32_t tokens = 0, descriptors = 0;
        require(rdna4_argument_counts(&tables, 0, 5, &tokens, &descriptors) && tokens == 6 && descriptors == 17,
            "sampled token and reflected-shape descriptor counts");
        require(rdna4_argument_counts(&tables, 1, 5, &tokens, &descriptors) && tokens == 6 && descriptors == 6,
            "storage token descriptors gain no shape slots");
        tables.comparisonSamplers = YES;
        require(rdna4_argument_counts(&tables, 2, 5, &tokens, &descriptors) && tokens == 6 && descriptors == 12,
            "comparison descriptors preserve logical token count and use a second sampler region");
        require(rdna4_argument_counts(&tables, 2, UINT32_MAX / 2 - 1, &tokens, &descriptors) &&
            descriptors == UINT32_MAX - 1, "last legal dual-use sampler descriptor count");
        require(!rdna4_argument_counts(&tables, 2, UINT32_MAX / 2, &tokens, &descriptors),
            "dual-use sampler descriptor count overflow rejects");
        tables.comparisonSamplers = NO;
        RDNA4ArgumentTables counted = { .nullImages = 1u << 15 };
        require(rdna4_argument_counts(&counted, 0, 5, &tokens, &descriptors) && descriptors == 22,
            "highest reflected shape determines the sparse tail size");
        counted.nullImages = 1u << 5;
        require(!rdna4_argument_counts(&counted, 0, 5, NULL, NULL), "3D-array shape is rejected");
        counted.nullImages = 1ull << 48;
        require(!rdna4_argument_counts(&counted, 0, 5, NULL, NULL), "shape outside ABI 5 is rejected");
        counted.nullImages = (1ull << 18) | (1ull << 34);
        require(rdna4_argument_counts(&counted, 0, 5, &tokens, &descriptors) && descriptors == 41,
            "uint and int shapes have distinct descriptor offsets");
        counted.nullImages = 1ull << 26;
        require(!rdna4_argument_counts(&counted, 0, 5, NULL, NULL), "integer depth shape is rejected");
        static const VkImageViewType shapeViews[16] = {
            VK_IMAGE_VIEW_TYPE_1D, VK_IMAGE_VIEW_TYPE_1D_ARRAY, VK_IMAGE_VIEW_TYPE_2D,
            VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_IMAGE_VIEW_TYPE_3D, VK_IMAGE_VIEW_TYPE_3D,
            VK_IMAGE_VIEW_TYPE_CUBE, VK_IMAGE_VIEW_TYPE_CUBE_ARRAY, VK_IMAGE_VIEW_TYPE_1D,
            VK_IMAGE_VIEW_TYPE_1D_ARRAY, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D_ARRAY,
            VK_IMAGE_VIEW_TYPE_3D, VK_IMAGE_VIEW_TYPE_3D, VK_IMAGE_VIEW_TYPE_CUBE,
            VK_IMAGE_VIEW_TYPE_CUBE_ARRAY };
        for (unsigned shape = 0; shape < RDNA4_ARGUMENT_NULL_IMAGES; ++shape) {
            VkImageViewType viewType = 0; BOOL depth = NO;
            BOOL legal = (shape & 7) != 5 && (shape < 16 || !(shape & 8));
            require(rdna4_argument_shape(shape, &viewType, &depth) == legal &&
                (!legal || (viewType == shapeViews[shape & 15] && depth == ((shape & 8) != 0))), "shape mapping");
        }
        layoutLimit = 1000000; error = nil;
        *f = *v;
        resetLayoutStages(); tables = (RDNA4ArgumentTables){0};
        require(rdna4_argument_table_layouts(&tables, v, NULL, NO, &error) &&
            layoutStageMask == 7u && layoutStages[0] == VK_SHADER_STAGE_VERTEX_BIT &&
            layoutStages[1] == VK_SHADER_STAGE_VERTEX_BIT &&
            layoutStages[2] == VK_SHADER_STAGE_VERTEX_BIT,
            "vertex-only table bindings use the vertex stage bit");
        resetLayoutStages(); tables = (RDNA4ArgumentTables){0};
        require(rdna4_argument_table_layouts(&tables, NULL, v, NO, &error) &&
            layoutStageMask == 7u && layoutStages[0] == VK_SHADER_STAGE_FRAGMENT_BIT &&
            layoutStages[1] == VK_SHADER_STAGE_FRAGMENT_BIT &&
            layoutStages[2] == VK_SHADER_STAGE_FRAGMENT_BIT,
            "fragment-only table bindings use the fragment stage bit");
        f->bind[1][160].used = 1; f->bind[1][160].type = VK_DESCRIPTOR_TYPE_SAMPLER; f->bind[1][160].count = 4;
        f->bind[1][32] = (RDNA4sBind){.used = 1, .type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .count = 7};
        resetLayoutStages(); tables = (RDNA4ArgumentTables){0};
        require(rdna4_argument_table_layouts(&tables, v, f, NO, &error) &&
            tables.capacity[2] == 90 &&
            layoutStageMask == 7u &&
            layoutStages[0] == (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT) &&
            layoutStages[1] == (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT) &&
            layoutStages[2] == (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT),
            "mixed stage ordinary descriptors count against UAB totals");
        layoutLimit = 512; tables = (RDNA4ArgumentTables){0};
        require(rdna4_argument_table_layouts(&tables, v, NULL, YES, &error) && tables.capacity[0] == 512 && tables.capacity[1] == 512,
            "layout-specific variable descriptor limit");
        layoutLimit = 16; tables = (RDNA4ArgumentTables){0};
        require(rdna4_argument_table_layouts(&tables, v, NULL, YES, &error) && !tables.variable[2] && tables.capacity[2] == 94,
            "fixed sampler array preserves supported UAB capacity");
        fixedLayoutSupported = NO; tables = (RDNA4ArgumentTables){0};
        require(rdna4_argument_table_layouts(&tables, v, NULL, YES, &error) && tables.variable[2] && tables.capacity[2] == 16,
            "unsupported fixed sampler layout retains checked variable limit");
        v->bind[3][0].used = 0; v->bind[4][0].used = 0;
        tables = (RDNA4ArgumentTables){0};
        require(rdna4_argument_table_layouts(&tables, v, NULL, YES, &error) && !tables.capacity[1] && !tables.capacity[2] &&
            tables.dsl[1] && tables.dsl[2], "empty categories preserve shared set positions");
        unsigned before = creates;
        layoutSupported = NO; tables = (RDNA4ArgumentTables){0};
        require(!rdna4_argument_table_layouts(&tables, v, NULL, YES, &error) && error && creates == before,
            "unsupported layout rejected before creation");
        layoutSupported = YES; error = nil; RDNA4R.descriptorLimits.maxPerStageUpdateAfterBindResources = 1;
        tables = (RDNA4ArgumentTables){0};
        require(!rdna4_argument_table_layouts(&tables, v, NULL, YES, &error) && error && creates == before,
            "exhausted aggregate limit rejected");

        setLimits();
        fixedLayoutSupported = YES;
        layoutSupported = YES;
        layoutLimit = 1000;
        error = nil;
        RDNA4sRefl *q = calloc(1, sizeof *q);
        require(q != NULL, "capacity reflection allocation");
        q->maxset = 4;
        q->bind[0][32] = (RDNA4sBind){.used = 1, .type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .count = 1};
        q->bind[0][160] = (RDNA4sBind){.used = 1, .type = VK_DESCRIPTOR_TYPE_SAMPLER, .count = 1};
        q->bind[2][0] = (RDNA4sBind){.used = 1, .type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .runtime = 1};
        q->bind[3][0] = (RDNA4sBind){.used = 1, .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .runtime = 1};
        q->bind[4][0] = (RDNA4sBind){.used = 1, .type = VK_DESCRIPTOR_TYPE_SAMPLER, .runtime = 1};
        rdna4_argument_capacity_limits capacityLimits = {
            .max_per_stage_uab = {512, 512, 512},
            .max_pipeline_uab = {512, 512, 512},
            .max_per_stage_uab_resources = 10000,
            .max_per_stage_ordinary = {512, 512, 512},
            .max_pipeline_ordinary = {512, 512, 512},
            .max_per_stage_ordinary_resources = 10000,
            .max_per_set_descriptors = 32,
            .max_bound_descriptor_sets = 8,
        };
        tables = (RDNA4ArgumentTables){0};
        require(rdna4_argument_table_layouts_limits(&tables, q, NULL, YES,
                    &capacityLimits, &error) &&
                tables.capacity[0] == 511 && tables.capacity[1] == 512 &&
                tables.capacity[2] == 511,
            "advisory maxPerSetDescriptors does not clamp argument layouts");

        capacityLimits.max_per_set_descriptors = 0;
        tables = (RDNA4ArgumentTables){0};
        before = creates;
        require(!rdna4_argument_table_layouts_limits(&tables, q, NULL, YES,
                    &capacityLimits, &error) && error && creates == before,
            "unqueried maxPerSetDescriptors is rejected before layout creation");
        capacityLimits.max_per_set_descriptors = 32;

        capacityLimits.max_bound_descriptor_sets = 4;
        tables = (RDNA4ArgumentTables){0};
        before = creates;
        require(!rdna4_argument_table_layouts_limits(&tables, q, NULL, YES,
                    &capacityLimits, &error) && error && creates == before,
            "maxBoundDescriptorSets is checked before layout creation");
        capacityLimits.max_bound_descriptor_sets = 8;

        unsigned createBefore = createAttempts;
        unsigned layoutsBefore = creates;
        unsigned destroyedBefore = destroyedLayouts;
        RDNA4R.ok = YES;
        vkDestroyDescriptorSetLayout = destroyLayout;
        createFailAt = createBefore + 2;
        tables = (RDNA4ArgumentTables){0};
        error = nil;
        require(!rdna4_argument_table_layouts_limits(&tables, q, NULL, YES,
                    &capacityLimits, &error) && error &&
                creates == layoutsBefore + 1 && destroyedLayouts == destroyedBefore + 1 &&
                !tables.dsl[0] && !tables.dsl[1] && !tables.dsl[2],
            "partial layout creation rolls back existing layouts");

        RDNA4ArgumentTables owned = {
            .dsl = {(VkDescriptorSetLayout)(uintptr_t)9001,
                    (VkDescriptorSetLayout)(uintptr_t)9002, VK_NULL_HANDLE},
            .active = YES,
        };
        destroyedBefore = destroyedLayouts;
        rdna4_argument_tables_destroy(&owned);
        rdna4_argument_tables_destroy(&owned);
        require(destroyedLayouts == destroyedBefore + 2 && !owned.dsl[0] &&
                !owned.dsl[1] && !owned.dsl[2],
            "destroy is idempotent and clears partial layout handles");
        RDNA4R.ok = NO;
        vkDestroyDescriptorSetLayout = NULL;
        free(q);

        NSObject *sampled = [NSObject new], *storage = [NSObject new];
        RDNA4StageState state = {0};
        state.tex[0] = (RDNA4Texture *)sampled; state.tex[1] = (RDNA4Texture *)storage;
        RDNA4PB bindings[] = {
            {.binding = 32, .type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .count = 1, .gpuArguments = YES},
            {.binding = 481, .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .count = 1, .gpuArguments = YES} };
        __block unsigned visits = 0;
        rdna4_each_tex(bindings, 2, &state, nil, ^(RDNA4Texture *texture, VkImageLayout layout, BOOL writable) {
            require(layout == VK_IMAGE_LAYOUT_GENERAL && writable == (visits == 1) &&
                (id)texture == (visits ? storage : sampled), "GENERAL sampled image must remain read-only");
            visits++;
        });
        require(visits == 2, "texture callback coverage");

        *f = (RDNA4sRefl){0};
        f->bind[0][672] = (RDNA4sBind){.used = 1, .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .count = 1};
        f->bind[0][673] = f->bind[0][672];
        f->bind[1][672] = f->bind[0][672]; f->bind[1][673] = f->bind[0][673]; f->maxset = 1;
        NSDictionary *queryGraph = @{ @"argument_resource_tables": @{ @"version": @3,
            @"table_binding": @672, @"image_query_binding": @673, @"categories": @[] }, @"bindings": @[] };
        checkMetadata(f, 0, queryGraph, YES); checkMetadata(f, 1, queryGraph, YES);
        NSDictionary *queryGraph4 = @{ @"argument_resource_tables": @{ @"version": @4,
            @"table_binding": @672, @"image_query_binding": @673, @"categories": @[] }, @"bindings": @[] };
        checkMetadata(f, 0, queryGraph4, YES);
        checkMetadata(f, 0, @{ @"argument_resource_tables": @{ @"version": @4,
            @"table_binding": @672, @"categories": @[] }, @"bindings": @[] }, NO);
        for (id value in @[@672, @YES, @673.5, @"673", [NSNull null]])
            checkMetadata(f, 0, @{ @"argument_resource_tables": @{ @"version": @3,
                @"table_binding": @672, @"image_query_binding": value, @"categories": @[] }, @"bindings": @[] }, NO);
        f->bind[0][673].count = 2; checkMetadata(f, 0, queryGraph, NO); f->bind[0][673].count = 1;
        checkMetadata(f, 0, graph, NO);
        setLimits(); error = nil; tables = (RDNA4ArgumentTables){0};
        unsigned queryBefore = queries;
        require(rdna4_argument_table_layouts(&tables, f, NULL, YES, &error) && !error && tables.active &&
            tables.imageQueries && !tables.capacity[0] && !tables.capacity[1] && !tables.capacity[2] &&
            queries == queryBefore, "query-only tables need no image descriptor capacity");
        uint32_t dimensions[8];
        require(rdna4_argument_image_dimensions(nil, dimensions), "null dimension row");
        for (unsigned i = 0; i < 8; ++i) require(dimensions[i] == 0, "null dimension values");
        RDNA4QueryDimensionsProbe *probe = [RDNA4QueryDimensionsProbe new];
        probe.width = 16; probe.height = 8; probe.depth = 4; probe.arrayLength = 2;
        probe.mipmapLevelCount = 3; probe.sampleCount = 1;
        require(rdna4_argument_image_dimensions((id)probe, dimensions) && dimensions[0] == 16 &&
            dimensions[1] == 8 && dimensions[2] == 4 && dimensions[3] == 2 && dimensions[4] == 3 &&
            dimensions[5] == 1 && !dimensions[6] && !dimensions[7], "exact image dimensions and reserved words");
        probe.width = NSUIntegerMax;
        require(!rdna4_argument_image_dimensions((id)probe, dimensions), "dimension overflow is rejected");
        NSArray *rows = @[@[@[@0x5244000011223344ULL, [NSNull null]]], @[],
            @[@[@0x52440000aabbccddULL, [NSNull null]]]];
        NSData *bytes = rdna4_argument_table_bytes(rows, NO, 2.5f);
        static const uint32_t expected[] = {
            7, 2, 11, 1, 13, 2, 0x40200000,
            0, 0, 0x11223344, 0x52440000,
            0, 0, 0, 0, 0xaabbccdd, 0x52440000 };
        require(bytes.length == sizeof expected && !memcmp(bytes.bytes, expected, sizeof expected),
            "ABI 6/7 exact header, float bits, first token at word 7 and full 64-bit token bytes");
        RDNA4R.lim.maxStorageBufferRange = sizeof expected - 1;
        require(rdna4_argument_table_bytes(rows, NO, 2.5f) == nil,
            "table bytes beyond maxStorageBufferRange reject");
        RDNA4R.lim.maxStorageBufferRange = 0;
        require(rdna4_argument_table_bytes(rows, NO, 2.5f) == nil,
            "unqueried maxStorageBufferRange rejects");
        RDNA4R.lim.maxStorageBufferRange = UINT32_MAX;
        const float invalidLimits[] = {-1.0f, -INFINITY, INFINITY, NAN};
        const float validLimits[] = {0.0f, -0.0f, 2.5f, 15.9989996f};
        for (unsigned coordinates = 0; coordinates < 2; ++coordinates) {
            NSArray *flags = coordinates ? @[@0, @1] : nil;
            for (unsigned i = 0; i < sizeof invalidLimits / sizeof invalidLimits[0]; ++i)
                require(rdna4_argument_table_bytes_abi(rows, NO, invalidLimits[i], coordinates, flags) == nil,
                    "legacy and coordinate tables reject nonfinite or negative bias limits");
            for (unsigned i = 0; i < sizeof validLimits / sizeof validLimits[0]; ++i) {
                uint32_t bits;
                memcpy(&bits, validLimits + i, sizeof bits);
                NSData *table = rdna4_argument_table_bytes_abi(rows, NO, validLimits[i], coordinates, flags);
                require(table.length >= 7 * sizeof(uint32_t) && ((const uint32_t *)table.bytes)[6] == bits,
                    "finite nonnegative bias limits preserve exact word-6 bits, including signed zero");
            }
        }
        NSArray *texelRows = @[
            @[@[@0x1111222233334444ULL, [NSNull null]]],
            @[@[@0xaaaabbbbccccddddULL, [NSNull null]]] ];
        const BOOL dynamicTexel[2] = {YES, YES};
        NSData *abi10 = rdna4_argument_table_bytes_abi10(rows, texelRows,
            dynamicTexel, NO, 2.5f, YES, @[@0, @1]);
        static const uint32_t expectedAbi10[] = {
            11, 2, 15, 1, 17, 2, 0x40200000, 23, 2, 27, 2,
            0, 0, 0x11223344, 0x52440000,
            0, 0,
            0, 0, 0xaabbccdd, 0x52440000,
            0, 1,
            0, 0, 0x33334444, 0x11112222,
            0, 0, 0xccccdddd, 0xaaaabbbb };
        require(abi10.length == sizeof expectedAbi10 &&
                !memcmp(abi10.bytes, expectedAbi10, sizeof expectedAbi10),
            "ABI 10 preserves words 0..6 and appends exact typed texel token regions");
        RDNA4R.lim.maxStorageBufferRange = sizeof expectedAbi10 - 1;
        require(rdna4_argument_table_bytes_abi10(rows, texelRows, dynamicTexel,
                    NO, 2.5f, YES, @[@0, @1]) == nil,
            "ABI 10 texel regions obey maxStorageBufferRange");
        RDNA4R.lim.maxStorageBufferRange = UINT32_MAX;
        free(v); free(f);
        printf("ARGUMENT_TABLES_PASS checks=%u layouts=%u queries=%u simulated_vulkan=1 gpu_execution=0\n", checks, creates, queries);
    }
    return 0;
}
