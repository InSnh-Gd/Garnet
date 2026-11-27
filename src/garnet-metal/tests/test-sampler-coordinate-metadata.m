// Exact ABI 8 sampler-coordinate metadata/table checks with mocked Vulkan.
#include "GarnetDevice.m"

@interface RDNA4CoordinateTextureProbe : NSObject
@property NSUInteger width, height, depth, arrayLength, mipmapLevelCount, sampleCount;
@property MTLTextureType textureType;
@property BOOL depthFormat, uintFormat, sintFormat;
@end
@implementation RDNA4CoordinateTextureProbe
- (BOOL)rdna4IsDS { return _depthFormat; }
- (BOOL)rdna4IsUInt { return _uintFormat; }
- (BOOL)rdna4IsSInt { return _sintFormat; }
@end

static unsigned checks, layoutCreates, samplerCreates, samplerDestroys;
static VkSamplerCreateInfo lastSampler;

static void require(BOOL ok, NSString *message) {
    ++checks;
    if (!ok) {
        fprintf(stderr, "SAMPLER_COORDINATE_METADATA_FAIL %s\n",
                message.UTF8String);
        exit(1);
    }
}

static VkResult VKAPI_CALL createLayout(VkDevice device,
    const VkDescriptorSetLayoutCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkDescriptorSetLayout *layout) {
    (void)device;
    (void)allocator;
    require(info && layout && info->bindingCount > 0,
        @"mock descriptor layout arguments");
    ++layoutCreates;
    *layout = (VkDescriptorSetLayout)(uintptr_t)(0x9000u + layoutCreates);
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL createSampler(VkDevice device,
    const VkSamplerCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkSampler *sampler) {
    (void)device;
    (void)allocator;
    require(info && sampler, @"mock sampler arguments");
    lastSampler = *info;
    *sampler = (VkSampler)(uintptr_t)(0xa000u + ++samplerCreates);
    return VK_SUCCESS;
}

static void VKAPI_CALL destroySampler(VkDevice device, VkSampler sampler,
                                      const VkAllocationCallbacks *allocator) {
    (void)device;
    (void)allocator;
    require(sampler != VK_NULL_HANDLE, @"mock sampler destroy");
    ++samplerDestroys;
}

static uint32_t word(NSData *data, NSUInteger index) {
    uint32_t value = 0;
    memcpy(&value, (const uint8_t *)data.bytes + index * sizeof value,
           sizeof value);
    return value;
}

static uint32_t floatWord(float value) {
    uint32_t bits = 0;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

static RDNA4CoordinateTextureProbe *textureProbe(MTLTextureType type,
    NSUInteger width, NSUInteger height, NSUInteger depth, NSUInteger arrayLength,
    NSUInteger levels, NSUInteger samples) {
    RDNA4CoordinateTextureProbe *probe = [RDNA4CoordinateTextureProbe new];
    probe.textureType = type;
    probe.width = width;
    probe.height = height;
    probe.depth = depth;
    probe.arrayLength = arrayLength;
    probe.mipmapLevelCount = levels;
    probe.sampleCount = samples;
    return probe;
}

static void testExactTableBytes(void) {
    RDNA4CoordinateTextureProbe *sample1 = textureProbe(
        (MTLTextureType)RDNA4TD_MTL_2D, 8, 8, 1, 1, 1, 1);
    RDNA4CoordinateTextureProbe *sample2 = textureProbe(
        (MTLTextureType)RDNA4TD_MTL_2D, 8, 8, 1, 1, 1, 1);
    NSArray *rows = @[
        @[
            @[ @(UINT64_C(0x11111111)), sample1 ],
            @[ @(UINT64_C(0x22222222)), sample2 ],
        ],
        @[ @[ @(UINT64_C(0x33333333)) ] ],
        @[
            @[ @(UINT64_C(0x44444444)) ],
            @[ @(UINT64_C(0x55555555)) ],
        ],
    ];
    NSArray<NSNumber *> *flags = @[ @0, @1, @0 ];
    float bias = 2.5f;
    NSMutableData *data = rdna4_argument_table_bytes_abi(rows, YES, bias, YES,
                                                          flags);
    require(data != nil, @"ABI 8 table serialization");
    require(data.length == 200, @"ABI 8 exact table byte length");
    require(word(data, 0) == 7 && word(data, 1) == 3,
        @"sampled offset/count");
    require(word(data, 2) == 13 && word(data, 3) == 2,
        @"storage offset/count");
    require(word(data, 4) == 17 && word(data, 5) == 3,
        @"sampler offset/count including dummy");
    require(word(data, 6) == floatWord(bias),
        @"maxSamplerLodBias float bits");
    require(word(data, 7) == 0 && word(data, 9) == UINT32_C(0x11111111) &&
        word(data, 11) == UINT32_C(0x22222222),
        @"sampled tokens start at word 7");
    require(word(data, 13) == 0 && word(data, 15) == UINT32_C(0x33333333),
        @"storage token list");
    require(word(data, 17) == 0 && word(data, 19) == UINT32_C(0x44444444) &&
        word(data, 21) == UINT32_C(0x55555555),
        @"sampler token list");
    require(word(data, 23) == 0 && word(data, 24) == 1 &&
        word(data, 25) == 0,
        @"flags immediately after sampler tokens, dummy zero");
    const uint32_t expectedQueryRows[] = {
        0, 0, 0, 0, 0, 0, 0, 0,
        8, 8, 1, 1, 1, 1, 2, 0,
        8, 8, 1, 1, 1, 1, 2, 0,
    };
    require(!memcmp((const uint8_t *)data.bytes + 26 * sizeof(uint32_t),
        expectedQueryRows, sizeof expectedQueryRows),
        @"exact dimension rows start at samplerOffset + 3*N");
    require(word(data, 40) == 2 && word(data, 48) == 2 &&
        word(data, 41) == 0 && word(data, 49) == 0,
        @"ABI 8 query rows carry 2D shape and clear multisample bit");

    NSMutableData *noQueries = rdna4_argument_table_bytes_abi(rows, NO, bias,
                                                               YES, flags);
    require(noQueries.length == 104,
        @"ABI 8 table without queries has no dimension tail");
    require(noQueries.length == 26 * 4 && word(noQueries, 23) == 0 &&
        word(noQueries, 24) == 1 && word(noQueries, 25) == 0,
        @"ABI 8 flags remain the final tail without queries");

    NSMutableData *legacy = rdna4_argument_table_bytes(rows, NO, bias);
    require(legacy.length == 92 && word(legacy, 4) == 17 &&
        word(legacy, 5) == 3,
        @"ABI 7 wrapper keeps the token-only layout");
    NSMutableData *legacyQueries = rdna4_argument_table_bytes(rows, YES, bias);
    require(legacyQueries.length == 47 * 4 &&
        word(legacyQueries, 37) == 0 && word(legacyQueries, 45) == 0,
        @"ABI 7 query rows keep reserved words 6/7 zero");

    require(!rdna4_argument_table_bytes_abi(rows, NO, bias, YES, nil),
        @"missing flags rejected");
    require(!rdna4_argument_table_bytes_abi(rows, NO, bias, YES,
                                             @[ @0, @0 ]),
        @"short flags rejected");
    require(!rdna4_argument_table_bytes_abi(rows, NO, bias, YES,
                                             @[ @0, @1, @0, @1 ]),
        @"long flags rejected");
    require(!rdna4_argument_table_bytes_abi(rows, NO, bias, YES,
                                             @[ @1, @0, @0 ]),
        @"nonzero dummy flag rejected");
    require(!rdna4_argument_table_bytes_abi(rows, NO, bias, YES,
                                             @[ @0, @2, @0 ]),
        @"undefined flag bits rejected");
    NSArray *nonNumericFlags = @[ @0, @"1", @0 ];
    require(!rdna4_argument_table_bytes_abi(rows, NO, bias, YES,
                                             nonNumericFlags),
        @"non-integer flag rejected");
    require(!rdna4_argument_table_bytes_abi(rows, NO, bias, YES,
                                             @[ @0, @YES, @0 ]),
        @"boolean flag rejected");
    require(!rdna4_argument_table_bytes_abi(rows, NO, -1, YES, flags),
        @"invalid LOD bias rejected");
}

static void testTable8QueryShape(void) {
    RDNA4CoordinateTextureProbe *uintArray = textureProbe(
        (MTLTextureType)RDNA4TD_MTL_2DARRAY, 16, 8, 1, 2, 3, 1);
    uintArray.uintFormat = YES;
    RDNA4CoordinateTextureProbe *depthMS = textureProbe(
        (MTLTextureType)RDNA4TD_MTL_2DMS, 4, 4, 1, 1, 1, 4);
    depthMS.depthFormat = YES;
    NSArray *rows = @[
        @[
            @[ @(UINT64_C(0x11111111)), uintArray ],
            @[ @(UINT64_C(0x22222222)), depthMS ],
        ],
        @[],
        @[],
    ];
    NSMutableData *data = rdna4_argument_table_bytes_abi(
        rows, YES, 2.5f, YES, @[ @0 ]);
    require(data != nil && data.length == 42 * 4,
        @"ABI 8 query rows reserve exactly eight words per row");
    require(word(data, 18) == 0 && word(data, 25) == 0 &&
        word(data, 26) == 16 && word(data, 27) == 8 &&
        word(data, 29) == 2 && word(data, 30) == 3 &&
        word(data, 31) == 1 && word(data, 32) == 19 &&
        word(data, 33) == 0,
        @"2D-array uint query row uses shape 19 and non-MS flag");
    require(word(data, 34) == 4 && word(data, 35) == 4 &&
        word(data, 39) == 4 && word(data, 40) == 10 &&
        word(data, 41) == 1,
        @"depth multisample query row uses depth shape and MS bit");

    uint32_t row[8] = {1, 2, 3, 4, 5, 1, 7, 8};
    require(rdna4_argument_image_query_row((id<MTLTexture>)uintArray, row, NO) &&
        row[6] == 0 && row[7] == 0,
        @"ABI 7 query row keeps reserved words 6/7 zero");
    memset(row, 0xff, sizeof row);
    require(rdna4_argument_image_query_row(nil, row, YES) &&
        !row[0] && !row[1] && !row[2] && !row[3] &&
        !row[4] && !row[5] && !row[6] && !row[7],
        @"null ABI 8 query row stays all zero");

    RDNA4CoordinateTextureProbe *oneDArray = textureProbe(
        (MTLTextureType)RDNA4TD_MTL_1DARRAY, 4, 1, 1, 2, 1, 1);
    RDNA4CoordinateTextureProbe *threeDSint = textureProbe(
        (MTLTextureType)RDNA4TD_MTL_3D, 4, 4, 4, 1, 1, 1);
    threeDSint.sintFormat = YES;
    RDNA4CoordinateTextureProbe *cubeArray = textureProbe(
        (MTLTextureType)RDNA4TD_MTL_CUBEARRAY, 4, 4, 1, 2, 1, 1);
    require(rdna4_argument_image_query_row((id<MTLTexture>)oneDArray, row, YES) &&
        row[6] == 1 &&
        rdna4_argument_image_query_row((id<MTLTexture>)threeDSint, row, YES) &&
        row[6] == 36 &&
        rdna4_argument_image_query_row((id<MTLTexture>)cubeArray, row, YES) &&
        row[6] == 7,
        @"ABI 8 query shape covers 1D-array, sint 3D and cube-array");

    RDNA4CoordinateTextureProbe *buffer = textureProbe(
        (MTLTextureType)RDNA4TD_MTL_BUFFER, 1, 1, 1, 1, 1, 1);
    require(!rdna4_argument_table_bytes_abi(
        @[ @[ @[ @1, buffer ] ], @[], @[] ], YES, 0, YES, @[ @0 ]),
        @"ABI 8 query row rejects a non-image texture type");
}

static NSDictionary *table8Metadata(void) {
    return @{
        @"trimmed_from_reflection_version": @64,
        @"normalized_sampler_coordinates": @YES,
        @"argument_resource_tables": @{
            @"version": @8,
            @"table_binding": @672,
            @"categories": @[ @0 ],
            @"comparison_sampler": @NO,
        },
        @"bindings": @[],
    };
}

static NSDictionary *table8QueryMetadata(void) {
    return @{
        @"trimmed_from_reflection_version": @64,
        @"normalized_sampler_coordinates": @YES,
        @"argument_resource_tables": @{
            @"version": @8,
            @"table_binding": @672,
            @"categories": @[ @0 ],
            @"comparison_sampler": @NO,
            @"image_query_binding": @673,
        },
        @"bindings": @[],
    };
}

static void tableReflection(RDNA4sRefl *reflection) {
    memset(reflection, 0, sizeof *reflection);
    reflection->maxset = 4;
    reflection->bind[0][672] = (RDNA4sBind){
        .used = 1, .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .count = 1 };
    reflection->bind[2][0] = (RDNA4sBind){
        .used = 1, .type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .runtime = 1 };
}

static void testTableVersionGate(void) {
    RDNA4sRefl reflection;
    tableReflection(&reflection);
    NSError *error = nil;
    require(rdna4_argument_table_metadata(&reflection, 0, table8Metadata(),
                                           &error) && !error,
        @"table 8 with reflection 64 accepted");

    NSMutableDictionary *metadata = [table8Metadata() mutableCopy];
    [metadata removeObjectForKey:@"trimmed_from_reflection_version"];
    error = nil;
    require(!rdna4_argument_table_metadata(&reflection, 0, metadata, &error) &&
        error, @"table 8 without trimmed reflection version rejected");

    metadata = [table8Metadata() mutableCopy];
    [metadata removeObjectForKey:@"normalized_sampler_coordinates"];
    error = nil;
    require(!rdna4_argument_table_metadata(&reflection, 0, metadata, &error) &&
        error, @"table 8 without normalized marker rejected");

    metadata = [table8Metadata() mutableCopy];
    metadata[@"normalized_sampler_coordinates"] = @NO;
    error = nil;
    require(!rdna4_argument_table_metadata(&reflection, 0, metadata, &error) &&
        error, @"table 8 with legacy marker rejected");

    metadata = [table8Metadata() mutableCopy];
    metadata[@"normalized_sampler_coordinates"] = @1;
    error = nil;
    require(!rdna4_argument_table_metadata(&reflection, 0, metadata, &error) &&
        error, @"malformed normalized marker rejected");

    metadata = [table8Metadata() mutableCopy];
    metadata[@"trimmed_from_reflection_version"] = @63;
    error = nil;
    require(!rdna4_argument_table_metadata(&reflection, 0, metadata, &error) &&
        error, @"table 8 with reflection 63 rejected");

    metadata = [table8Metadata() mutableCopy];
    metadata[@"trimmed_from_reflection_version"] = @65;
    error = nil;
    require(!rdna4_argument_table_metadata(&reflection, 0, metadata, &error) &&
        error, @"unknown reflection 65 rejected");

    metadata = [table8Metadata() mutableCopy];
    metadata[@"trimmed_from_reflection_version"] = @1.5;
    error = nil;
    require(!rdna4_argument_table_metadata(&reflection, 0, metadata, &error) &&
        error, @"fractional reflection version rejected");

    NSMutableDictionary *legacy = [table8Metadata() mutableCopy];
    NSMutableDictionary *legacyTable =
        [legacy[@"argument_resource_tables"] mutableCopy];
    legacyTable[@"version"] = @7;
    legacy[@"argument_resource_tables"] = legacyTable;
    legacy[@"trimmed_from_reflection_version"] = @63;
    [legacy removeObjectForKey:@"normalized_sampler_coordinates"];
    error = nil;
    require(rdna4_argument_table_metadata(&reflection, 0, legacy, &error) &&
        !error, @"legacy table 7 with reflection 63 accepted");

    legacy[@"trimmed_from_reflection_version"] = @64;
    error = nil;
    require(rdna4_argument_table_metadata(&reflection, 0, legacy, &error) &&
        !error, @"legacy table 7 with reflection 64 and no marker accepted");

    legacy[@"normalized_sampler_coordinates"] = @NO;
    error = nil;
    require(rdna4_argument_table_metadata(&reflection, 0, legacy, &error) &&
        !error, @"legacy table 7 with reflection 64 and false marker accepted");

    legacy[@"normalized_sampler_coordinates"] = @YES;
    error = nil;
    require(!rdna4_argument_table_metadata(&reflection, 0, legacy, &error) &&
        error, @"table 7 with normalized marker rejected");

    reflection.bind[0][673] = (RDNA4sBind){
        .used = 1, .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .count = 1 };
    error = nil;
    require(rdna4_argument_table_metadata(&reflection, 0, table8QueryMetadata(),
                                          &error) && !error,
        @"table 8 image-query metadata accepted with query binding 673");
    reflection.bind[0][673].used = 0;
    error = nil;
    require(!rdna4_argument_table_metadata(&reflection, 0, table8QueryMetadata(),
                                           &error) && error,
        @"table 8 image-query metadata without reflected 673 rejected");
}

static NSDictionary *validCoordinateMetadata(void);

static void testMixedTableAbiRejected(void) {
    NSMutableDictionary *legacy = [table8Metadata() mutableCopy];
    NSMutableDictionary *legacyTable =
        [legacy[@"argument_resource_tables"] mutableCopy];
    legacyTable[@"version"] = @7;
    legacy[@"argument_resource_tables"] = legacyTable;
    legacy[@"trimmed_from_reflection_version"] = @63;
    NSDictionary *empty = @{ @"bindings": @[] };
    NSDictionary *direct = validCoordinateMetadata();
    require(!rdna4_argument_table_stages_compatible(table8Metadata(), legacy),
        @"mixed ABI 8 and ABI 7 GPU table stages rejected");
    NSMutableDictionary *abi9 = [table8Metadata() mutableCopy];
    NSMutableDictionary *abi9Table =
        [abi9[@"argument_resource_tables"] mutableCopy];
    abi9Table[@"version"] = @9;
    abi9[@"argument_resource_tables"] = abi9Table;
    NSMutableDictionary *abi10 = [abi9 mutableCopy];
    NSMutableDictionary *abi10Table =
        [abi9[@"argument_resource_tables"] mutableCopy];
    abi10Table[@"version"] = @10;
    abi10[@"argument_resource_tables"] = abi10Table;
    require(!rdna4_argument_table_stages_compatible(abi9, abi10) &&
        !rdna4_argument_table_stages_compatible(abi10, abi9),
        @"mixed ABI 9 and ABI 10 stages with different token headers rejected");
    require(rdna4_argument_table_stages_compatible(abi9, abi9) &&
        rdna4_argument_table_stages_compatible(abi10, abi10),
        @"matching ABI 9 or ABI 10 stage headers remain compatible");
    require(rdna4_argument_table_stages_compatible(table8Metadata(), empty) &&
        rdna4_argument_table_stages_compatible(empty, legacy) &&
        rdna4_argument_table_stages_compatible(legacy, legacy),
        @"GPU table ABI gate allows one table stage or matching legacy stages");
    require(rdna4_argument_table_stages_compatible(table8Metadata(), direct) &&
        rdna4_argument_table_stages_compatible(direct, table8Metadata()),
        @"table8 and direct674 stages keep independent coordinate channels");
    require(rdna4_argument_table_stages_compatible(legacy, direct) &&
        rdna4_argument_table_stages_compatible(direct, legacy),
        @"table7 and direct674 stages keep independent coordinate channels");
}

static void testCoordinateBandMapping(void) {
    uint64_t index = UINT64_MAX;
    RDNA4PB source = {
        .binding = 160, .type = VK_DESCRIPTOR_TYPE_SAMPLER, .count = 4,
    };
    require(rdna4_coordinate_band_index(&source, 0, &index) && index == 0 &&
        rdna4_coordinate_band_index(&source, 3, &index) && index == 3,
        @"ordinary sampler band maps 160..191 to 0..31");
    source.binding = 736;
    source.count = 3;
    require(rdna4_coordinate_band_index(&source, 0, &index) && index == 0 &&
        rdna4_coordinate_band_index(&source, 2, &index) && index == 2,
        @"comparison sampler band maps 736..767 to the same flags");
    source.binding = 174;
    source.count = 20;
    require(!rdna4_coordinate_band_index(&source, 18, &index) &&
        !rdna4_coordinate_band_index(&source, 19, &index),
        @"sampler array overflow past slot 31 rejected");
    for (NSNumber *outside in @[ @159, @192, @735, @768 ]) {
        source.binding = outside.unsignedIntValue;
        source.count = 1;
        require(!rdna4_coordinate_band_index(&source, 0, &index),
            @"sampler coordinate binding outside the two legal bands rejected");
    }

    uint32_t flags[32] = {0};
    BOOL assigned[32] = {NO};
    require(rdna4_coordinate_flag_merge(flags, assigned, 7, YES) ==
        RDNA4CoordinateFlagOK && assigned[7] && flags[7] == 1 &&
        rdna4_coordinate_flag_merge(flags, assigned, 7, YES) ==
        RDNA4CoordinateFlagOK,
        @"repeated equivalent sampler mappings agree");
    require(rdna4_coordinate_flag_merge(flags, assigned, 7, NO) ==
        RDNA4CoordinateFlagConflict &&
        rdna4_coordinate_flag_merge(flags, assigned, 32, NO) ==
        RDNA4CoordinateFlagRange,
        @"conflicting or overflowing sampler mappings rejected");
}

static void testTableStageFlagsUseActualStage(void) {
    RDNA4sRefl vertex = {0}, fragment = {0};
    vertex.bind[2][0] = (RDNA4sBind){
        .used = 1, .type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        .count = 1, .runtime = 1,
    };
    fragment.bind[0][674] = (RDNA4sBind){
        .used = 1, .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .count = 1,
    };
    RDNA4ArgumentTables tables = {0};
    rdna4_argument_capacity_limits limits = {0};
    require(rdna4_argument_capacity_scan(&tables, &vertex, &fragment, NO,
                                         &limits, NULL) &&
        limits.used_stages[0] == 1 && limits.used_stages[1] == 0 &&
        limits.used_stages[2] == 0,
        @"vertex table8 uses only the vertex stage flag beside fragment 674");

    memset(&tables, 0, sizeof tables);
    memset(&limits, 0, sizeof limits);
    memset(&vertex, 0, sizeof vertex);
    memset(&fragment, 0, sizeof fragment);
    vertex.bind[0][674] = (RDNA4sBind){
        .used = 1, .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .count = 1,
    };
    fragment.bind[2][0] = (RDNA4sBind){
        .used = 1, .type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        .count = 1, .runtime = 1,
    };
    require(rdna4_argument_capacity_scan(&tables, &vertex, &fragment, NO,
                                         &limits, NULL) &&
        limits.used_stages[0] == 2 && limits.used_stages[1] == 0 &&
        limits.used_stages[2] == 0,
        @"fragment table8 uses only the fragment stage flag beside vertex 674");
}

static void testDestroyClearsSamplerCoordinates(void) {
    RDNA4ArgumentTables tables = {
        .active = YES,
        .imageQueries = YES,
        .comparisonSamplers = YES,
        .samplerCoordinates = YES,
    };
    rdna4_argument_tables_destroy(&tables);
    require(!tables.active && !tables.imageQueries &&
        !tables.comparisonSamplers && !tables.samplerCoordinates,
        @"table destroy clears ABI 8 sampler-coordinate mode");
    rdna4_argument_tables_destroy(&tables);
    require(!tables.active && !tables.samplerCoordinates,
        @"table destroy is idempotent after clearing sampler-coordinate mode");
}

static NSDictionary *coordinateMetadata(NSString *kind, NSNumber *set,
                                        NSNumber *binding, NSNumber *count,
                                        NSNumber *declared, NSNumber *extentBytes,
                                        NSString *access, NSNumber *version) {
    return @{
        @"trimmed_from_reflection_version": version,
        @"normalized_sampler_coordinates": @YES,
        @"bindings": @[@{
            @"kind": kind,
            @"metal_index": @0,
            @"descriptor": @{
                @"set": set,
                @"binding": binding,
                @"count": count,
            },
            @"declared_size": declared,
            @"extent": @{ @"Object": @{ @"bytes": extentBytes } },
            @"access": access,
        }],
    };
}

static NSDictionary *validCoordinateMetadata(void) {
    return coordinateMetadata(@"SamplerCoordinateFlags", @0, @674, @1, @128,
                              @128, @"ReadOnly", @64);
}

static RDNA4sRefl *directReflection(BOOL include674) {
    RDNA4sRefl *reflection = calloc(1, sizeof *reflection);
    require(reflection != NULL, @"direct reflection allocation");
    if (include674)
        reflection->bind[0][674] = (RDNA4sBind){
            .used = 1, .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .count = 1 };
    return reflection;
}

static BOOL stageLayout(RDNA4sRefl *reflection, NSDictionary *metadata,
                        VkDescriptorSetLayout *layout, RDNA4PB **bindings,
                        uint32_t *count, NSMutableArray *owned,
                        NSError **error) {
    *layout = VK_NULL_HANDLE;
    *bindings = NULL;
    *count = 0;
    return rdna4_stage_dsl(reflection, 0, VK_SHADER_STAGE_COMPUTE_BIT,
                           metadata, layout, bindings, count, owned, error);
}

static BOOL fragmentStageLayout(RDNA4sRefl *reflection,
                                NSDictionary *metadata,
                                VkDescriptorSetLayout *layout,
                                RDNA4PB **bindings, uint32_t *count,
                                NSMutableArray *owned, NSError **error) {
    *layout = VK_NULL_HANDLE;
    *bindings = NULL;
    *count = 0;
    return rdna4_stage_dsl(reflection, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                           metadata, layout, bindings, count, owned, error);
}

static void testStageMetadataRelocation(void) {
    NSDictionary *metadata = @{ @"bindings": @[
        @{ @"kind": @"SamplerCoordinateFlags",
           @"descriptor": @{ @"set": @0, @"binding": @674 } },
        @{ @"kind": @"EmbeddedArgBufferSampler",
           @"descriptor": @{ @"set": @2, @"binding": @0 },
           @"comparison_descriptor": @{ @"set": @4, @"binding": @0 } },
    ] };
    NSDictionary *relocated = rdna4_stage_meta_relocate(metadata, 1);
    NSArray *rows = relocated[@"bindings"];
    require([rows[0][@"descriptor"][@"set"] isEqual:@1] &&
        [rows[1][@"descriptor"][@"set"] isEqual:@2] &&
        [rows[1][@"comparison_descriptor"][@"set"] isEqual:@4] &&
        [metadata[@"bindings"][0][@"descriptor"][@"set"] isEqual:@0],
        @"stage metadata relocation preserves table sets 2/4 and the source");
}

static void testDirect674Metadata(void) {
    RDNA4sRefl *reflection = directReflection(YES);
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    RDNA4PB *bindings = NULL;
    uint32_t count = 0;
    NSError *error = nil;
    NSMutableArray *owned = [NSMutableArray array];
    require(stageLayout(reflection, validCoordinateMetadata(), &layout,
                        &bindings, &count, owned, &error) &&
        !error && layout != VK_NULL_HANDLE && count == 1 &&
        bindings[0].binding == 674 && bindings[0].samplerCoordinateFlags &&
        !bindings[0].normalizedCoordinates && !bindings[0].gpuArguments,
        @"valid direct SamplerCoordinateFlags layout");
    free(bindings);
    free(reflection);

    reflection = directReflection(YES);
    reflection->bind[1][674] = reflection->bind[0][674];
    memset(&reflection->bind[0][674], 0, sizeof reflection->bind[0][674]);
    reflection->maxset = 1;
    error = nil;
    require(fragmentStageLayout(reflection, validCoordinateMetadata(), &layout,
                                &bindings, &count, owned, &error) &&
        !error && count == 1 && bindings[0].binding == 674 &&
        bindings[0].samplerCoordinateFlags,
        @"fragment binding 674 accepts producer set 0 and relocates to set 1");
    free(bindings);

    NSDictionary *fragmentMetadata =
        coordinateMetadata(@"SamplerCoordinateFlags", @1, @674, @1, @128,
                           @128, @"ReadOnly", @64);
    error = nil;
    require(fragmentStageLayout(reflection, fragmentMetadata, &layout,
                                &bindings, &count, owned, &error) &&
        !error && count == 1 && bindings[0].binding == 674 &&
        bindings[0].samplerCoordinateFlags,
        @"fragment binding 674 still accepts an already relocated set 1");
    free(bindings);
    free(reflection);

    NSArray<NSDictionary *> *invalid = @[
        coordinateMetadata(@"SamplerCoordinateFlag", @0, @674, @1, @128, @128,
                           @"ReadOnly", @64),
        coordinateMetadata(@"SamplerCoordinateFlags", @1, @674, @1, @128, @128,
                           @"ReadOnly", @64),
        coordinateMetadata(@"SamplerCoordinateFlags", @0, @673, @1, @128, @128,
                           @"ReadOnly", @64),
        coordinateMetadata(@"SamplerCoordinateFlags", @0, @674, @0, @128, @128,
                           @"ReadOnly", @64),
        coordinateMetadata(@"SamplerCoordinateFlags", @0, @674, @2, @128, @128,
                           @"ReadOnly", @64),
        coordinateMetadata(@"SamplerCoordinateFlags", @0, @674, @1, @127, @128,
                           @"ReadOnly", @64),
        coordinateMetadata(@"SamplerCoordinateFlags", @0, @674, @1, @128, @64,
                           @"ReadOnly", @64),
        coordinateMetadata(@"SamplerCoordinateFlags", @0, @674, @1, @128, @128,
                           @"ReadWrite", @64),
        coordinateMetadata(@"SamplerCoordinateFlags", @0, @674, @1, @128, @128,
                           @"ReadOnly", @63),
        coordinateMetadata(@"SamplerCoordinateFlags", @0, @674, @1, @128, @128,
                           @"ReadOnly", @65),
    ];
    unsigned before = layoutCreates;
    for (NSDictionary *metadata in invalid) {
        reflection = directReflection(YES);
        error = nil;
        layout = VK_NULL_HANDLE;
        bindings = NULL;
        count = 0;
        require(!stageLayout(reflection, metadata, &layout, &bindings, &count,
                             owned, &error) &&
            error && !layout && !bindings && !count,
            @"malformed direct 674 metadata rejected before layout");
        free(reflection);
    }
    require(layoutCreates == before,
        @"malformed direct 674 reached Vulkan layout creation");

    NSMutableDictionary *missingDeclared =
        [validCoordinateMetadata() mutableCopy];
    NSMutableDictionary *declaredRow =
        [missingDeclared[@"bindings"][0] mutableCopy];
    [declaredRow removeObjectForKey:@"declared_size"];
    missingDeclared[@"bindings"] = @[ declaredRow ];
    NSMutableDictionary *missingExtent =
        [validCoordinateMetadata() mutableCopy];
    NSMutableDictionary *missingExtentRow =
        [missingExtent[@"bindings"][0] mutableCopy];
    [missingExtentRow removeObjectForKey:@"extent"];
    missingExtent[@"bindings"] = @[ missingExtentRow ];
    NSMutableDictionary *missingAccess =
        [validCoordinateMetadata() mutableCopy];
    NSMutableDictionary *accessRow =
        [missingAccess[@"bindings"][0] mutableCopy];
    [accessRow removeObjectForKey:@"access"];
    missingAccess[@"bindings"] = @[ accessRow ];
    NSMutableDictionary *missingMarker =
        [validCoordinateMetadata() mutableCopy];
    [missingMarker removeObjectForKey:@"normalized_sampler_coordinates"];
    NSMutableDictionary *legacyMarker =
        [validCoordinateMetadata() mutableCopy];
    legacyMarker[@"normalized_sampler_coordinates"] = @NO;
    NSMutableDictionary *malformedMarker =
        [validCoordinateMetadata() mutableCopy];
    malformedMarker[@"normalized_sampler_coordinates"] = @"YES";
    NSMutableDictionary *duplicateRows =
        [validCoordinateMetadata() mutableCopy];
    duplicateRows[@"bindings"] = @[
        validCoordinateMetadata()[@"bindings"][0],
        validCoordinateMetadata()[@"bindings"][0],
    ];
    NSMutableDictionary *extraBinding =
        [validCoordinateMetadata() mutableCopy];
    NSMutableDictionary *bindingRow =
        [extraBinding[@"bindings"][0] mutableCopy];
    bindingRow[@"synthetic_noise"] = @YES;
    extraBinding[@"bindings"] = @[ bindingRow ];
    NSMutableDictionary *extraDescriptor =
        [validCoordinateMetadata() mutableCopy];
    NSMutableDictionary *descriptorRow =
        [extraDescriptor[@"bindings"][0] mutableCopy];
    NSMutableDictionary *descriptor =
        [descriptorRow[@"descriptor"] mutableCopy];
    descriptor[@"synthetic_noise"] = @0;
    descriptorRow[@"descriptor"] = descriptor;
    extraDescriptor[@"bindings"] = @[ descriptorRow ];
    NSMutableDictionary *extraExtent = [validCoordinateMetadata() mutableCopy];
    NSMutableDictionary *extentRow =
        [extraExtent[@"bindings"][0] mutableCopy];
    NSMutableDictionary *extent = [extentRow[@"extent"] mutableCopy];
    NSMutableDictionary *object = [extent[@"Object"] mutableCopy];
    object[@"synthetic_noise"] = @0;
    extent[@"Object"] = object;
    extentRow[@"extent"] = extent;
    extraExtent[@"bindings"] = @[ extentRow ];
    for (NSDictionary *metadata in @[ missingDeclared, missingExtent,
                                      missingAccess, missingMarker,
                                      legacyMarker, malformedMarker,
                                      duplicateRows, extraBinding,
                                      extraDescriptor, extraExtent ]) {
        reflection = directReflection(YES);
        error = nil;
        layout = VK_NULL_HANDLE;
        bindings = NULL;
        count = 0;
        require(!stageLayout(reflection, metadata, &layout, &bindings, &count,
                             owned, &error) &&
            error && !layout && !bindings && !count,
            @"missing or duplicate direct 674 fields rejected");
        free(reflection);
    }

    reflection = directReflection(NO);
    reflection->bind[1][674] = (RDNA4sBind){
        .used = 1, .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .count = 1 };
    error = nil;
    NSDictionary *globalSetMetadata = coordinateMetadata(
        @"SamplerCoordinateFlags", @2, @674, @1, @128, @128, @"ReadOnly", @64);
    require(!fragmentStageLayout(reflection, globalSetMetadata, &layout,
                                 &bindings, &count, owned, &error) &&
        error, @"fragment 674 metadata cannot use a global table set");
    free(reflection);

    reflection = directReflection(NO);
    error = nil;
    require(!stageLayout(reflection, validCoordinateMetadata(), &layout,
                         &bindings, &count, owned, &error) &&
        error && layoutCreates == before,
        @"674 metadata without reflected binding rejected");
    free(reflection);

    reflection = directReflection(YES);
    error = nil;
    require(!stageLayout(reflection, @{
            @"trimmed_from_reflection_version": @64,
            @"bindings": @[],
        }, &layout, &bindings, &count, owned, &error) &&
        error && layoutCreates == before,
        @"reflected 674 without SamplerCoordinateFlags metadata rejected");
    free(reflection);

    reflection = directReflection(YES);
    NSMutableDictionary *withTable = [validCoordinateMetadata() mutableCopy];
    withTable[@"argument_resource_tables"] =
        table8Metadata()[@"argument_resource_tables"];
    error = nil;
    require(!stageLayout(reflection, withTable, &layout, &bindings, &count,
                         owned, &error) &&
        error && layoutCreates == before,
        @"674 mixed with GPU table path rejected");
    free(reflection);
}

static void testTableAndDirectPBChannelsIndependent(void) {
    RDNA4sRefl *table = calloc(1, sizeof *table);
    require(table != NULL, @"table reflection allocation");
    table->maxset = 4;
    table->bind[0][672] = (RDNA4sBind){
        .used = 1, .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .count = 1,
    };
    table->bind[2][0] = (RDNA4sBind){
        .used = 1, .type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        .count = 1, .runtime = 1,
    };
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    RDNA4PB *bindings = NULL;
    uint32_t count = 0;
    NSError *error = nil;
    require(stageLayout(table, table8Metadata(), &layout, &bindings, &count,
                        [NSMutableArray array], &error) &&
        !error && count == 1 && bindings[0].binding == 672 &&
        bindings[0].gpuArguments && bindings[0].argumentTable,
        @"table8 stage marks only its table buffer as GPU-argument backed");
    free(bindings);
    free(table);

    RDNA4sRefl *direct = directReflection(YES);
    layout = VK_NULL_HANDLE;
    bindings = NULL;
    count = 0;
    error = nil;
    require(stageLayout(direct, validCoordinateMetadata(), &layout, &bindings,
                        &count, [NSMutableArray array], &error) &&
        !error && count == 1 && bindings[0].binding == 674 &&
        !bindings[0].gpuArguments && bindings[0].samplerCoordinateFlags,
        @"direct674 stage keeps its stage-local descriptor channel");
    free(bindings);
    free(direct);
}

static void testActualFragmentDirect674(void) {
    NSString *fixture = [[@(__FILE__) stringByDeletingLastPathComponent]
        stringByAppendingPathComponent:@"fixtures/abi8"];
    NSData *spv = [NSData dataWithContentsOfFile:
        [fixture stringByAppendingPathComponent:@"fragment-indirect.spv"]];
    NSData *json = [NSData dataWithContentsOfFile:
        [fixture stringByAppendingPathComponent:@"fragment-indirect.meta.json"]];
    require(spv && json, @"ABI 8 producer fixture is missing");
    require([rdna4_sha256hex(spv) isEqual:
        @"4ff1a46d762084c3ac29feab4882fad4075758ec87c788b341207b3a2cf70708"] &&
        [rdna4_sha256hex(json) isEqual:
        @"0241ba6ed45dc3a6a04a3a735e353a1ad57e3913e3851d2e6b8fca3b3e647cec"],
        @"ABI 8 producer fixture hashes");
    NSDictionary *metadata = [NSJSONSerialization JSONObjectWithData:json
                                                              options:0
                                                                error:NULL];
    require([metadata isKindOfClass:[NSDictionary class]] &&
        [metadata[@"stage"] isEqual:@"Fragment"] &&
        [metadata[@"normalized_sampler_coordinates"] isEqual:@YES] &&
        [metadata[@"trimmed_from_reflection_version"] isEqual:@64],
        @"ABI 8 producer fragment metadata");
    NSArray *rows = metadata[@"bindings"];
    NSDictionary *coordinate = nil;
    for (NSDictionary *row in rows)
        if ([row[@"kind"] isEqual:@"SamplerCoordinateFlags"])
            coordinate = row;
    require(coordinate &&
        [[NSSet setWithArray:coordinate.allKeys] isEqualToSet:
            [NSSet setWithObjects:@"kind", @"metal_index", @"descriptor",
                @"declared_size", @"extent", @"access", nil]] &&
        [coordinate[@"descriptor"] isEqual:
            @{ @"set": @0, @"binding": @674, @"count": @1 }] &&
        [coordinate[@"declared_size"] isEqual:@128] &&
        [coordinate[@"extent"] isEqual:
            @{ @"Object": @{ @"bytes": @128 } }],
        @"ABI 8 producer row is already exact after the trim contract");

    NSData *relocated = rdna4_spv_set(spv, 1);
    RDNA4sMod module = {0};
    RDNA4sRefl *reflection = calloc(1, sizeof *reflection);
    char why[256] = {0};
    require(relocated && reflection &&
        !rdna4s_parse(relocated.bytes, relocated.length / 4, &module,
                      why, sizeof why) &&
        module.has_ep && module.ep_model == 4 &&
        !rdna4s_reflect(&module, 1, reflection),
        @"ABI 8 producer SPIR-V parses as relocated fragment set1");
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    RDNA4PB *bindings = NULL;
    uint32_t count = 0;
    NSError *error = nil;
    BOOL previousNonUniform = RDNA4R.nonUniformSampledImages;
    RDNA4R.nonUniformSampledImages = YES;
    BOOL validLayout = fragmentStageLayout(reflection, metadata, &layout,
        &bindings, &count, [NSMutableArray array], &error);
    RDNA4R.nonUniformSampledImages = previousNonUniform;
    require(validLayout && !error, [NSString stringWithFormat:
        @"ABI 8 actual fragment metadata and SPIR-V stage layout: %@", error]);
    BOOL flags = NO, sampler = NO;
    for (uint32_t i = 0; i < count; ++i) {
        if (bindings[i].binding == 674)
            flags = bindings[i].samplerCoordinateFlags &&
                !bindings[i].gpuArguments;
        if (bindings[i].binding == 160)
            sampler = bindings[i].normalizedCoordinates &&
                bindings[i].embedded;
    }
    require(flags && sampler,
        @"ABI 8 actual fragment uses direct674 and normalized embedded sampler");
    free(bindings);
    free(reflection);
    rdna4s_free(&module);
}

static NSDictionary *staticMetadata(NSString *coordinates,
                                    NSString *addressMode) {
    return @{
        @"trimmed_from_reflection_version": @64,
        @"normalized_sampler_coordinates": @YES,
        @"bindings": @[@{
            @"kind": @"StaticSampler",
            @"descriptor": @{ @"set": @0, @"binding": @160, @"count": @1 },
            @"static_sampler": @{
                @"min_filter": @"Nearest",
                @"mag_filter": @"Nearest",
                @"mip_filter": @"None",
                @"address_mode_s": addressMode,
                @"address_mode_t": addressMode,
                @"address_mode_r": addressMode,
                @"border_color": @"TransparentBlack",
                @"compare_function": @"None",
                @"coordinates": coordinates,
                @"lod_min_clamp": @0.0,
                @"lod_max_clamp": @0.0,
                @"max_anisotropy": @1,
            },
        }],
    };
}

static void testStaticPixelCompileTime(void) {
    RDNA4sRefl *reflection = calloc(1, sizeof *reflection);
    require(reflection != NULL, @"static reflection allocation");
    reflection->bind[0][160] = (RDNA4sBind){
        .used = 1, .type = VK_DESCRIPTOR_TYPE_SAMPLER, .count = 1 };
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    RDNA4PB *bindings = NULL;
    uint32_t count = 0;
    NSError *error = nil;
    NSMutableArray *owned = [NSMutableArray array];
    unsigned created = samplerCreates;
    require(stageLayout(reflection, staticMetadata(@"Pixel", @"ClampToEdge"),
                        &layout, &bindings, &count, owned, &error) &&
        !error && count == 1 && bindings[0].binding == 160 &&
        bindings[0].stat != VK_NULL_HANDLE &&
        bindings[0].normalizedCoordinates &&
        bindings[0].pixelCoordinates &&
        !bindings[0].samplerCoordinateFlags &&
        samplerCreates == created + 1 &&
        lastSampler.unnormalizedCoordinates == VK_FALSE &&
        lastSampler.compareEnable == VK_FALSE &&
        lastSampler.mipLodBias == 0.0f,
        @"static pixel sampler uses compile-time coordinate metadata");
    rdna4_destroy_sampler(bindings[0].stat, nil);
    free(bindings);

    bindings = NULL;
    count = 0;
    layout = VK_NULL_HANDLE;
    error = nil;
    created = samplerCreates;
    require(!stageLayout(reflection, staticMetadata(@"Pixel", @"Repeat"),
                         &layout, &bindings, &count, owned, &error) &&
        error && samplerCreates == created,
        @"static pixel repeat rejected before sampler creation");
    free(reflection);
}

static void testLegacyReflectionDoesNotEnableNormalizedPB(void) {
    RDNA4sRefl *reflection = calloc(1, sizeof *reflection);
    require(reflection != NULL, @"legacy reflection allocation");
    reflection->bind[0][160] = (RDNA4sBind){
        .used = 1, .type = VK_DESCRIPTOR_TYPE_SAMPLER, .count = 1 };
    NSDictionary *metadata = @{
        @"trimmed_from_reflection_version": @64,
        @"bindings": @[@{
            @"kind": @"Sampler",
            @"descriptor": @{ @"set": @0, @"binding": @160, @"count": @1 },
        }],
    };
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    RDNA4PB *bindings = NULL;
    uint32_t count = 0;
    NSError *error = nil;
    require(stageLayout(reflection, metadata, &layout, &bindings, &count,
                        [NSMutableArray array], &error) &&
        !error && count == 1 && !bindings[0].normalizedCoordinates,
        @"reflection 64 without marker stays on the legacy sampler backend");
    free(bindings);
    free(reflection);
}

static void testPartialTablePostpassSelection(void) {
    NSDictionary *flat = @{ @"kind": @"EmbeddedArgBufferTexture",
        @"descriptor": @{ @"set": @0, @"binding": @32, @"count": @1 } };
    NSDictionary *lowered = @{ @"kind": @"EmbeddedArgBufferTexture",
        @"descriptor": @{ @"set": @2, @"binding": @0, @"count": @0 } };
    NSDictionary *table = @{ @"version": @9, @"null_shapes": @[@2] };
    require(rdna4_argument_needs_postpass(@{ @"bindings": @[flat] }),
        @"flat field requires token lowering");
    require(rdna4_argument_needs_postpass(@{ @"bindings": @[flat],
        @"argument_resource_tables": table }),
        @"static null table does not suppress flat-field lowering");
    require(rdna4_argument_needs_postpass(@{ @"bindings": @[lowered, flat],
        @"argument_resource_tables": table }),
        @"partial table still lowers remaining flat fields");
    require(!rdna4_argument_needs_postpass(@{ @"bindings": @[lowered],
        @"argument_resource_tables": table }),
        @"completed table does not run twice");
    require(!rdna4_argument_needs_postpass(@{ @"bindings": @[],
        @"argument_resource_tables": table }),
        @"static null-only stage needs no postpass");
}

int main(void) {
    @autoreleasepool {
        RDNA4R.ok = YES;
        RDNA4R.dev = (VkDevice)(uintptr_t)0x1234;
        RDNA4R.gpuArgumentTables = YES;
        RDNA4R.samplerAnisotropy = YES;
        RDNA4R.samplerMirrorClamp = NO;
        RDNA4R.lim.maxSamplerAnisotropy = 16.0f;
        RDNA4R.lim.maxSamplerAllocationCount = 64;
        RDNA4R.lim.maxStorageBufferRange = UINT32_MAX;
        atomic_store(&rdna4_sampler_allocations, 0);
        vkCreateDescriptorSetLayout = createLayout;
        vkCreateSampler = createSampler;
        vkDestroySampler = destroySampler;

        testExactTableBytes();
        testTable8QueryShape();
        testTableVersionGate();
        testMixedTableAbiRejected();
        testCoordinateBandMapping();
        testTableStageFlagsUseActualStage();
        testDestroyClearsSamplerCoordinates();
        testStageMetadataRelocation();
        testDirect674Metadata();
        testTableAndDirectPBChannelsIndependent();
        testActualFragmentDirect674();
        testStaticPixelCompileTime();
        testLegacyReflectionDoesNotEnableNormalizedPB();
        testPartialTablePostpassSelection();
        printf("SAMPLER_COORDINATE_METADATA_PASS table=8 flags_tail=1 "
               "dimensions_offset=1 direct674=1 malformed_reject=1 "
               "legacy7=1 actual_fragment=1 simulated_vulkan=1 "
               "gpu_execution=0\n");
    }
    return 0;
}
