// Exercise production SPIR-V reflection and sampler metadata gates on captured samples.
// Vulkan is mocked; no backend device or GPU command is created.
#include "GarnetDevice.m"
#import <objc/runtime.h>

@interface StageMetadataBufferProbe : NSObject
@end
@implementation StageMetadataBufferProbe
- (VkBuffer)vkBuffer { return (VkBuffer)(uintptr_t)0xf001; }
- (NSUInteger)rdna4VkOffset { return 64; }
@end

@interface StageMetadataUploadProbe : NSObject
@property NSData *uploaded;
@end
@implementation StageMetadataUploadProbe
- (id)device { return nil; }
- (BOOL)rdna4Bytes:(const void *)bytes length:(NSUInteger)length
    buffer:(RDNA4Buffer * __strong *)buffer offset:(NSUInteger *)offset {
    self.uploaded = [NSData dataWithBytes:bytes length:length];
    *buffer = (id)[StageMetadataBufferProbe new];
    *offset = 256;
    return YES;
}
@end

typedef struct {
    uint32_t binding;
    VkDescriptorType type;
    uint32_t count;
    BOOL comparisonSampler;
    BOOL gpuArguments;
    BOOL argumentTable;
    BOOL argumentRoot;
    BOOL addressTable;
    BOOL stageResourceMetadata;
    BOOL hasStaticSampler;
    uint32_t embeddedBuffer;
} MetadataExpectedBinding;

typedef struct {
    const char *name;
    int maxSet;
    uint32_t descriptorFeatures;
    BOOL gpuArguments;
    BOOL bufferAddress;
    BOOL hasTable;
    BOOL tableComparison;
    BOOL hasDref;
    unsigned samplerCreates;
    const MetadataExpectedBinding *bindings;
    uint32_t bindingCount;
} MetadataSampleCase;

static const MetadataExpectedBinding kDirectPairBindings[] = {
    { .binding = 0, .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .count = 1 },
    { .binding = 32, .type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .count = 1 },
    { .binding = 33, .type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .count = 1 },
    { .binding = 160, .type = VK_DESCRIPTOR_TYPE_SAMPLER, .count = 1 },
    { .binding = 736, .type = VK_DESCRIPTOR_TYPE_SAMPLER, .count = 1,
        .comparisonSampler = YES },
};
static const MetadataExpectedBinding kStaticCompareBindings[] = {
    { .binding = 0, .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .count = 1 },
    { .binding = 33, .type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .count = 1 },
    { .binding = 736, .type = VK_DESCRIPTOR_TYPE_SAMPLER, .count = 1,
        .comparisonSampler = YES, .hasStaticSampler = YES },
};
static const MetadataExpectedBinding kGpuNormalBindings[] = {
    { .binding = 0, .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .count = 1,
        .gpuArguments = YES },
    { .binding = 3, .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .count = 1,
        .gpuArguments = YES },
    { .binding = 640, .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .count = 1,
        .gpuArguments = YES, .addressTable = YES },
    { .binding = 672, .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .count = 1,
        .gpuArguments = YES, .argumentTable = YES },
    { .binding = 674, .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .count = 1,
        .gpuArguments = YES, .stageResourceMetadata = YES },
};
static const MetadataExpectedBinding kGpuPortablePairBindings[] = {
    { .binding = 0, .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .count = 1,
        .gpuArguments = YES },
    { .binding = 3, .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .count = 1,
        .gpuArguments = YES },
    { .binding = 640, .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .count = 1,
        .gpuArguments = YES, .addressTable = YES },
    { .binding = 672, .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .count = 1,
        .gpuArguments = YES, .argumentTable = YES },
    { .binding = 674, .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .count = 1,
        .gpuArguments = YES, .stageResourceMetadata = YES },
    { .binding = 707, .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .count = 1,
        .gpuArguments = YES, .argumentRoot = YES, .embeddedBuffer = 3 },
};

static const MetadataSampleCase kSamples[] = {
    { .name = "direct-pair", .maxSet = 0, .descriptorFeatures = 0,
        .hasDref = YES, .bindings = kDirectPairBindings,
        .bindingCount = sizeof kDirectPairBindings / sizeof kDirectPairBindings[0] },
    { .name = "static-compare-only", .maxSet = 0, .descriptorFeatures = 0,
        .hasDref = YES, .samplerCreates = 1,
        .bindings = kStaticCompareBindings,
        .bindingCount = sizeof kStaticCompareBindings /
            sizeof kStaticCompareBindings[0] },
    { .name = "gpu-normal-only", .maxSet = 4,
        .descriptorFeatures = RDNA4S_NONUNIFORM_SAMPLED | RDNA4S_RUNTIME_ARRAY,
        .gpuArguments = YES, .bufferAddress = YES, .hasTable = YES,
        .tableComparison = NO, .bindings = kGpuNormalBindings,
        .bindingCount = sizeof kGpuNormalBindings / sizeof kGpuNormalBindings[0] },
    { .name = "gpu-mixed", .maxSet = 4,
        .descriptorFeatures = RDNA4S_NONUNIFORM_SAMPLED | RDNA4S_RUNTIME_ARRAY,
        .gpuArguments = YES, .bufferAddress = YES, .hasTable = YES, .tableComparison = YES,
        .hasDref = YES, .bindings = kGpuNormalBindings,
        .bindingCount = sizeof kGpuNormalBindings / sizeof kGpuNormalBindings[0] },
    { .name = "gpu-compare-only", .maxSet = 4,
        .descriptorFeatures = RDNA4S_NONUNIFORM_SAMPLED | RDNA4S_RUNTIME_ARRAY,
        .gpuArguments = YES, .bufferAddress = YES, .hasTable = YES, .tableComparison = YES,
        .hasDref = YES, .bindings = kGpuNormalBindings,
        .bindingCount = sizeof kGpuNormalBindings / sizeof kGpuNormalBindings[0] },
};
static const MetadataSampleCase kGpuPortablePair = {
    .name = "gpu-portable-embedded-pair", .maxSet = 4,
    .descriptorFeatures = RDNA4S_NONUNIFORM_SAMPLED | RDNA4S_RUNTIME_ARRAY,
    .gpuArguments = YES, .bufferAddress = YES, .hasTable = YES, .tableComparison = YES,
    .hasDref = YES, .bindings = kGpuPortablePairBindings,
    .bindingCount = sizeof kGpuPortablePairBindings /
        sizeof kGpuPortablePairBindings[0],
};

static unsigned checks;
static unsigned layoutAttempts;
static unsigned samplerAttempts;
static unsigned destroyedSamplers;
static unsigned expectedSamplerCreates;
static const MetadataExpectedBinding *expectedBindings;
static uint32_t expectedBindingCount;

static void require(BOOL ok, NSString *message) {
    checks++;
    if (!ok) {
        fprintf(stderr, "SAMPLER_METADATA_FAIL %s\n", message.UTF8String);
        exit(1);
    }
}

static unsigned stageMetadataWrites;
static void VKAPI_CALL captureStageMetadata(VkDevice device, uint32_t count,
    const VkWriteDescriptorSet *writes, uint32_t copies, const VkCopyDescriptorSet *copy) {
    (void)device; (void)copy;
    require(count == 1 && !copies && writes[0].dstBinding == 674 &&
        writes[0].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER &&
        writes[0].descriptorCount == 1, @"stage metadata descriptor bytes");
    require(writes[0].pBufferInfo && writes[0].pBufferInfo->buffer == (VkBuffer)(uintptr_t)0xf001 &&
        writes[0].pBufferInfo->offset == 320 && writes[0].pBufferInfo->range == 384,
        @"stage metadata descriptor offset and exact 384-byte range");
    ++stageMetadataWrites;
}

static void testStageMetadataBytes(void) {
    StageMetadataUploadProbe *probe = [StageMetadataUploadProbe new];
    RDNA4PB binding = {.binding=674, .type=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .count=1,
        .samplerCoordinateFlags=YES, .stageResourceMetadata=YES, .stageTokenTable=YES,
        .nullShapeCount=3, .nullShapeRemap={1, 2, 0}};
    RDNA4StageState state = {0};
    vkUpdateDescriptorSets = captureStageMetadata;
    SEL selector = @selector(rdna4FillSet:bindings:count:state:stage:);
    BOOL (*fill)(id, SEL, VkDescriptorSet, const RDNA4PB *, uint32_t,
        const RDNA4StageState *, const char *) =
        (void *)class_getMethodImplementation(RDNA4CommandBuffer.class, selector);
    require(fill(probe, selector, (VkDescriptorSet)(uintptr_t)0xf002, &binding, 1, &state, "metadata-test"),
        @"production stage metadata upload");
    require(stageMetadataWrites == 1 && probe.uploaded.length == 96 * sizeof(uint32_t),
        @"stage metadata upload byte count");
    const uint32_t *words = probe.uploaded.bytes;
    for (unsigned i = 0; i < 96; ++i) {
        uint32_t expected = i < 32 ? 0 : i == 32 ? 1 : i == 33 ? 2 : i == 34 ? 0 : UINT32_MAX;
        require(words[i] == expected, [NSString stringWithFormat:@"stage metadata word %u", i]);
    }
    vkUpdateDescriptorSets = NULL;
}

static VkResult VKAPI_CALL createLayout(VkDevice device,
    const VkDescriptorSetLayoutCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkDescriptorSetLayout *layout) {
    (void)device;
    (void)allocator;
    layoutAttempts++;
    require(info != NULL && layout != NULL, @"createLayout null argument");
    require(expectedBindings != NULL,
        @"createLayout called without an expected binding table");
    require(info->sType ==
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        @"descriptor layout sType");
    require(info->pNext == NULL && info->flags == 0,
        @"descriptor layout flags changed");
    require(info->bindingCount == expectedBindingCount,
        @"descriptor layout binding count");
    require(info->bindingCount > 0 && info->pBindings != NULL,
        @"descriptor layout binding array");
    for (uint32_t i = 0; i < expectedBindingCount; ++i) {
        const MetadataExpectedBinding *expected = &expectedBindings[i];
        const VkDescriptorSetLayoutBinding *actual = &info->pBindings[i];
        require(actual->binding == expected->binding,
            [NSString stringWithFormat:@"layout binding %u number", i]);
        require(actual->descriptorType == expected->type,
            [NSString stringWithFormat:@"layout binding %u type", i]);
        require(actual->descriptorCount == expected->count,
            [NSString stringWithFormat:@"layout binding %u count", i]);
        require(actual->stageFlags == VK_SHADER_STAGE_COMPUTE_BIT,
            [NSString stringWithFormat:@"layout binding %u stage", i]);
        require(actual->pImmutableSamplers == NULL,
            [NSString stringWithFormat:@"layout binding %u immutable samplers", i]);
    }
    *layout = (VkDescriptorSetLayout)(uintptr_t)(0x4000u + layoutAttempts);
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL createSampler(VkDevice device,
    const VkSamplerCreateInfo *info, const VkAllocationCallbacks *allocator,
    VkSampler *sampler) {
    (void)device;
    (void)allocator;
    samplerAttempts++;
    require(info != NULL && sampler != NULL, @"createSampler null argument");
    require(expectedSamplerCreates > 0,
        @"createSampler called for non-static metadata");
    require(info->sType == VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        @"static sampler sType");
    require(info->compareEnable == VK_TRUE &&
        info->compareOp == VK_COMPARE_OP_NEVER,
        @"static comparison sampler is not compare=Never");
    require(info->minFilter == VK_FILTER_NEAREST &&
        info->magFilter == VK_FILTER_NEAREST &&
        info->mipmapMode == VK_SAMPLER_MIPMAP_MODE_NEAREST,
        @"static comparison sampler filters");
    require(info->addressModeU == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE &&
        info->addressModeV == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE &&
        info->addressModeW == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        @"static comparison sampler address modes");
    require(info->anisotropyEnable == VK_FALSE &&
        info->maxAnisotropy == 1.0f && info->unnormalizedCoordinates == VK_FALSE,
        @"static comparison sampler anisotropy/coordinates");
    *sampler = (VkSampler)(uintptr_t)(0x5000u + samplerAttempts);
    return VK_SUCCESS;
}

static void VKAPI_CALL destroySampler(VkDevice device, VkSampler sampler,
                                      const VkAllocationCallbacks *allocator) {
    (void)device;
    (void)allocator;
    require(sampler != VK_NULL_HANDLE, @"destroySampler null handle");
    destroyedSamplers++;
}

static void configureRuntime(const MetadataSampleCase *sample) {
    layoutAttempts = samplerAttempts = destroyedSamplers = 0;
    expectedSamplerCreates = sample->samplerCreates;
    expectedBindings = sample->bindings;
    expectedBindingCount = sample->bindingCount;
    RDNA4R.ok = YES;
    RDNA4R.dev = (VkDevice)(uintptr_t)0x1234;
    RDNA4R.gpuArgumentTables = sample->gpuArguments;
    RDNA4R.bufferAddress = sample->bufferAddress;
    RDNA4R.nonUniformSampledImages = YES;
    RDNA4R.nonUniformStorageImages = YES;
    RDNA4R.nonUniformBuffers = YES;
    RDNA4R.samplerAnisotropy = YES;
    RDNA4R.samplerMirrorClamp = NO;
    RDNA4R.lim.maxSamplerAnisotropy = 16.0f;
    RDNA4R.lim.maxSamplerAllocationCount = 16;
    atomic_store(&rdna4_sampler_allocations, 0);
    vkCreateDescriptorSetLayout = createLayout;
    vkCreateSampler = createSampler;
    vkDestroySampler = destroySampler;
}

static NSDictionary *loadJSON(NSString *path) {
    NSError *error = nil;
    NSData *data = [NSData dataWithContentsOfFile:path options:0 error:&error];
    require(data.length > 0,
        error.localizedDescription ?:
            [NSString stringWithFormat:@"metadata unavailable: %@", path]);
    id value = [NSJSONSerialization JSONObjectWithData:data options:0
        error:&error];
    require([value isKindOfClass:NSDictionary.class],
        error.localizedDescription ?:
            [NSString stringWithFormat:@"metadata is not JSON: %@", path]);
    return value;
}

static uint32_t *loadSPV(NSString *path, size_t *wordCount) {
    NSError *error = nil;
    NSData *data = [NSData dataWithContentsOfFile:path options:0 error:&error];
    require(data.length >= 20 && data.length % sizeof(uint32_t) == 0,
        error.localizedDescription ?:
            [NSString stringWithFormat:@"SPIR-V size invalid: %@", path]);
    uint32_t *words = malloc(data.length);
    require(words != NULL, @"SPIR-V allocation");
    memcpy(words, data.bytes, data.length);
    *wordCount = data.length / sizeof(uint32_t);
    require(words[0] == 0x07230203u, @"SPIR-V magic");
    return words;
}

static BOOL spvHasDref(const uint32_t *words, size_t wordCount) {
    size_t cursor = 5;
    while (cursor < wordCount) {
        uint32_t instruction = words[cursor];
        uint32_t opcode = instruction & 0xffffu;
        uint32_t count = instruction >> 16;
        require(count > 0 && cursor + count <= wordCount,
            @"malformed SPIR-V instruction during Dref scan");
        // OpImageSampleDrefImplicitLod/ExplicitLod; opcode 88 is ordinary sampling.
        if (opcode == 89 || opcode == 90)
            return YES;
        cursor += count;
    }
    return NO;
}

static NSMutableDictionary *mutableMetadata(NSDictionary *source) {
    NSMutableDictionary *metadata = [source mutableCopy];
    NSMutableArray *rows = [NSMutableArray array];
    for (id row in source[@"bindings"])
        [rows addObject:[row isKindOfClass:NSDictionary.class] ?
            [row mutableCopy] : row];
    metadata[@"bindings"] = rows;
    if ([source[@"argument_resource_tables"] isKindOfClass:
            NSDictionary.class])
        metadata[@"argument_resource_tables"] =
            [source[@"argument_resource_tables"] mutableCopy];
    return metadata;
}

static NSMutableDictionary *portablePairMetadata(NSDictionary *source) {
    // The captured GPU samples predate the embedded portable-pair row. Derive
    // its metadata from gpu-mixed while retaining the captured SPIR-V/table ABI.
    NSMutableDictionary *metadata = mutableMetadata(source);
    NSMutableDictionary *table = metadata[@"argument_resource_tables"];
    table[@"roots"] = @[ @{ @"buffer_index": @3, @"binding": @707 } ];
    NSMutableDictionary *row = [@{
        @"kind": @"EmbeddedArgBufferSampler",
        @"descriptor": @{ @"set": @4, @"binding": @0, @"count": @0 },
        @"comparison_descriptor":
            @{ @"set": @4, @"binding": @0, @"count": @0 },
        @"embedded_source": @{ @"buffer_index": @3, @"field_offset": @16 },
    } mutableCopy];
    [metadata[@"bindings"] addObject:row];
    return metadata;
}

static NSMutableDictionary *pairRow(NSDictionary *metadata, NSString *kind) {
    for (NSMutableDictionary *row in metadata[@"bindings"])
        if ([row[@"kind"] isEqual:kind] && row[@"comparison_descriptor"])
            return row;
    require(NO, [NSString stringWithFormat:
        @"missing %@ comparison pair", kind]);
    return nil;
}

static NSMutableDictionary *mutablePair(NSDictionary *metadata, NSString *kind) {
    NSMutableDictionary *row = pairRow(metadata, kind);
    NSMutableDictionary *paired = [row[@"comparison_descriptor"] mutableCopy];
    row[@"comparison_descriptor"] = paired;
    return paired;
}

static void verifyBindings(const MetadataSampleCase *sample,
                           const RDNA4PB *bindings, uint32_t count) {
    require(count == sample->bindingCount,
        @"PB binding count differs from descriptor layout");
    for (uint32_t i = 0; i < count; ++i) {
        const MetadataExpectedBinding *expected = &sample->bindings[i];
        const RDNA4PB *actual = &bindings[i];
        require(actual->binding == expected->binding,
            [NSString stringWithFormat:@"PB binding %u number", i]);
        require(actual->type == expected->type,
            [NSString stringWithFormat:@"PB binding %u type", i]);
        require(actual->count == expected->count,
            [NSString stringWithFormat:@"PB binding %u count", i]);
        require(actual->comparisonSampler == expected->comparisonSampler,
            [NSString stringWithFormat:@"PB binding %u comparison flag", i]);
        require(actual->gpuArguments == expected->gpuArguments,
            [NSString stringWithFormat:@"PB binding %u GPU-arguments flag", i]);
        require(actual->argumentTable == expected->argumentTable,
            [NSString stringWithFormat:@"PB binding %u argument-table flag", i]);
        require(actual->argumentRoot == expected->argumentRoot,
            [NSString stringWithFormat:@"PB binding %u argument-root flag", i]);
        require(actual->addressTable == expected->addressTable,
            [NSString stringWithFormat:@"PB binding %u address-table flag", i]);
        require(actual->stageResourceMetadata == expected->stageResourceMetadata &&
            actual->samplerCoordinateFlags == expected->stageResourceMetadata &&
            actual->stageTokenTable == expected->stageResourceMetadata,
            [NSString stringWithFormat:@"PB binding %u stage-resource metadata flags", i]);
        require(!actual->embedded,
            [NSString stringWithFormat:@"PB binding %u unexpectedly embedded", i]);
        require(actual->embeddedBuffer == expected->embeddedBuffer,
            [NSString stringWithFormat:@"PB binding %u embedded buffer", i]);
        require((actual->stat != VK_NULL_HANDLE) ==
            expected->hasStaticSampler,
            [NSString stringWithFormat:@"PB binding %u static sampler", i]);
    }
}

static void runAccepted(const uint32_t *words, size_t wordCount,
                        NSDictionary *metadata,
                        const MetadataSampleCase *sample, BOOL addRoot) {
    char why[256] = {0};
    RDNA4sMod module = {0};
    require(rdna4s_parse(words, wordCount, &module, why, sizeof why) == 0,
        [NSString stringWithFormat:@"%s parse: %s", sample->name,
            why[0] ? why : "failed"]);
    require(module.has_ep && module.ep_model == 5,
        [NSString stringWithFormat:@"%s is not a kernel", sample->name]);
    require(module.descriptor_features == sample->descriptorFeatures,
        [NSString stringWithFormat:@"%s descriptor features", sample->name]);

    RDNA4sRefl *reflection = calloc(1, sizeof *reflection);
    require(reflection != NULL,
        [NSString stringWithFormat:@"%s reflection allocation", sample->name]);
    require(rdna4s_reflect(&module, 2, reflection) == 0,
        [NSString stringWithFormat:@"%s reflection: %s", sample->name,
            reflection->why]);
    require(reflection->maxset == sample->maxSet,
        [NSString stringWithFormat:@"%s descriptor-set max", sample->name]);
    if (addRoot) {
        reflection->bind[0][707] = (RDNA4sBind){
            .used = 1, .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .count = 1 };
    }
    require(spvHasDref(words, wordCount) == sample->hasDref,
        [NSString stringWithFormat:@"%s Dref operation", sample->name]);

    id table = metadata[@"argument_resource_tables"];
    require((table != nil) == sample->hasTable,
        [NSString stringWithFormat:@"%s table presence", sample->name]);
    if (sample->hasTable) {
        require([table isKindOfClass:NSDictionary.class] &&
            [table[@"version"] unsignedLongLongValue] == 9 &&
            [table[@"table_binding"] unsignedLongLongValue] == 672,
            [NSString stringWithFormat:@"%s table ABI", sample->name]);
        require([table[@"categories"] isEqual:@[@0, @2]],
            [NSString stringWithFormat:@"%s portable categories",
                sample->name]);
        require([table[@"comparison_sampler"] boolValue] ==
            sample->tableComparison,
            [NSString stringWithFormat:@"%s comparison-sampler flag",
                sample->name]);
    }

    configureRuntime(sample);
    VkDescriptorSetLayout layout = (VkDescriptorSetLayout)(uintptr_t)0xdead;
    RDNA4PB *bindings = NULL;
    uint32_t count = 0;
    NSError *error = nil;
    NSMutableArray<NSValue *> *ownedSamplers = [NSMutableArray array];
    BOOL ok = rdna4_stage_dsl(reflection, 0, VK_SHADER_STAGE_COMPUTE_BIT,
        metadata, &layout, &bindings, &count, ownedSamplers, &error);
    require(ok && error == nil,
        error.localizedDescription ?:
            [NSString stringWithFormat:@"%s stage metadata rejected",
                sample->name]);
    require(layoutAttempts == 1 &&
        layout == (VkDescriptorSetLayout)(uintptr_t)0x4001,
        [NSString stringWithFormat:@"%s layout creation count", sample->name]);
    require(samplerAttempts == sample->samplerCreates &&
        ownedSamplers.count == sample->samplerCreates,
        [NSString stringWithFormat:@"%s static sampler count", sample->name]);
    verifyBindings(sample, bindings, count);
    if (sample->samplerCreates) {
        VkSampler sampler = (VkSampler)[ownedSamplers[0] pointerValue];
        require(sampler != VK_NULL_HANDLE &&
            bindings[count - 1].stat == sampler,
            [NSString stringWithFormat:@"%s static sampler handle",
                sample->name]);
        rdna4_destroy_sampler(sampler, nil);
        require(destroyedSamplers == 1 &&
            atomic_load(&rdna4_sampler_allocations) == 0,
            [NSString stringWithFormat:@"%s static sampler teardown",
                sample->name]);
    } else {
        require(atomic_load(&rdna4_sampler_allocations) == 0,
            [NSString stringWithFormat:@"%s allocated an unused sampler",
                sample->name]);
    }
    printf("SAMPLER_METADATA_CASE_OK name=%s bindings=%u table=%d "
           "dref=%d static_samplers=%u\n", sample->name,
        (unsigned)count, sample->hasTable, sample->hasDref,
        samplerAttempts);

    free(bindings);
    free(reflection);
    rdna4s_free(&module);
}

static void runRejected(const char *name, const uint32_t *words,
                        size_t wordCount, NSDictionary *metadata,
                        const MetadataSampleCase *sample, BOOL addRoot) {
    char why[256] = {0};
    RDNA4sMod module = {0};
    require(rdna4s_parse(words, wordCount, &module, why, sizeof why) == 0,
        [NSString stringWithFormat:@"%s malformed parse: %s", name, why]);
    RDNA4sRefl *reflection = calloc(1, sizeof *reflection);
    require(reflection != NULL,
        [NSString stringWithFormat:@"%s reflection allocation", name]);
    require(rdna4s_reflect(&module, 2, reflection) == 0,
        [NSString stringWithFormat:@"%s malformed reflection", name]);
    if (addRoot) {
        reflection->bind[0][707] = (RDNA4sBind){
            .used = 1, .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .count = 1 };
    }

    configureRuntime(sample);
    VkDescriptorSetLayout layout = (VkDescriptorSetLayout)(uintptr_t)0xdead;
    RDNA4PB *bindings = NULL;
    uint32_t count = 0;
    NSError *error = nil;
    NSMutableArray<NSValue *> *ownedSamplers = [NSMutableArray array];
    BOOL ok = rdna4_stage_dsl(reflection, 0, VK_SHADER_STAGE_COMPUTE_BIT,
        metadata, &layout, &bindings, &count, ownedSamplers, &error);
    require(!ok && error != nil,
        [NSString stringWithFormat:@"%s malformed metadata accepted", name]);
    require(layout == (VkDescriptorSetLayout)(uintptr_t)0xdead &&
        layoutAttempts == 0 && samplerAttempts == 0 &&
        ownedSamplers.count == 0 && bindings == NULL && count == 0 &&
        atomic_load(&rdna4_sampler_allocations) == 0,
        [NSString stringWithFormat:@"%s reached Vulkan or leaked output",
            name]);
    printf("SAMPLER_METADATA_REJECT_OK name=%s vulkan_calls=0\n", name);

    free(bindings);
    free(reflection);
    rdna4s_free(&module);
}

static NSDictionary *loadActual(NSString *directory, const char *name,
                                uint32_t **words, size_t *wordCount) {
    NSString *base = [directory stringByAppendingPathComponent:
        [NSString stringWithUTF8String:name]];
    NSDictionary *metadata = loadJSON([base stringByAppendingPathExtension:
        @"json"]);
    *words = loadSPV([base stringByAppendingPathExtension:@"spv"], wordCount);
    return metadata;
}

int main(int argc, const char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        puts("Usage: test-sampler-metadata METADATA_SAMPLES_DIR");
        puts("Runs captured JSON/SPIR-V pairs through the production "
             "sampler metadata gate with mocked Vulkan.");
        return 0;
    }
    if (argc != 2) {
        fprintf(stderr, "usage: test-sampler-metadata METADATA_SAMPLES_DIR\n");
        return 2;
    }

    @autoreleasepool {
        setvbuf(stdout, NULL, _IONBF, 0);
        NSString *directory = [NSString stringWithUTF8String:argv[1]];
        NSFileManager *manager = [NSFileManager defaultManager];
        BOOL isDirectory = NO;
        require([manager fileExistsAtPath:directory isDirectory:&isDirectory] &&
            isDirectory, @"metadata samples directory unavailable");

        NSMutableDictionary<NSString *, NSDictionary *> *actual =
            [NSMutableDictionary dictionary];
        for (size_t i = 0; i < sizeof kSamples / sizeof kSamples[0]; ++i) {
            uint32_t *words = NULL;
            size_t wordCount = 0;
            NSDictionary *metadata = loadActual(directory, kSamples[i].name,
                &words, &wordCount);
            actual[[NSString stringWithUTF8String:kSamples[i].name]] = metadata;
            runAccepted(words, wordCount, metadata, &kSamples[i], NO);
            free(words);
        }

        uint32_t *gpuWords = NULL;
        size_t gpuWordCount = 0;
        NSDictionary *gpuMixed = actual[@"gpu-mixed"];
        NSMutableDictionary *portable = portablePairMetadata(gpuMixed);
        gpuWords = loadSPV([directory stringByAppendingPathComponent:
            @"gpu-mixed.spv"], &gpuWordCount);
        runAccepted(gpuWords, gpuWordCount, portable, &kGpuPortablePair, YES);

        uint32_t *directWords = NULL;
        size_t directWordCount = 0;
        NSDictionary *direct = actual[@"direct-pair"];
        directWords = loadSPV([directory stringByAppendingPathComponent:
            @"direct-pair.spv"], &directWordCount);
        for (unsigned i = 0; i < 3; ++i) {
            NSMutableDictionary *malformed = mutableMetadata(direct);
            NSMutableDictionary *paired = mutablePair(malformed, @"Sampler");
            const char *names[] = {
                "direct-pair-set", "direct-pair-binding",
                "direct-pair-count" };
            id values[] = { @1, @737, @2 };
            paired[i == 0 ? @"set" : i == 1 ? @"binding" : @"count"] =
                values[i];
            runRejected(names[i], directWords, directWordCount, malformed,
                &kSamples[0], NO);
        }
        free(directWords);

        for (unsigned i = 0; i < 3; ++i) {
            NSMutableDictionary *malformed = portablePairMetadata(gpuMixed);
            NSMutableDictionary *paired =
                mutablePair(malformed, @"EmbeddedArgBufferSampler");
            const char *names[] = {
                "gpu-portable-pair-set", "gpu-portable-pair-binding",
                "gpu-portable-pair-count" };
            id values[] = { @3, @1, @1 };
            paired[i == 0 ? @"set" : i == 1 ? @"binding" : @"count"] =
                values[i];
            runRejected(names[i], gpuWords, gpuWordCount, malformed,
                &kGpuPortablePair, YES);
        }

        for (unsigned i = 0; i < 4; ++i) {
            NSMutableDictionary *malformed = portablePairMetadata(gpuMixed);
            NSMutableDictionary *table =
                malformed[@"argument_resource_tables"];
            const char *names[] = {
                "gpu-table-version-v6", "gpu-table-version-v8",
                "gpu-table-version-string", "gpu-table-version-unknown" };
            id values[] = { @6, @8, @"9", @999 };
            table[@"version"] = values[i];
            runRejected(names[i], gpuWords, gpuWordCount, malformed,
                &kGpuPortablePair, YES);
        }

        for (unsigned i = 0; i < 4; ++i) {
            NSMutableDictionary *malformed = portablePairMetadata(gpuMixed);
            NSMutableDictionary *table =
                malformed[@"argument_resource_tables"];
            const char *names[] = {
                "gpu-table-bool-false", "gpu-table-bool-number",
                "gpu-table-bool-string", "gpu-table-bool-missing" };
            id values[] = { @NO, @1, @"true", nil };
            if (values[i]) table[@"comparison_sampler"] = values[i];
            else [table removeObjectForKey:@"comparison_sampler"];
            runRejected(names[i], gpuWords, gpuWordCount, malformed,
                &kGpuPortablePair, YES);
        }
        free(gpuWords);

        testStageMetadataBytes();
        printf("SAMPLER_METADATA_PASS actual_cases=5 "
               "gpu_portable_pair=1 direct_pair_strict=1 malformed_rejected=14 "
               "stage_metadata_words=96 checks=%u simulated_vulkan=1 gpu_execution=0\n", checks);
    }
    return 0;
}
