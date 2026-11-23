#include "rdna4_argument_descriptor_scan.h"

// Shared resource sets stay at 2..4; stage-local arguments stay at 0/1.
typedef struct {
    VkDescriptorSetLayout dsl[3];
    uint32_t capacity[3];
    BOOL variable[3];
    BOOL active;
    BOOL imageQueries;
    BOOL comparisonSamplers;
    BOOL samplerCoordinates;
    uint64_t nullImages;
    BOOL shapeRegistry;
    BOOL abi10;
    BOOL texel[2];
    uint8_t texelComponents[2];
    BOOL dynamicTexel[2];
    BOOL texelVariable[2];
    uint32_t texelCapacity[2];
    BOOL imageTable[3];
    uint32_t texelStages[2];
    uint32_t tableStages[3];
    rdna4_argument_descriptor_usage ordinaryUsage;
    uint64_t fragmentColorAttachments;
    BOOL descriptorBudgetReady;
    rdna4ns_registry nullShapes;
    BOOL tokenTableSeen, tokenTableNormalized;
} RDNA4ArgumentTables;

#include "rdna4_argument_capacity.h"

static void rdna4_argument_tables_destroy(RDNA4ArgumentTables *tables);

static VkDescriptorType rdna4_argument_descriptor_type(unsigned category) {
    const VkDescriptorType types[] = {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLER};
    return types[category];
}

static VkShaderStageFlags rdna4_argument_stage_flags(uint32_t stages,
                                                     BOOL compute) {
    if (!stages) return 0;
    if (compute) return VK_SHADER_STAGE_COMPUTE_BIT;
    VkShaderStageFlags flags = 0;
    if (stages & 1u) flags |= VK_SHADER_STAGE_VERTEX_BIT;
    if (stages & 2u) flags |= VK_SHADER_STAGE_FRAGMENT_BIT;
    return flags;
}

// ABI 5 adds component class (float/uint/int) to the ABI 4 shape offsets.
#define RDNA4_ARGUMENT_NULL_IMAGES 48u
static BOOL rdna4_argument_shape(unsigned shape, VkImageViewType *viewType, BOOL *depth) {
    if (shape >= RDNA4_ARGUMENT_NULL_IMAGES) return NO;
    unsigned component = shape / 16, depthBit = (shape & 15) >> 3, fields = shape & 7, arrayed = fields & 1, dim = fields >> 1;
    if (component && depthBit) return NO;
    if (dim > 3 || (dim == 2 && arrayed)) return NO;
    static const VkImageViewType views[4][2] = {
        { VK_IMAGE_VIEW_TYPE_1D, VK_IMAGE_VIEW_TYPE_1D_ARRAY },
        { VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D_ARRAY },
        { VK_IMAGE_VIEW_TYPE_3D, VK_IMAGE_VIEW_TYPE_3D },
        { VK_IMAGE_VIEW_TYPE_CUBE, VK_IMAGE_VIEW_TYPE_CUBE_ARRAY },
    };
    if (viewType) *viewType = views[dim][arrayed];
    if (depth) *depth = depthBit != 0;
    return YES;
}

// Token count stays in the ABI header. Shape descriptors extend only set 2's
// sampled-image array, after every real token.
static BOOL rdna4_argument_counts(const RDNA4ArgumentTables *tables, unsigned category, size_t rows,
                                  uint32_t *tokens, uint32_t *descriptors) {
    if (!tables || category >= 3 || rows > UINT32_MAX - 1ull) return NO;
    uint64_t tokenCount = rows + 1ull;
    uint32_t descriptorCount = 0;
    if (tables->shapeRegistry) {
        if (tables->nullShapes.count > RDNA4NS_MAX_SHAPES ||
            rdna4ns_table_descriptor_count(category, (uint32_t)tokenCount,
                                           &tables->nullShapes,
                                           &descriptorCount) != RDNA4NS_OK)
            return NO;
        if (category == 2 && tables->comparisonSamplers) {
            if (tokenCount > UINT32_MAX / 2u) return NO;
            descriptorCount = (uint32_t)(tokenCount * 2u);
        }
    } else {
        if (category == 0 && !rdna4_argument_capacity_tail(
                tables->nullImages, RDNA4_ARGUMENT_NULL_IMAGES, NULL)) return NO;
        for (unsigned shape = 0; shape < RDNA4_ARGUMENT_NULL_IMAGES; ++shape)
            if ((tables->nullImages & (1ull << shape)) &&
                !rdna4_argument_shape(shape, NULL, NULL)) return NO;
        if (!rdna4_argument_capacity_descriptor_count(
                category, tokenCount, tables->nullImages,
                tables->comparisonSamplers, &descriptorCount))
            return NO;
    }
    if (tokens) *tokens = (uint32_t)tokenCount;
    if (descriptors) *descriptors = descriptorCount;
    return YES;
}

static BOOL rdna4_argument_image_dimensions(id<MTLTexture> texture, uint32_t row[8]) {
    memset(row, 0, 8 * sizeof *row);
    if (!texture) return YES;
    NSUInteger values[] = {texture.width, texture.height, texture.depth, texture.arrayLength,
        texture.mipmapLevelCount, texture.sampleCount};
    for (unsigned i = 0; i < 6; ++i) {
        if (values[i] > UINT32_MAX) return NO;
        row[i] = (uint32_t)values[i];
    }
    return YES;
}

static BOOL rdna4_argument_image_query_row(id<MTLTexture> texture, uint32_t row[8],
                                           BOOL samplerCoordinates) {
    if (!rdna4_argument_image_dimensions(texture, row)) return NO;
    if (!samplerCoordinates || !texture) return YES;
    // ABI 8 query consumers need the sampled shape and MS bit; ABI 7 keeps both words reserved.
    RDNA4Texture *native = (RDNA4Texture *)texture;
    uint32_t dim = 0, arrayed = 0;
    switch ((unsigned long)[native textureType]) {
    case RDNA4TD_MTL_1D: break;
    case RDNA4TD_MTL_1DARRAY: arrayed = 1; break;
    case RDNA4TD_MTL_2D: dim = 1; break;
    case RDNA4TD_MTL_2DARRAY: dim = 1; arrayed = 1; break;
    case RDNA4TD_MTL_2DMS: dim = 1; break;
    case RDNA4TD_MTL_2DMSARRAY: dim = 1; arrayed = 1; break;
    case RDNA4TD_MTL_CUBE: dim = 3; break;
    case RDNA4TD_MTL_CUBEARRAY: dim = 3; arrayed = 1; break;
    case RDNA4TD_MTL_3D: dim = 2; break;
    default: return NO;
    }
    BOOL depth = [native rdna4IsDS];
    BOOL uintFormat = [native rdna4IsUInt];
    BOOL sintFormat = [native rdna4IsSInt];
    if (uintFormat && sintFormat) return NO;
    uint32_t component = uintFormat ? 1u : sintFormat ? 2u : 0u;
    row[6] = 2u * dim + arrayed + (depth ? 8u : 0u) + 16u * component;
    row[7] = row[5] > 1 ? 1u : 0u;
    return YES;
}

static BOOL rdna4_argument_reflection_version(NSDictionary *metadata,
                                              BOOL *present,
                                              uint64_t *version) {
    id value = metadata[@"trimmed_from_reflection_version"];
    *present = value != nil;
    *version = 0;
    if (!value) return YES;
    return rdna4_number_u64((__bridge CFTypeRef)value, version) &&
        *version > 0 && *version <= 66;
}

static BOOL rdna4_argument_normalized_metadata(NSDictionary *metadata,
                                                BOOL *normalized,
                                                NSError **error) {
    BOOL present = NO;
    uint64_t version = 0;
    if (!rdna4_argument_reflection_version(metadata, &present, &version)) {
        if (error) *error = rdna4_err(49,
            @"trimmed_from_reflection_version is unknown or malformed");
        return NO;
    }
    id marker = metadata[@"normalized_sampler_coordinates"];
    BOOL enabled = NO;
    if (marker) {
        if (CFGetTypeID((__bridge CFTypeRef)marker) !=
            CFBooleanGetTypeID()) {
            if (error) *error = rdna4_err(49,
                @"normalized_sampler_coordinates is not a boolean");
            return NO;
        }
        enabled = CFBooleanGetValue((__bridge CFBooleanRef)marker);
    }
    if (enabled && (!present || (version < 64 || version > 66))) {
        if (error) *error = rdna4_err(49,
            @"normalized_sampler_coordinates requires reflection version 64..66");
        return NO;
    }
    *normalized = enabled;
    return YES;
}

static BOOL rdna4_argument_table_uses_shape_registry(NSDictionary *metadata) {
    id table = metadata[@"argument_resource_tables"];
    uint64_t version = 0;
    return [table isKindOfClass:[NSDictionary class]] &&
        rdna4_number_u64((__bridge CFTypeRef)table[@"version"], &version) &&
        version >= 9 && version <= 10;
}

static BOOL rdna4_argument_table_stages_compatible(NSDictionary *a,
                                                    NSDictionary *b) {
    id at = a[@"argument_resource_tables"];
    id bt = b[@"argument_resource_tables"];
    uint64_t av = 0, bv = 0;
    BOOL aTable = [at isKindOfClass:[NSDictionary class]] &&
        rdna4_number_u64((__bridge CFTypeRef)at[@"version"], &av);
    BOOL bTable = [bt isKindOfClass:[NSDictionary class]] &&
        rdna4_number_u64((__bridge CFTypeRef)bt[@"version"], &bv);
    return rdna4ns_table_abi_compatible(aTable ? av : 0, bTable ? bv : 0);
}

static NSMutableData *rdna4_argument_table_bytes_internal(
    NSArray<NSArray *> *rows, NSArray<NSArray *> *texelRows,
    const BOOL dynamicTexel[2], BOOL abi10, BOOL imageQueries,
    float lodBiasLimit, BOOL samplerCoordinates,
    NSArray<NSNumber *> *samplerFlags) {
    if (rows.count != 3 || (abi10 && (!texelRows || texelRows.count != 2)))
        return nil;
    if (!isfinite(lodBiasLimit) || lodBiasLimit < 0) return nil;
    const uint64_t rowCounts[3] = {rows[0].count, rows[1].count, rows[2].count};
    if (rowCounts[0] >= UINT32_MAX || rowCounts[1] >= UINT32_MAX ||
        rowCounts[2] >= UINT32_MAX ||
        (samplerCoordinates && samplerFlags.count != rowCounts[2] + 1))
        return nil;
    uint64_t words = abi10 ? 11u : 7u;
    for (unsigned category = 0; category < 3; ++category)
        if (!rdna4_argument_capacity_add_u64(words,
                2u * (rowCounts[category] + 1u), &words)) return nil;
    if (samplerCoordinates && !rdna4_argument_capacity_add_u64(
            words, rowCounts[2] + 1u, &words)) return nil;
    if (imageQueries && !rdna4_argument_capacity_add_u64(
            words, 8u * (rowCounts[0] + 1u), &words)) return nil;
    if (abi10) for (unsigned texel = 0; texel < 2; ++texel) {
        if (!dynamicTexel[texel]) continue;
        uint64_t count = texelRows[texel].count;
        if (count >= UINT32_MAX ||
            !rdna4_argument_capacity_add_u64(words, 2u * (count + 1u), &words))
            return nil;
    }
    if (words > UINT32_MAX || words > UINT64_MAX / sizeof(uint32_t)) return nil;
    uint64_t allocatedBytes = words * sizeof(uint32_t);
    if (allocatedBytes > RDNA4R.lim.maxStorageBufferRange ||
        allocatedBytes > NSUIntegerMax) return nil;

    NSMutableData *tokens = [NSMutableData dataWithLength:
        (abi10 ? 11u : 7u) * sizeof(uint32_t)];
    uint32_t header[11] = {0};
    memcpy(header + 6, &lodBiasLimit, sizeof lodBiasLimit);
    for (unsigned category = 0; category < 3; ++category) {
        header[2 * category] = (uint32_t)(tokens.length / sizeof(uint32_t));
        header[2 * category + 1] = (uint32_t)rows[category].count + 1;
        uint64_t zero = 0; [tokens appendBytes:&zero length:sizeof zero];
        for (NSArray *row in rows[category]) {
            uint64_t token = [row[0] unsignedLongLongValue];
            [tokens appendBytes:&token length:sizeof token];
        }
    }
    if (samplerCoordinates) {
        for (NSUInteger i = 0; i < samplerFlags.count; ++i) {
            uint64_t flag = 0;
            if (!rdna4_number_u64((__bridge CFTypeRef)samplerFlags[i], &flag) ||
                flag > 1 || (!i && flag)) return nil;
            uint32_t word = (uint32_t)flag;
            [tokens appendBytes:&word length:sizeof word];
        }
    }
    if (imageQueries) {
        uint32_t dimensions[8] = {0}; [tokens appendBytes:dimensions length:sizeof dimensions];
        for (NSArray *row in rows[0]) {
            if (!rdna4_argument_image_query_row(row[1], dimensions,
                                                samplerCoordinates)) return nil;
            [tokens appendBytes:dimensions length:sizeof dimensions];
        }
    }
    if (abi10) for (unsigned texel = 0; texel < 2; ++texel) {
        if (!dynamicTexel[texel]) continue;
        header[7 + 2 * texel] = (uint32_t)(tokens.length / sizeof(uint32_t));
        header[8 + 2 * texel] = (uint32_t)texelRows[texel].count + 1u;
        uint64_t zero = 0;
        [tokens appendBytes:&zero length:sizeof zero];
        for (NSArray *row in texelRows[texel]) {
            uint64_t token = [row[0] unsignedLongLongValue];
            [tokens appendBytes:&token length:sizeof token];
        }
    }
    if (tokens.length != allocatedBytes) return nil;
    memcpy(tokens.mutableBytes, header,
           (abi10 ? 11u : 7u) * sizeof(uint32_t));
    return tokens;
}

static NSMutableData *rdna4_argument_table_bytes_abi(NSArray<NSArray *> *rows,
    BOOL imageQueries, float lodBiasLimit, BOOL samplerCoordinates,
    NSArray<NSNumber *> *samplerFlags) {
    return rdna4_argument_table_bytes_internal(rows, nil, NULL, NO,
        imageQueries, lodBiasLimit, samplerCoordinates, samplerFlags);
}

static NSMutableData *rdna4_argument_table_bytes_abi10(
    NSArray<NSArray *> *rows, NSArray<NSArray *> *texelRows,
    const BOOL dynamicTexel[2], BOOL imageQueries, float lodBiasLimit,
    BOOL samplerCoordinates, NSArray<NSNumber *> *samplerFlags) {
    if (!dynamicTexel) return nil;
    return rdna4_argument_table_bytes_internal(rows, texelRows, dynamicTexel,
        YES, imageQueries, lodBiasLimit, samplerCoordinates, samplerFlags);
}

static inline NSMutableData *rdna4_argument_table_bytes(NSArray<NSArray *> *rows,
    BOOL imageQueries, float lodBiasLimit) {
    return rdna4_argument_table_bytes_abi(rows, imageQueries, lodBiasLimit,
                                          NO, nil);
}

static NSData *rdna4_argument_cached_data(NSString *path, size_t limit) {
    int fd = open(path.fileSystemRepresentation, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return nil;
    struct stat info;
    if (fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_size < 0 || (uint64_t)info.st_size > limit) { close(fd); return nil; }
    NSMutableData *data = [NSMutableData dataWithLength:(NSUInteger)info.st_size];
    size_t offset = 0;
    while (offset < data.length) {
        ssize_t count = read(fd, (char *)data.mutableBytes + offset, data.length - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { close(fd); return nil; }
        offset += (size_t)count;
    }
    close(fd); return data;
}

static BOOL rdna4_argument_needs_postpass(NSDictionary *metadata) {
    if (![(id)metadata[@"bindings"] isKindOfClass:[NSArray class]]) return NO;
    for (id row in metadata[@"bindings"])
        if ([row isKindOfClass:[NSDictionary class]] &&
            ([row[@"kind"] isEqual:@"EmbeddedArgBufferTexture"] || [row[@"kind"] isEqual:@"EmbeddedArgBufferSampler"]) &&
            [row[@"descriptor"] isKindOfClass:[NSDictionary class]] &&
            [row[@"descriptor"][@"set"] isEqual:@0]) return YES;
    return NO;
}

static NSData *rdna4_gpu_argument_spv(NSData *spv, NSDictionary **metadata, uint64_t deadline, NSError **error) {
    if (!RDNA4R.gpuArgumentTables || rdna4_is_ws() || !rdna4x_active()) return spv;
    // A static null registry can coexist with flat fields that still need token lowering.
    if (!rdna4_argument_needs_postpass(*metadata)) return spv;
    NSData *input = [NSJSONSerialization dataWithJSONObject:*metadata options:NSJSONWritingSortedKeys error:error];
    if (!input) return nil;
    NSString *directory = rdna4x_user_dir();
    if (!directory || !rdna4x_spv_size_ok(spv.length) || !rdna4x_meta_size_ok(input.length)) {
        if (error) *error = rdna4_err(49, @"GPU argument input exceeds the translator caps or its cache is unavailable"); return nil;
    }
    const rdna4x_backend_capabilities queryCaps = rdna4_query_caps();
    uint8_t capHash[CC_SHA256_DIGEST_LENGTH]; char capHex[65];
    rdna4x_capabilities_hash(capHash, &queryCaps); rdna4x_hex(capHex, capHash, sizeof capHash);
    NSString *identity = [NSString stringWithFormat:@"rdna4x-argument-resources-47-dynamic-texel\n%s\n%@\n%@\n", capHex, rdna4_sha256hex(spv), rdna4_sha256hex(input)];
    NSString *sha = rdna4_sha256hex([identity dataUsingEncoding:NSUTF8StringEncoding]);
    NSString *codePath = [directory stringByAppendingPathComponent:[sha stringByAppendingString:@".spv"]];
    NSString *jsonPath = [directory stringByAppendingPathComponent:[sha stringByAppendingString:@".meta.json"]];
    NSData *result = rdna4_argument_cached_data(codePath, RDNA4X_MAX_SPV);
    if (!result) {
        rdna4x_engine_prepare();
        uint64_t now = rdna4_now(), left = deadline > now ? (deadline - now) / 1000000ull : 0;
        int waitMs = left > RDNA4G_SYNC_WAIT_APP_MS ? RDNA4G_SYNC_WAIT_APP_MS : (int)left;
        BOOL waiting = waitMs > 0 && rdna4_sync_begin(!rdna4_is_ws()); if (!waiting) waitMs = 0;
        char why[256] = {0};
        int rc = rdna4x_engine_lower_arguments(&RDNA4XL.eng, directory.fileSystemRepresentation, sha.UTF8String,
            spv.bytes, spv.length, input.bytes, input.length, waitMs, why, sizeof why);
        if (waiting) rdna4_sync_end(rc == RDNA4X_OK, rc == RDNA4X_E_TIMEOUT);
        if (rc != RDNA4X_OK) {
            NSString *reason = why[0] ? @(why) : rdna4x_cached_refusal(directory, sha) ?: @"no failure reason was reported";
            if (error) *error = rdna4_err(49, [NSString stringWithFormat:@"GPU argument lowering failed: code %d: %@", rc, reason]); return nil;
        }
        result = rdna4_argument_cached_data(codePath, RDNA4X_MAX_SPV);
    }
    NSData *json = rdna4_argument_cached_data(jsonPath, RDNA4X_MAX_META);
    id meta = json ? [NSJSONSerialization JSONObjectWithData:json options:0 error:error] : nil;
    if (!rdna4x_spv_size_ok(result.length) || !rdna4x_meta_size_ok(json.length) || ![meta isKindOfClass:[NSDictionary class]]) {
        if (error) *error = rdna4_err(49, @"GPU argument cache pair is missing or invalid");
        return nil;
    }
    *metadata = meta;
    return result;
}

static BOOL rdna4_argument_table_metadata_ex(
    const RDNA4sRefl *reflection, int stageSet, NSDictionary *metadata,
    rdna4ns_registry *outShapes, BOOL *outShapeRegistry, BOOL *outAbi10,
    BOOL outTexel[2], BOOL outDynamicTexel[2], NSError **error) {
    if (outShapes) *outShapes = (rdna4ns_registry){0};
    if (outShapeRegistry) *outShapeRegistry = NO;
    if (outAbi10) *outAbi10 = NO;
    if (outTexel) outTexel[0] = outTexel[1] = NO;
    if (outDynamicTexel) outDynamicTexel[0] = outDynamicTexel[1] = NO;

    BOOL reflectionVersionPresent = NO, normalized = NO;
    uint64_t reflectionVersion = 0;
    if (!rdna4_argument_reflection_version(metadata, &reflectionVersionPresent,
                                           &reflectionVersion)) {
        if (error) *error = rdna4_err(49,
            @"trimmed_from_reflection_version metadata is malformed");
        return NO;
    }
    if (!rdna4_argument_normalized_metadata(metadata, &normalized, error))
        return NO;
    id table = metadata[@"argument_resource_tables"];
    if (!table) {
        return YES;
    }
    uint64_t version = 0, binding = 0, queryBinding = 0;
    BOOL valid = RDNA4R.gpuArgumentTables && reflection && reflection->maxset <= 4 &&
        [table isKindOfClass:[NSDictionary class]] &&
        rdna4_number_u64((__bridge CFTypeRef)table[@"version"], &version) &&
        version >= 1 && version <= 10 &&
        rdna4_number_u64((__bridge CFTypeRef)table[@"table_binding"], &binding) &&
        binding == 672;
    if (version == 8)
        valid = valid && reflectionVersionPresent &&
            reflectionVersion == 64 && normalized;
    else if (version == 9)
        valid = valid && reflectionVersionPresent &&
            (reflectionVersion == 65 || reflectionVersion == 66) &&
            metadata[@"normalized_sampler_coordinates"] &&
            CFGetTypeID((__bridge CFTypeRef)metadata[@"normalized_sampler_coordinates"]) ==
                CFBooleanGetTypeID();
    else if (version == 10)
        valid = valid && reflectionVersionPresent && reflectionVersion == 66 &&
            metadata[@"normalized_sampler_coordinates"] &&
            CFGetTypeID((__bridge CFTypeRef)metadata[@"normalized_sampler_coordinates"]) ==
                CFBooleanGetTypeID();
    else
        valid = valid && !normalized &&
            (!reflectionVersionPresent || (reflectionVersion != 65 && reflectionVersion != 66) ||
             (version == 7 && !reflection->bind[stageSet][674].used &&
              !table[@"null_shapes"]));

    id roots = table[@"roots"];
    if (valid) {
        valid = version == 1
            ? [roots isKindOfClass:[NSArray class]] && [roots count] > 0
            : version >= 4 ? (!roots || [roots isKindOfClass:[NSArray class]])
                           : roots == nil;
    }

    const RDNA4sBind *main = &reflection->bind[stageSet][672];
    const RDNA4sBind *query = &reflection->bind[stageSet][673];
    BOOL hasQuery = table[@"image_query_binding"] != nil;
    if (valid && version < 9) {
        valid = main->used &&
            main->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER &&
            main->count == 1 && !main->runtime;
        if (version == 3)
            valid = valid && hasQuery &&
                rdna4_number_u64((__bridge CFTypeRef)
                                     table[@"image_query_binding"],
                                 &queryBinding) &&
                queryBinding == 673 &&
                query->used &&
                query->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER &&
                query->count == 1 && !query->runtime;
        else if (version >= 4)
            valid = valid && (hasQuery
                ? rdna4_number_u64((__bridge CFTypeRef)
                                       table[@"image_query_binding"],
                                   &queryBinding) &&
                      queryBinding == 673 && query->used &&
                      query->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER &&
                      query->count == 1 && !query->runtime
                : !query->used);
        else
            valid = valid && !hasQuery && !query->used;
    } else if (valid) {
        if (main->used)
            valid = main->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER &&
                main->count == 1 && !main->runtime;
        valid = valid && (hasQuery
            ? main->used &&
                  rdna4_number_u64((__bridge CFTypeRef)
                                       table[@"image_query_binding"],
                                   &queryBinding) &&
                  queryBinding == 673 && query->used &&
                  query->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER &&
                  query->count == 1 && !query->runtime
            : !query->used);
    }

    NSMutableSet *rootSet = [NSMutableSet set];
    if (valid && roots) for (id root in roots) {
        uint64_t buffer = 0, slot = 0;
        if (![root isKindOfClass:[NSDictionary class]] ||
            !rdna4_number_u64((__bridge CFTypeRef)root[@"buffer_index"],
                              &buffer) ||
            buffer >= 31 ||
            !rdna4_number_u64((__bridge CFTypeRef)root[@"binding"], &slot) ||
            slot != 704 + buffer || [rootSet containsObject:@(buffer)]) {
            valid = NO;
            break;
        }
        const RDNA4sBind *descriptor =
            &reflection->bind[stageSet][slot];
        if (!descriptor->used ||
            descriptor->type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
            descriptor->count != 1 || descriptor->runtime) {
            valid = NO;
            break;
        }
        [rootSet addObject:@(buffer)];
    }
    if (valid) for (unsigned i = 0; i < 31; ++i)
        if (reflection->bind[stageSet][704 + i].used !=
            [rootSet containsObject:@(i)])
            valid = NO;

    id comparison = table[@"comparison_sampler"];
    if (valid)
        valid = version >= 7
            ? comparison &&
                  CFGetTypeID((__bridge CFTypeRef)comparison) ==
                      CFBooleanGetTypeID()
            : comparison == nil;

    BOOL categories[5] = {NO};
    if (valid && version >= 2) {
        id declared = table[@"categories"];
        valid = [declared isKindOfClass:[NSArray class]] &&
            [declared count] <= (version == 10 ? 5u : 3u);
        if (valid && version < 9)
            valid = [declared count] > 0 ||
                (version >= 3 && table[@"image_query_binding"] != nil);
        if (valid && version >= 9 && !main->used)
            valid = !hasQuery && (!roots || [roots count] == 0);
        uint64_t previous = 0;
        BOOL first = YES;
        if (valid) for (id value in declared) {
            uint64_t category = 0;
            if (!rdna4_number_u64((__bridge CFTypeRef)value, &category) ||
                category > (version == 10 ? 4u : 2u) ||
                (!first && category <= previous)) {
                valid = NO;
                break;
            }
            categories[category] = YES;
            previous = category;
            first = NO;
        }
        if (valid && version == 10 && !categories[3] && !categories[4])
            valid = NO;
    }

    rdna4ns_registry shapes = {0};
    if (valid && version >= 9) {
        id declared = table[@"null_shapes"];
        valid = [declared isKindOfClass:[NSArray class]] &&
            [declared count] <= RDNA4NS_MAX_SHAPES;
        uint32_t words[RDNA4NS_MAX_SHAPES] = {0};
        NSUInteger count = valid ? [declared count] : 0;
        if (valid) for (NSUInteger i = 0; i < count; ++i) {
            uint64_t word = 0;
            if (!rdna4_number_u64((__bridge CFTypeRef)declared[i], &word) ||
                word > UINT32_MAX) {
                valid = NO;
                break;
            }
            words[i] = (uint32_t)word;
        }
        valid = valid &&
            rdna4ns_registry_build_abi((uint32_t)version, words, count,
                                       &shapes) == RDNA4NS_OK;
        if (valid && count) {
            const RDNA4sBind *stageMetadata = &reflection->bind[stageSet][674];
            valid = stageMetadata->used &&
                stageMetadata->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER &&
                stageMetadata->count == 1 && !stageMetadata->runtime;
        }
        rdna4ns_registry reflected = {0};
        if (valid) {
            valid = !reflection->argumentNullShapesInvalid &&
                rdna4ns_registry_build(
                    reflection->argumentNullShapes,
                    reflection->argumentNullShapeCount, &reflected) ==
                    RDNA4NS_OK;
        }
        for (size_t i = 0; valid && i < shapes.count; ++i)
            valid = rdna4ns_registry_lookup(
                        &reflected, shapes.words[i], NULL, NULL) ==
                RDNA4NS_OK;
    }

    if (valid) for (id row in metadata[@"bindings"]) {
        if (![row isKindOfClass:[NSDictionary class]]) {
            valid = NO;
            break;
        }
        BOOL sampler = [row[@"kind"] isEqual:@"EmbeddedArgBufferSampler"];
        if (!sampler &&
            ![row[@"kind"] isEqual:@"EmbeddedArgBufferTexture"])
            continue;
        if (version != 1 && version < 4) {
            valid = NO;
            break;
        }
        id descriptor = row[@"descriptor"], source = row[@"embedded_source"];
        BOOL texelTexture = !sampler &&
            [row[@"texture_shape"] isKindOfClass:[NSDictionary class]] &&
            [row[@"texture_shape"][@"dimension"] isEqual:@"Buffer"];
        if (!sampler &&
            (![row[@"texture_shape"] isKindOfClass:[NSDictionary class]] ||
             !row[@"texture_shape"][@"writable"] ||
             CFGetTypeID((__bridge CFTypeRef)
                             row[@"texture_shape"][@"writable"]) !=
                 CFBooleanGetTypeID())) {
            valid = NO;
            break;
        }
        uint64_t set = 0, slot = 0, count = 0, buffer = 0, offset = 0;
        unsigned category = sampler ? 2 : texelTexture
            ? ([row[@"texture_shape"][@"writable"] boolValue] ? 4u : 3u)
            : ([row[@"texture_shape"][@"writable"] boolValue] ? 1u : 0u);
        uint64_t expectedSet = category == 3 ? 2 : category == 4 ? 3 : 2 + category;
        uint64_t expectedBinding = category >= 3 ? 1 : 0;
        if (![descriptor isKindOfClass:[NSDictionary class]] ||
            ![source isKindOfClass:[NSDictionary class]] ||
            !rdna4_number_u64((__bridge CFTypeRef)descriptor[@"set"],
                              &set) ||
            set != expectedSet ||
            !rdna4_number_u64((__bridge CFTypeRef)descriptor[@"binding"],
                              &slot) ||
            slot != expectedBinding ||
            !rdna4_number_u64((__bridge CFTypeRef)descriptor[@"count"],
                              &count) ||
            count ||
            !rdna4_number_u64((__bridge CFTypeRef)source[@"buffer_index"],
                              &buffer) ||
            ![rootSet containsObject:@(buffer)] ||
            !rdna4_number_u64((__bridge CFTypeRef)source[@"field_offset"],
                              &offset) ||
            offset > UINT32_MAX || offset % 8) {
            valid = NO;
            break;
        }
        id paired = row[@"comparison_descriptor"];
        if (paired) {
            uint64_t pairedSet = 0, pairedSlot = 0, pairedCount = 0;
            if (!sampler || version < 7 ||
                ![table[@"comparison_sampler"] boolValue] ||
                ![paired isKindOfClass:NSDictionary.class] ||
                !rdna4_number_u64((__bridge CFTypeRef)paired[@"set"],
                                  &pairedSet) ||
                pairedSet != 4 ||
                !rdna4_number_u64((__bridge CFTypeRef)paired[@"binding"],
                                  &pairedSlot) ||
                pairedSlot ||
                !rdna4_number_u64((__bridge CFTypeRef)paired[@"count"],
                                  &pairedCount) ||
                pairedCount) {
                valid = NO;
                break;
            }
        }
        categories[category] = YES;
    }

    BOOL texel[2] = {NO};
    BOOL dynamicTexel[2] = {categories[3], categories[4]};
    if (valid && version >= 9) {
        texel[0] = reflection->bind[2][1].used;
        texel[1] = reflection->bind[3][1].used;
        if (texel[0])
            valid = reflection->bind[2][1].type ==
                    VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER &&
                reflection->bind[2][1].runtime &&
                reflection->bind[2][1].texelComponentKinds != 0;
        if (valid && texel[1])
            valid = reflection->bind[3][1].type ==
                    VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER &&
                reflection->bind[3][1].runtime &&
                reflection->bind[3][1].texelComponentKinds != 0;
        rdna4ns_registry reflected = {0};
        if (valid)
            valid = !reflection->argumentNullShapesInvalid &&
                rdna4ns_registry_build(
                    reflection->argumentNullShapes,
                    reflection->argumentNullShapeCount, &reflected) ==
                    RDNA4NS_OK;
        if (valid && dynamicTexel[0])
            valid = main->used && texel[0] && rdna4ns_registry_access_count(
                &reflected, RDNA4NS_ACCESS_UNIFORM_TEXEL) > 0;
        if (valid && dynamicTexel[1])
            valid = main->used && texel[1] && rdna4ns_registry_access_count(
                &reflected, RDNA4NS_ACCESS_STORAGE_TEXEL) > 0;
        if (valid && !texel[0] && shapes.access_count[RDNA4NS_ACCESS_UNIFORM_TEXEL])
            valid = NO;
        if (valid && !texel[1] && shapes.access_count[RDNA4NS_ACCESS_STORAGE_TEXEL])
            valid = NO;
    }
    if (valid) for (unsigned category = 0; category < 3; ++category) {
        const RDNA4sBind *slot = &reflection->bind[2 + category][0];
        if (slot->used != categories[category] ||
            (slot->used &&
             (slot->type != rdna4_argument_descriptor_type(category) ||
              !slot->runtime))) {
            valid = NO;
            break;
        }
        for (unsigned bindingIndex = 1;
             bindingIndex < RDNA4S_MAXBIND; ++bindingIndex) {
            BOOL expected = version >= 9 && category < 2 &&
                bindingIndex == 1 && texel[category];
            if (reflection->bind[2 + category][bindingIndex].used !=
                expected) {
                valid = NO;
                break;
            }
        }
        if (!valid) break;
    }
    if (valid && [table[@"comparison_sampler"] boolValue] &&
        !categories[2])
        valid = NO;
    if (!valid) {
        if (error) *error = rdna4_err(
            49,
            @"GPU argument table metadata does not match the reflected stage");
        return NO;
    }
    if (outShapes) *outShapes = shapes;
    if (outShapeRegistry) *outShapeRegistry = version >= 9;
    if (outAbi10) *outAbi10 = version == 10;
    if (outTexel) outTexel[0] = texel[0], outTexel[1] = texel[1];
    if (outDynamicTexel)
        outDynamicTexel[0] = dynamicTexel[0], outDynamicTexel[1] = dynamicTexel[1];
    return YES;
}

static BOOL rdna4_argument_table_metadata(
    const RDNA4sRefl *reflection, int stageSet, NSDictionary *metadata,
    NSError **error) {
    return rdna4_argument_table_metadata_ex(
        reflection, stageSet, metadata, NULL, NULL, NULL, NULL, NULL, error);
}

static BOOL rdna4_argument_table_metadata_apply(
    RDNA4ArgumentTables *tables, const RDNA4sRefl *reflection,
    int stageSet, NSDictionary *metadata, NSError **error) {
    rdna4ns_registry shapes = {0};
    BOOL shapeRegistry = NO, abi10 = NO;
    BOOL texel[2] = {NO}, dynamicTexel[2] = {NO};
    if (!rdna4_argument_table_metadata_ex(
            reflection, stageSet, metadata, &shapes, &shapeRegistry,
            &abi10, texel, dynamicTexel, error))
        return NO;
    if (!shapeRegistry) return YES;
    if (reflection && reflection->bind[stageSet][672].used) {
        BOOL normalized = [metadata[@"normalized_sampler_coordinates"] boolValue];
        if (tables->tokenTableSeen && tables->tokenTableNormalized != normalized) {
            if (error) *error = rdna4_err(49,
                @"GPU argument stages use different sampler coordinate table layouts");
            return NO;
        }
        tables->tokenTableSeen = YES;
        tables->tokenTableNormalized = normalized;
        tables->samplerCoordinates = normalized;
    }
    if (tables->shapeRegistry || tables->nullShapes.count) {
        rdna4ns_registry merged = {0};
        if (rdna4ns_registry_merge(&tables->nullShapes, &shapes,
                                   &merged) != RDNA4NS_OK) {
            if (error) *error = rdna4_err(
                49, @"GPU argument stages require a null-shape ordinal remap");
            return NO;
        }
        tables->nullShapes = merged;
    } else {
        tables->nullShapes = shapes;
    }
    tables->shapeRegistry = YES;
    tables->abi10 |= abi10;
    tables->texel[0] |= texel[0];
    tables->texel[1] |= texel[1];
    if (reflection) {
        tables->texelComponents[0] |=
            reflection->bind[2][1].texelComponentKinds;
        tables->texelComponents[1] |=
            reflection->bind[3][1].texelComponentKinds;
    }
    tables->dynamicTexel[0] |= dynamicTexel[0];
    tables->dynamicTexel[1] |= dynamicTexel[1];
    return YES;
}

static void rdna4_argument_capacity_from_device(rdna4_argument_capacity_limits *limits) {
    const VkPhysicalDeviceVulkan12Properties *source = &RDNA4R.descriptorLimits;
    *limits = (rdna4_argument_capacity_limits){
        .max_per_stage_uab = {
            source->maxPerStageDescriptorUpdateAfterBindSampledImages,
            source->maxPerStageDescriptorUpdateAfterBindStorageImages,
            source->maxPerStageDescriptorUpdateAfterBindSamplers },
        .max_pipeline_uab = {
            source->maxDescriptorSetUpdateAfterBindSampledImages,
            source->maxDescriptorSetUpdateAfterBindStorageImages,
            source->maxDescriptorSetUpdateAfterBindSamplers },
        .max_per_stage_uab_resources =
            source->maxPerStageUpdateAfterBindResources,
        .max_per_stage_ordinary = {
            RDNA4R.lim.maxPerStageDescriptorSampledImages,
            RDNA4R.lim.maxPerStageDescriptorStorageImages,
            RDNA4R.lim.maxPerStageDescriptorSamplers },
        .max_pipeline_ordinary = {
            RDNA4R.lim.maxDescriptorSetSampledImages,
            RDNA4R.lim.maxDescriptorSetStorageImages,
            RDNA4R.lim.maxDescriptorSetSamplers },
        .max_per_stage_ordinary_resources = RDNA4R.lim.maxPerStageResources,
        .max_per_set_descriptors = RDNA4R.maxPerSetDescriptors,
        .max_bound_descriptor_sets = RDNA4R.lim.maxBoundDescriptorSets,
    };
}

static BOOL rdna4_argument_descriptor_validate(
    const rdna4_argument_descriptor_usage *usage, BOOL uab, NSError **error) {
    rdna4_argument_descriptor_limits limits = rdna4_argument_descriptor_device_limits(
        &RDNA4R.lim, &RDNA4R.descriptorLimits, uab);
    rdna4_argument_descriptor_budget_plan plan;
    rdna4_argument_descriptor_budget_status status =
        rdna4_argument_capacity_descriptor_budget(&limits, usage, &plan);
    if (status == RDNA4_ARGUMENT_DESCRIPTOR_BUDGET_OK) return YES;
    if (error) *error = rdna4_err(49, [NSString stringWithFormat:
        @"%@ descriptor budget rejected (status %u, stage %u, group %u)",
        uab ? @"ordinary plus UAB" : @"ordinary", (unsigned)status,
        plan.failed_stage, plan.failed_group]);
    return NO;
}

static BOOL rdna4_argument_allocation_budget(const RDNA4ArgumentTables *tables,
    const uint32_t descriptors[3], const uint32_t texelDescriptors[2],
    NSError **error) {
    if (!tables->descriptorBudgetReady) return YES;
    rdna4_argument_descriptor_usage usage;
    if (!rdna4_argument_descriptor_tables(&tables->ordinaryUsage,
            tables->tableStages, tables->texelStages, descriptors,
            texelDescriptors, &usage)) {
        if (error) *error = rdna4_err(49, @"GPU argument descriptor totals overflow");
        return NO;
    }
    return rdna4_argument_descriptor_validate(&usage, YES, error);
}

static BOOL rdna4_argument_capacity_scan(RDNA4ArgumentTables *tables,
    const RDNA4sRefl *vertex, const RDNA4sRefl *fragment, BOOL compute,
    rdna4_argument_capacity_limits *limits, NSError **error) {
    const RDNA4sRefl *stages[] = {vertex, fragment};
    memset(tables->tableStages, 0, sizeof tables->tableStages);
    tables->texelStages[0] = tables->texelStages[1] = 0;
    tables->imageTable[0] = tables->imageTable[1] =
        tables->imageTable[2] = NO;
    BOOL active = tables->imageQueries;
    if (!rdna4_argument_descriptor_scan(vertex, fragment,
            tables->fragmentColorAttachments, &tables->ordinaryUsage)) {
        if (error) *error = rdna4_err(49, @"reflected descriptor counts are invalid or overflow");
        return NO;
    }
    if (tables->descriptorBudgetReady &&
        !rdna4_argument_descriptor_validate(&tables->ordinaryUsage, NO, error))
        return NO;
    for (unsigned stage = 0; stage < 2; ++stage) {
        const RDNA4sRefl *reflection = stages[stage];
        if (!reflection) continue;
        if (reflection->maxset > 4) {
            if (error) *error = rdna4_err(49, @"GPU argument tables exceed descriptor set 4");
            return NO;
        }
        tables->imageQueries |= reflection->bind[stage][673].used;
        tables->nullImages |= reflection->argumentNullImages;
        active |= tables->imageQueries;
        for (unsigned category = 0; category < 3; ++category)
            if (reflection->bind[2 + category][0].used) {
                tables->imageTable[category] = YES;
                tables->tableStages[category] |= 1u << stage;
                limits->used_stages[category] |= 1u << stage;
                active = YES;
            }
        for (unsigned category = 0; category < 2; ++category)
            if (tables->shapeRegistry && reflection->bind[2 + category][1].used) {
                tables->texel[category] = YES;
                tables->texelStages[category] |= 1u << stage;
                uint64_t reserved = tables->dynamicTexel[category]
                    ? 1u + rdna4ns_registry_access_count(&tables->nullShapes,
                        category ? RDNA4NS_ACCESS_STORAGE_TEXEL
                                 : RDNA4NS_ACCESS_UNIFORM_TEXEL)
                    : RDNA4NS_TEXEL_SHAPES;
                if (!rdna4_argument_capacity_add_u64(
                        limits->fixed[stage][category], reserved,
                        &limits->fixed[stage][category]) ||
                    !rdna4_argument_capacity_add_u64(
                        limits->fixed_resources[stage], reserved,
                        &limits->fixed_resources[stage])) {
                    if (error) *error = rdna4_err(49,
                        @"GPU argument texel descriptor reserve overflows");
                    return NO;
                }
                if (tables->dynamicTexel[category])
                    limits->used_stages[category] |= 1u << stage;
                active = YES;
            }
        uint64_t groups[RDNA4_ARGUMENT_DESCRIPTOR_TYPE_GROUPS], resources = 0;
        if (!rdna4_argument_descriptor_counts_by_group(
                &tables->ordinaryUsage.per_stage[stage], groups, &resources) ||
            (stage == 1 && !rdna4_argument_capacity_add_u64(resources,
                tables->fragmentColorAttachments, &resources))) {
            if (error) *error = rdna4_err(49, @"GPU argument direct descriptor count overflows");
            return NO;
        }
        limits->direct_resources[stage] = resources;
        limits->direct[stage][0] = groups[RDNA4_ARGUMENT_DESCRIPTOR_GROUP_SAMPLED_IMAGES];
        limits->direct[stage][1] = groups[RDNA4_ARGUMENT_DESCRIPTOR_GROUP_STORAGE_IMAGES];
        limits->direct[stage][2] = groups[RDNA4_ARGUMENT_DESCRIPTOR_GROUP_SAMPLERS];
    }
    for (unsigned category = 0; category < 3; ++category) {
        if (category < 2 && tables->texelStages[category]) {
            limits->fixed_pipeline[category] = tables->dynamicTexel[category]
                ? 1u + rdna4ns_registry_access_count(&tables->nullShapes,
                    category ? RDNA4NS_ACCESS_STORAGE_TEXEL
                             : RDNA4NS_ACCESS_UNIFORM_TEXEL)
                : RDNA4NS_TEXEL_SHAPES;
        }
    }
    tables->active = active;
    if (tables->comparisonSamplers && !limits->used_stages[2]) {
        if (error) *error = rdna4_err(49, @"comparison sampler metadata has no sampler table");
        return NO;
    }
    (void)compute;
    return YES;
}

static NSError *rdna4_argument_capacity_error(rdna4_argument_capacity_status status,
                                              unsigned category) {
    NSString *detail = category < 3
        ? [NSString stringWithFormat:@"category %u", category] : @"all categories";
    switch (status) {
    case RDNA4_ARGUMENT_CAPACITY_LIMIT_UNAVAILABLE:
        return rdna4_err(49, @"backend descriptor-set size limit was not queried");
    case RDNA4_ARGUMENT_CAPACITY_OVERFLOW:
        return rdna4_err(49, [NSString stringWithFormat:@"%@ descriptor capacity exceeds the table representation", detail]);
    case RDNA4_ARGUMENT_CAPACITY_SET_COUNT:
        return rdna4_err(49, @"backend has fewer than five descriptor sets for GPU arguments");
    case RDNA4_ARGUMENT_CAPACITY_DIRECT_OVER_LIMIT:
        return rdna4_err(49, [NSString stringWithFormat:@"direct %@ descriptors exceed the per-stage limit", detail]);
    case RDNA4_ARGUMENT_CAPACITY_ORDINARY_DIRECT_OVER_LIMIT:
        return rdna4_err(49, [NSString stringWithFormat:@"ordinary %@ descriptors exceed the non-UAB descriptor limit", detail]);
    case RDNA4_ARGUMENT_CAPACITY_RESOURCE_OVER_LIMIT:
        return rdna4_err(49, @"direct and indirect GPU argument resources exceed the per-stage limit");
    case RDNA4_ARGUMENT_CAPACITY_ORDINARY_RESOURCE_OVER_LIMIT:
        return rdna4_err(49, @"ordinary GPU argument resources exceed the non-UAB per-stage limit");
    case RDNA4_ARGUMENT_CAPACITY_NO_DESCRIPTORS:
        return rdna4_err(49, [NSString stringWithFormat:@"backend has no remaining GPU argument descriptors for %@", detail]);
    default:
        return rdna4_err(49, @"GPU argument capacity input is invalid");
    }
}

static BOOL rdna4_argument_table_layouts_limits(RDNA4ArgumentTables *tables,
    const RDNA4sRefl *vertex, const RDNA4sRefl *fragment, BOOL compute,
    const rdna4_argument_capacity_limits *deviceLimits, NSError **error) {
    if (!tables || !deviceLimits) {
        if (error) *error = rdna4_err(49, @"GPU argument capacity limits are unavailable");
        return NO;
    }
    RDNA4ArgumentTables built = *tables;
    for (unsigned category = 0; category < 3; ++category) {
        built.dsl[category] = VK_NULL_HANDLE;
        built.capacity[category] = 0;
        built.variable[category] = NO;
        if (category < 2) {
            built.texelCapacity[category] = 0;
            built.texelVariable[category] = NO;
        }
    }
    built.active = NO;

    rdna4_argument_capacity_limits limits = *deviceLimits;
    if (!rdna4_argument_capacity_scan(&built, vertex, fragment, compute, &limits, error))
        goto fail;
    if (!built.active) {
        uint32_t noTexelDescriptors[2] = {0};
        if (!rdna4_argument_allocation_budget(&built, built.capacity,
                noTexelDescriptors, error)) goto fail;
        *tables = built;
        return YES;
    }

    rdna4_argument_capacity_plan capacity = {0};
    rdna4_argument_capacity_status status =
        rdna4_argument_capacity_calculate(&limits, &capacity);
    if (status != RDNA4_ARGUMENT_CAPACITY_OK) {
        if (error) *error = rdna4_argument_capacity_error(status, capacity.failed_category);
        goto fail;
    }
    if (built.descriptorBudgetReady &&
        ((built.tableStages[1] | built.texelStages[1]) & 2u)) {
        uint64_t groups[RDNA4_ARGUMENT_DESCRIPTOR_TYPE_GROUPS], resources = 0;
        uint64_t outputs = 0;
        if (!rdna4_argument_descriptor_counts_by_group(
                &built.ordinaryUsage.per_stage[1], groups, &resources) ||
            !rdna4_argument_capacity_add_u64(
                groups[RDNA4_ARGUMENT_DESCRIPTOR_GROUP_STORAGE_BUFFERS],
                groups[RDNA4_ARGUMENT_DESCRIPTOR_GROUP_STORAGE_IMAGES], &outputs) ||
            !rdna4_argument_capacity_add_u64(outputs,
                built.fragmentColorAttachments, &outputs) ||
            !rdna4_argument_capacity_add_u64(outputs,
                built.texelStages[1] & 2u
                    ? (built.dynamicTexel[1]
                        ? 1u + rdna4ns_registry_access_count(&built.nullShapes,
                            RDNA4NS_ACCESS_STORAGE_TEXEL)
                        : RDNA4NS_TEXEL_SHAPES)
                    : 0, &outputs) ||
            outputs > RDNA4R.lim.maxFragmentCombinedOutputResources) {
            if (error) *error = rdna4_err(49, @"fragment output resource budget exhausted");
            goto fail;
        }
        uint64_t headroom = RDNA4R.lim.maxFragmentCombinedOutputResources - outputs;
        if (capacity.capacity[1] > headroom)
            capacity.capacity[1] = (uint32_t)headroom;
        if ((built.imageTable[1] || built.dynamicTexel[1]) &&
            !capacity.capacity[1]) {
            if (error) *error = rdna4_err(49,
                @"fragment storage table has no output resource headroom");
            goto fail;
        }
    }
    for (unsigned category = 0; category < 3; ++category) {
        built.capacity[category] = built.imageTable[category]
            ? capacity.capacity[category] : 0;
        if (category >= 2 || !built.dynamicTexel[category]) continue;
        uint64_t reserve = 1u + rdna4ns_registry_access_count(
            &built.nullShapes, category ? RDNA4NS_ACCESS_STORAGE_TEXEL
                                        : RDNA4NS_ACCESS_UNIFORM_TEXEL);
        uint64_t available = capacity.capacity[category];
        uint64_t imageCapacity = 0;
        if (built.imageTable[category]) {
            if (available < 2) {
                if (error) *error = rdna4_err(49,
                    @"image and texel argument tables cannot both fit");
                goto fail;
            }
            imageCapacity = available / 2u;
            if (!imageCapacity) imageCapacity = 1;
            built.capacity[category] = (uint32_t)imageCapacity;
            available -= imageCapacity;
        }
        uint64_t texelCapacity = reserve + available;
        if (texelCapacity > UINT32_MAX) {
            if (error) *error = rdna4_err(49,
                @"GPU argument texel capacity exceeds the descriptor ABI");
            goto fail;
        }
        built.texelCapacity[category] = (uint32_t)texelCapacity;
        built.texelVariable[category] = YES;
    }
    uint32_t layoutTexelDescriptors[2] = {0};
    for (unsigned category = 0; category < 2; ++category)
        layoutTexelDescriptors[category] = built.texel[category]
            ? (built.texelVariable[category] ? built.texelCapacity[category]
                                             : RDNA4NS_TEXEL_SHAPES)
            : 0;
    if (!rdna4_argument_allocation_budget(&built, built.capacity,
            layoutTexelDescriptors, error)) goto fail;

    for (unsigned category = 0; category < 3; ++category) {
        VkShaderStageFlags stageFlags =
            rdna4_argument_stage_flags(built.tableStages[category], compute);
        VkShaderStageFlags texelStageFlags = category < 2
            ? rdna4_argument_stage_flags(built.texelStages[category], compute)
            : 0;
        BOOL used = built.imageTable[category] && stageFlags != 0 &&
            built.capacity[category] != 0;
        BOOL texel = texelStageFlags != 0 && category < 2 &&
            built.texel[category];
        BOOL dynamicTexel = texel && built.dynamicTexel[category];
        VkDescriptorSetLayoutBinding bindings[2];
        VkDescriptorBindingFlags flags[2];
        uint32_t bindingCount = 0, variableBinding = UINT32_MAX;
        if (used) {
            bindings[bindingCount] = (VkDescriptorSetLayoutBinding){
                0, rdna4_argument_descriptor_type(category),
                built.capacity[category], stageFlags, NULL };
            flags[bindingCount++] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
            if (!texel) variableBinding = 0;
        }
        if (texel) {
            if (dynamicTexel) variableBinding = bindingCount;
            bindings[bindingCount] = (VkDescriptorSetLayoutBinding){
                1,
                category ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER
                         : VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
                dynamicTexel ? built.texelCapacity[category]
                             : RDNA4NS_TEXEL_SHAPES,
                texelStageFlags, NULL };
            flags[bindingCount] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
            if (dynamicTexel)
                flags[bindingCount] |=
                    VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;
            ++bindingCount;
        }
        if (variableBinding != UINT32_MAX && !dynamicTexel)
            flags[variableBinding] |=
                VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;
        VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlags = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
            .bindingCount = bindingCount,
            .pBindingFlags = flags };
        VkDescriptorSetLayoutCreateInfo info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .pNext = &bindingFlags,
            .flags = (used || texel)
                ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT
                : 0,
            .bindingCount = bindingCount,
            .pBindings = bindings };
        if (used && !texel) {
            VkDescriptorSetVariableDescriptorCountLayoutSupport variable = {
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_LAYOUT_SUPPORT };
            VkDescriptorSetLayoutSupport support = {
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT, .pNext = &variable };
            bindings[variableBinding].descriptorCount = 1;
            if (vkGetDescriptorSetLayoutSupport)
                vkGetDescriptorSetLayoutSupport(RDNA4R.dev, &info, &support);
            if (!support.supported || !variable.maxVariableDescriptorCount) {
                if (error) *error = rdna4_err(49, @"backend cannot create the GPU argument descriptor layout");
                goto fail;
            }
            built.variable[category] = YES;
            if (variable.maxVariableDescriptorCount < built.capacity[category]) {
                if (category == 2) {
                    bindings[variableBinding].descriptorCount =
                        built.capacity[category];
                    flags[variableBinding] &=
                        ~VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;
                    support.pNext = NULL;
                    support.supported = VK_FALSE;
                    if (vkGetDescriptorSetLayoutSupport)
                        vkGetDescriptorSetLayoutSupport(RDNA4R.dev, &info, &support);
                    if (support.supported) built.variable[category] = NO;
                    else flags[variableBinding] |=
                        VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;
                }
                if (built.variable[category])
                    built.capacity[category] = variable.maxVariableDescriptorCount;
            }
            bindings[variableBinding].descriptorCount =
                built.capacity[category];
            if (!built.capacity[category]) {
                if (error) *error = rdna4_err(49, @"backend reported a zero GPU argument descriptor layout limit");
                goto fail;
            }
        }
        if (dynamicTexel) {
            VkDescriptorSetVariableDescriptorCountLayoutSupport variable = {
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_LAYOUT_SUPPORT };
            VkDescriptorSetLayoutSupport support = {
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT,
                .pNext = &variable };
            uint32_t minimum = 1u + rdna4ns_registry_access_count(
                &built.nullShapes, category ? RDNA4NS_ACCESS_STORAGE_TEXEL
                                            : RDNA4NS_ACCESS_UNIFORM_TEXEL);
            bindings[variableBinding].descriptorCount = 1;
            if (vkGetDescriptorSetLayoutSupport)
                vkGetDescriptorSetLayoutSupport(RDNA4R.dev, &info, &support);
            if (!support.supported || variable.maxVariableDescriptorCount < minimum) {
                if (error) *error = rdna4_err(49,
                    @"backend cannot create a variable texel argument descriptor layout");
                goto fail;
            }
            if (built.texelCapacity[category] >
                variable.maxVariableDescriptorCount)
                built.texelCapacity[category] =
                    variable.maxVariableDescriptorCount;
            bindings[variableBinding].descriptorCount =
                built.texelCapacity[category];
            support.pNext = NULL;
            support.supported = VK_FALSE;
            if (vkGetDescriptorSetLayoutSupport)
                vkGetDescriptorSetLayoutSupport(RDNA4R.dev, &info, &support);
            if (!support.supported) {
                if (error) *error = rdna4_err(49,
                    @"backend rejected the final texel argument descriptor layout");
                goto fail;
            }
            built.texelVariable[category] = YES;
        } else if (used && texel) {
            VkDescriptorSetLayoutSupport support = {
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT };
            if (vkGetDescriptorSetLayoutSupport)
                vkGetDescriptorSetLayoutSupport(RDNA4R.dev, &info, &support);
            if (!support.supported) {
                if (error) *error = rdna4_err(49,
                    @"backend cannot create the fixed image and texel argument layout");
                goto fail;
            }
        }
        VkResult result = vkCreateDescriptorSetLayout(RDNA4R.dev, &info, NULL, &built.dsl[category]);
        if (result != VK_SUCCESS) {
            if (error) *error = rdna4_err(49, [NSString stringWithFormat:@"GPU argument descriptor layout = %d", result]);
            goto fail;
        }
    }
    uint32_t finalTexelDescriptors[2] = {0};
    for (unsigned category = 0; category < 2; ++category)
        finalTexelDescriptors[category] = built.texel[category]
            ? (built.texelVariable[category] ? built.texelCapacity[category]
                                             : RDNA4NS_TEXEL_SHAPES)
            : 0;
    if (!rdna4_argument_allocation_budget(&built, built.capacity,
            finalTexelDescriptors, error)) goto fail;
    *tables = built;
    return YES;

fail:
    rdna4_argument_tables_destroy(&built);
    return NO;
}

static BOOL rdna4_argument_table_layouts(RDNA4ArgumentTables *tables, const RDNA4sRefl *vertex,
    const RDNA4sRefl *fragment, BOOL compute, NSError **error) {
    if (!tables) {
        if (error) *error = rdna4_err(49, @"GPU argument tables are unavailable");
        return NO;
    }
    rdna4_argument_capacity_limits limits = {0};
    rdna4_argument_capacity_from_device(&limits);
    RDNA4ArgumentTables candidate = *tables;
    candidate.descriptorBudgetReady = YES;
    if (!rdna4_argument_table_layouts_limits(&candidate, vertex, fragment, compute, &limits, error))
        return NO;
    *tables = candidate;
    return YES;
}

static void rdna4_argument_tables_destroy(RDNA4ArgumentTables *tables) {
    if (!tables) return;
    for (unsigned category = 0; category < 3; ++category) {
        if (RDNA4R.ok && vkDestroyDescriptorSetLayout && tables->dsl[category])
            vkDestroyDescriptorSetLayout(RDNA4R.dev, tables->dsl[category], NULL);
        tables->dsl[category] = VK_NULL_HANDLE;
    }
    tables->capacity[0] = tables->capacity[1] = tables->capacity[2] = 0;
    tables->variable[0] = tables->variable[1] = tables->variable[2] = NO;
    tables->active = NO;
    tables->imageQueries = NO;
    tables->comparisonSamplers = NO;
    tables->samplerCoordinates = NO;
    tables->nullImages = 0;
    tables->shapeRegistry = NO;
    tables->abi10 = NO;
    tables->texel[0] = tables->texel[1] = NO;
    tables->texelComponents[0] = tables->texelComponents[1] = 0;
    tables->dynamicTexel[0] = tables->dynamicTexel[1] = NO;
    tables->texelVariable[0] = tables->texelVariable[1] = NO;
    tables->texelCapacity[0] = tables->texelCapacity[1] = 0;
    tables->imageTable[0] = tables->imageTable[1] =
        tables->imageTable[2] = NO;
    tables->texelStages[0] = tables->texelStages[1] = 0;
    tables->nullShapes = (rdna4ns_registry){0};
    memset(tables->tableStages, 0, sizeof(tables->tableStages));
    tables->ordinaryUsage = (rdna4_argument_descriptor_usage){0};
    tables->fragmentColorAttachments = 0;
    tables->descriptorBudgetReady = NO;
}

static BOOL rdna4_argument_stage_remap(const RDNA4ArgumentTables *tables,
    RDNA4PB *bindings, uint32_t count, NSError **error) {
    for (uint32_t binding = 0; binding < count; ++binding) {
        RDNA4PB *slot = &bindings[binding];
        if (!slot->stageResourceMetadata) continue;
        if (rdna4ns_registry_remap(&tables->nullShapes, slot->nullShapeRemap,
                slot->nullShapeCount, slot->nullShapeRemap,
                slot->nullShapeAccess) != RDNA4NS_OK) {
            if (error) *error = rdna4_err(49, @"stage null-shape remap has no shared descriptor");
            return NO;
        }
    }
    return YES;
}
