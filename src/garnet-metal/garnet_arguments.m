// garnet_arguments.m - GarnetMetal modular compilation unit.
#if !defined(GARNET_INTERNAL) && !defined(GARNET_METAL_MODULAR_BUILD)
#define GARNET_INTERNAL static
#endif
#if !defined(RDNA4_ARGUMENT_SUMMARY_RUNTIME_TEST) && !defined(GARNET_ARGUMENT_SUMMARY_OWNERS_ONLY)
#ifdef GARNET_METAL_MODULAR_BUILD
#include "garnet_metal_internal.h"
#endif
#define RDNA4S_TRACE(...) ((void)0)
#include "garnet_stitch_reflection.h"
#undef RDNA4S_TRACE
#endif

#if !defined(RDNA4_ARGUMENT_SUMMARY_RUNTIME_TEST) && !defined(GARNET_ARGUMENT_SUMMARY_OWNERS_ONLY)

// Shared resource sets stay at 2..4; stage-local arguments stay at 0/1.




static VkDescriptorType rdna4_argument_descriptor_type(unsigned category) {
    const VkDescriptorType types[] = {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLER};
    return types[category];
}

GARNET_INTERNAL VkShaderStageFlags rdna4_argument_stage_flags(uint32_t stages,
                                                     BOOL compute) {
    if (!stages) return 0;
    if (compute) return VK_SHADER_STAGE_COMPUTE_BIT;
    VkShaderStageFlags flags = 0;
    if (stages & 1u) {
        flags |= VK_SHADER_STAGE_VERTEX_BIT;
        if (RDNA4R.tessellation) flags |= VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    }
    if (stages & 2u) flags |= VK_SHADER_STAGE_FRAGMENT_BIT;
    return flags;
}

// ABI 5 adds component class (float/uint/int) to the ABI 4 shape offsets.
GARNET_INTERNAL BOOL rdna4_argument_shape(unsigned shape, VkImageViewType *viewType, BOOL *depth) {
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

GARNET_INTERNAL BOOL rdna4_argument_reflection_version(NSDictionary *metadata,
                                              BOOL *present,
                                              uint64_t *version) {
    id value = metadata[@"trimmed_from_reflection_version"];
    *present = value != nil;
    *version = 0;
    if (!value) return YES;
    return rdna4_number_u64((__bridge CFTypeRef)value, version) &&
        *version > 0 && *version <= 66;
}

GARNET_INTERNAL BOOL rdna4_argument_normalized_metadata(NSDictionary *metadata,
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

GARNET_INTERNAL BOOL rdna4_argument_table_uses_shape_registry(NSDictionary *metadata) {
    id table = metadata[@"argument_resource_tables"];
    uint64_t version = 0;
    return [table isKindOfClass:[NSDictionary class]] &&
        rdna4_number_u64((__bridge CFTypeRef)table[@"version"], &version) &&
        version >= 9 && version <= 10;
}

GARNET_INTERNAL BOOL rdna4_argument_table_stages_compatible(NSDictionary *a,
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

static inline __attribute__((unused)) NSMutableData *rdna4_argument_table_bytes(NSArray<NSArray *> *rows,
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

GARNET_INTERNAL NSData *rdna4_gpu_argument_spv(NSData *spv, NSDictionary **metadata, uint64_t deadline, NSError **error) {
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

GARNET_INTERNAL BOOL rdna4_argument_table_metadata(
    const RDNA4sRefl *reflection, int stageSet, NSDictionary *metadata,
    NSError **error) {
    return rdna4_argument_table_metadata_ex(
        reflection, stageSet, metadata, NULL, NULL, NULL, NULL, NULL, error);
}

GARNET_INTERNAL BOOL rdna4_argument_table_metadata_apply(
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

GARNET_INTERNAL BOOL rdna4_argument_descriptor_validate(
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

GARNET_INTERNAL BOOL rdna4_argument_table_layouts(RDNA4ArgumentTables *tables, const RDNA4sRefl *vertex,
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

GARNET_INTERNAL void rdna4_argument_tables_destroy(RDNA4ArgumentTables *tables) {
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

GARNET_INTERNAL BOOL rdna4_argument_stage_remap(const RDNA4ArgumentTables *tables,
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
#endif // !defined(RDNA4_ARGUMENT_SUMMARY_RUNTIME_TEST) && !defined(GARNET_ARGUMENT_SUMMARY_OWNERS_ONLY)
#if !defined(RDNA4_ARGUMENT_SUMMARY_RUNTIME_TEST) && !defined(GARNET_ARGUMENT_SUMMARY_OWNERS_ONLY)
static _Atomic uint64_t rdna4_argument_pool_descriptors;


@implementation RDNA4ArgumentPool
- (instancetype)initWithInfo:(const VkDescriptorPoolCreateInfo *)info error:(NSError **)error {
    self = [super init];
    if (!self) return nil;
    uint64_t count = 0;
    for (uint32_t i = 0; i < info->poolSizeCount; ++i) {
        if (info->pPoolSizes[i].descriptorCount > UINT64_MAX - count) {
            if (error) *error = rdna4_err(49, @"GPU argument pool descriptor count overflows");
            return nil;
        }
        count += info->pPoolSizes[i].descriptorCount;
    }
    uint64_t limit = RDNA4R.descriptorLimits.maxUpdateAfterBindDescriptorsInAllPools;
    uint64_t used = atomic_load(&rdna4_argument_pool_descriptors);
    do {
        if (used > limit || count > limit - used) {
            if (error) *error = rdna4_err(49, @"GPU argument pools exceed the backend global descriptor limit");
            return nil;
        }
    } while (!atomic_compare_exchange_weak(&rdna4_argument_pool_descriptors, &used, used + count));
    VkResult result = vkCreateDescriptorPool(RDNA4R.dev, info, NULL, &handle);
    if (result != VK_SUCCESS) {
        handle = VK_NULL_HANDLE;
        atomic_fetch_sub(&rdna4_argument_pool_descriptors, count);
        if (error) *error = rdna4_err(49, [NSString stringWithFormat:@"GPU argument descriptor pool = %d", result]);
        return nil;
    }
    _descriptorCount = count;
    return self;
}
- (void)dealloc {
    if (handle && RDNA4R.ok) vkDestroyDescriptorPool(RDNA4R.dev, handle, NULL);
    atomic_fetch_sub(&rdna4_argument_pool_descriptors, _descriptorCount);
}
@end
#endif // !defined(RDNA4_ARGUMENT_SUMMARY_RUNTIME_TEST) && !defined(GARNET_ARGUMENT_SUMMARY_OWNERS_ONLY)
#if !defined(RDNA4_ARGUMENT_SUMMARY_RUNTIME_TEST) && !defined(GARNET_ARGUMENT_SUMMARY_OWNERS_ONLY)
@implementation RDNA4CommandBuffer (RDNA4ArgumentCommands)
- (BOOL)rdna4PrepareArguments:(const RDNA4ArgumentTables *)tables resources:(NSSet *)resources writable:(NSSet *)writable scan:(const RDNA4ArgumentScanPrepared *)scan textures:(void (^)(RDNA4Texture *, VkImageLayout, BOOL))prepare {
    NSSet *residency = [self rdna4ResidencyDeclaredResources];
    if (residency.count) resources = resources ? [resources setByAddingObjectsFromSet:residency] : residency;
    NSMutableSet<RDNA4Buffer *> *bufferResources = [NSMutableSet set];
    for (id object in resources) {
        // Declarations remain active when an encoder continues in a new segment.
        [self rdna4RetainArgument:object];
        if ([object isKindOfClass:[RDNA4Buffer class]]) [bufferResources addObject:object];
        if ([object isKindOfClass:[RDNA4Heap class]])
            for (id resource in [object rdna4LiveResources]) if (![resource isAliasable]) {
                [self rdna4RetainArgument:resource];
                if ([resource isKindOfClass:[RDNA4Buffer class]]) [bufferResources addObject:resource];
            }
    }
    // Keep only this draw/dispatch's declarations; each segment owns its snapshot.
    NSSet *bufferSnapshot = [bufferResources copy];
    NSSet *retainedSnapshot = (_curSegment && !_curSegment->ended)
        ? ([_curSegment->argumentRefs member:bufferSnapshot] ?: bufferSnapshot)
        : bufferSnapshot;
    [self rdna4RetainArgument:retainedSnapshot];
    _bufferAddressResources = retainedSnapshot;
    NSMutableSet *samplers = [NSMutableSet set];
    NSMutableArray<NSDictionary *> *writes = [NSMutableArray array];
    NSString *scopeError = nil;
    if (!rdna4_argument_sampler_scope([self device], scan, bufferSnapshot, writable,
                                      samplers, writes,
                                      &scopeError)) {
        [self rdna4Fail:scopeError ?: @"argument sampler scope failed"];
        return NO;
    }
    for (NSDictionary *write in writes) {
        id buffer = write[@"buffer"];
        NSArray<NSValue *> *ranges = write[@"ranges"];
        NSSet *candidates = write[@"candidates"];
        if (!buffer || !ranges.count) {
            [self rdna4Fail:@"argument sampler scope produced an empty write intent"];
            return NO;
        }
        [self rdna4ArgumentWriteRanges:buffer ranges:ranges candidates:candidates];
        if ([self rdna4Error]) return NO;
    }
    if (!tables->active) return YES;
    NSArray *snapshot = rdna4_argument_resource_snapshot([self device], resources, samplers);
    NSMutableArray *rows[3] = {[NSMutableArray array], [NSMutableArray array], [NSMutableArray array]};
    NSMutableArray *texelRows[2] = {[NSMutableArray array], [NSMutableArray array]};
    for (NSArray *row in snapshot) {
        if ([row[2] boolValue]) {
            RDNA4Texture *texture = row[1];
            if ([texture rdna4IsTexel]) {
                MTLTextureUsage usage = [texture usage];
                if (tables->dynamicTexel[0] && (!usage || (usage & MTLTextureUsageShaderRead)))
                    [texelRows[0] addObject:row];
                if (tables->dynamicTexel[1] && [texture rdna4TexelWritable])
                    [texelRows[1] addObject:row];
                continue;
            }
            if (tables->capacity[0] || tables->imageQueries) [rows[0] addObject:row];
            if (tables->capacity[1] && ([row[1] usage] & MTLTextureUsageShaderWrite)) [rows[1] addObject:row];
        } else if (tables->capacity[2]) [rows[2] addObject:row];
    }
    uint32_t tokenCounts[5] = {0}, descriptorCounts[3] = {0};
    uint32_t texelDescriptorCounts[2] = {0};
    VkDescriptorPoolSize sizes[5]; uint32_t sizeCount = 0;
    for (unsigned category = 0; category < 3; ++category) {
        if (!rdna4_argument_counts(tables, category, rows[category].count,
                                   &tokenCounts[category], &descriptorCounts[category])) {
            [self rdna4Fail:@"GPU argument token or shape count exceeds the descriptor ABI"]; return NO;
        }
        if (!tables->capacity[category]) continue;
        if (descriptorCounts[category] > tables->capacity[category]) {
            [self rdna4Fail:[NSString stringWithFormat:@"GPU argument category %u needs %lu descriptors; backend pipeline capacity is %u",
                category, (unsigned long)descriptorCounts[category], tables->capacity[category]]]; return NO;
        }
        sizes[sizeCount++] = (VkDescriptorPoolSize){rdna4_argument_descriptor_type(category),
            tables->variable[category] ? descriptorCounts[category] : tables->capacity[category]};
    }
    for (unsigned texel = 0; texel < 2; ++texel) {
        if (!tables->texel[texel]) continue;
        if (tables->dynamicTexel[texel]) {
            uint64_t tokens = texelRows[texel].count + 1ull;
            uint64_t dummies = rdna4ns_registry_access_count(
                &tables->nullShapes, texel ? RDNA4NS_ACCESS_STORAGE_TEXEL
                                           : RDNA4NS_ACCESS_UNIFORM_TEXEL);
            uint32_t descriptorCount = 0;
            if (tokens > UINT32_MAX || dummies > UINT32_MAX ||
                rdna4ns_texel_descriptor_count((uint32_t)tokens,
                    (uint32_t)dummies, &descriptorCount) != RDNA4NS_OK) {
                [self rdna4Fail:@"GPU texel token and null-shape counts overflow"]; return NO;
            }
            tokenCounts[3 + texel] = (uint32_t)tokens;
            texelDescriptorCounts[texel] = descriptorCount;
        } else {
            texelDescriptorCounts[texel] = RDNA4NS_TEXEL_SHAPES;
        }
        uint32_t texelCapacity = tables->texelVariable[texel]
            ? tables->texelCapacity[texel] : RDNA4NS_TEXEL_SHAPES;
        if (texelDescriptorCounts[texel] > texelCapacity) {
            [self rdna4Fail:[NSString stringWithFormat:
                @"GPU texel category %u needs %u descriptors; pipeline capacity is %u",
                texel, texelDescriptorCounts[texel], texelCapacity]];
            return NO;
        }
        sizes[sizeCount++] = (VkDescriptorPoolSize){
            texel ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER
                  : VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
            tables->texelVariable[texel] ? texelDescriptorCounts[texel]
                                         : RDNA4NS_TEXEL_SHAPES};
    }
    uint32_t allocatedDescriptors[3] = {0};
    for (unsigned category = 0; category < 3; ++category)
        if (tables->capacity[category])
            allocatedDescriptors[category] = tables->variable[category]
                ? descriptorCounts[category] : tables->capacity[category];
    NSError *budgetError = nil;
    if (!rdna4_argument_allocation_budget(tables, allocatedDescriptors,
            texelDescriptorCounts, &budgetError)) {
        [self rdna4Fail:budgetError.localizedDescription];
        return NO;
    }
    NSMutableArray<NSNumber *> *samplerFlags = nil;
    if (tables->samplerCoordinates) {
        samplerFlags = [NSMutableArray arrayWithCapacity:rows[2].count + 1];
        [samplerFlags addObject:@0];
        for (NSArray *row in rows[2])
            [samplerFlags addObject:([row[1] rdna4PixelCoordinates] ? @1 : @0)];
    }
    NSMutableData *tokens = tables->abi10
        ? rdna4_argument_table_bytes_abi10(@[rows[0], rows[1], rows[2]],
            @[texelRows[0], texelRows[1]], tables->dynamicTexel,
            tables->imageQueries, RDNA4R.lim.maxSamplerLodBias,
            tables->samplerCoordinates, samplerFlags)
        : rdna4_argument_table_bytes_abi(@[rows[0], rows[1], rows[2]],
            tables->imageQueries, RDNA4R.lim.maxSamplerLodBias,
            tables->samplerCoordinates, samplerFlags);
    if (!tokens) { [self rdna4Fail:@"GPU argument table dimensions or sampler LOD limit are invalid"]; return NO; }
    VkDescriptorPoolCreateInfo poolInfo = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT, .maxSets = 3, .poolSizeCount = sizeCount, .pPoolSizes = sizes };
    NSError *poolError = nil;
    RDNA4ArgumentPool *pool = [[RDNA4ArgumentPool alloc] initWithInfo:&poolInfo error:&poolError];
    if (!pool) { [self rdna4Fail:poolError.localizedDescription]; return NO; }
    uint32_t variableCounts[3] = {0};
    for (unsigned category = 0; category < 3; ++category) {
        if (category < 2 && tables->texelVariable[category])
            variableCounts[category] = texelDescriptorCounts[category];
        else if (tables->variable[category])
            variableCounts[category] = descriptorCounts[category];
    }
    VkDescriptorSetVariableDescriptorCountAllocateInfo variable = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO,
        .descriptorSetCount = 3, .pDescriptorCounts = variableCounts };
    VkDescriptorSetAllocateInfo allocation = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .pNext = &variable,
        .descriptorPool = pool->handle, .descriptorSetCount = 3, .pSetLayouts = tables->dsl };
    VkDescriptorSet sets[3] = {VK_NULL_HANDLE};
    VkResult result = vkAllocateDescriptorSets(RDNA4R.dev, &allocation, sets);
    if (result != VK_SUCCESS) { [self rdna4Fail:[NSString stringWithFormat:@"GPU argument descriptor allocation = %d", result]]; return NO; }
    for (unsigned category = 0; category < 3; ++category) {
        uint32_t tokenCount = tokenCounts[category];
        for (NSArray *row in rows[category]) [self rdna4RetainArgument:row[1]];
        if (!tables->capacity[category]) continue;
        VkDescriptorImageInfo *images = calloc(descriptorCounts[category], sizeof *images);
        if (!images) { [self rdna4Fail:@"GPU argument descriptor allocation failed"]; return NO; }
        BOOL valid = YES;
        uint32_t imageCount = category == 2 ? descriptorCounts[category] : tokenCount;
        for (uint32_t index = 0; index < imageCount; ++index) {
            if (category == 2) {
                uint32_t logical = index % tokenCount;
                BOOL comparison = index >= tokenCount;
                RDNA4SamplerState *sampler = logical ? rows[category][logical - 1][1] : rdna4_dummy_sampler([self device]);
                if (!sampler) { valid = NO; break; }
                NSError *error = nil;
                images[index].sampler = tables->samplerCoordinates
                    ? [sampler rdna4SamplerComparison:comparison normalizedCoordinates:YES error:&error]
                    : [sampler rdna4SamplerComparison:comparison error:&error];
                if (!images[index].sampler) { [self rdna4Fail:error.localizedDescription ?: @"GPU argument sampler unavailable"]; valid = NO; break; }
                [self rdna4RetainArgument:sampler];
            } else if (!index) {
                if (!RDNA4R.nullDescriptors) {
                    RDNA4Texture *texture = category ? rdna4_dummy_texture_write([self device]) : rdna4_dummy_texture([self device]);
                    if (!texture) { valid = NO; break; }
                    [self rdna4RetainArgument:texture]; prepare(texture, VK_IMAGE_LAYOUT_GENERAL, category == 1);
                    images[index].imageView = [texture vkView];
                }
                images[index].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            } else {
                RDNA4Texture *texture = rows[category][index - 1][1];
                if (!texture) { valid = NO; break; }
                [self rdna4RetainArgument:texture]; prepare(texture, VK_IMAGE_LAYOUT_GENERAL, category == 1);
                images[index].imageView = [texture vkView]; images[index].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            }
        }
        if (valid) {
            VkWriteDescriptorSet write = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[category],
                .dstBinding = 0, .descriptorCount = imageCount, .descriptorType = rdna4_argument_descriptor_type(category), .pImageInfo = images };
            vkUpdateDescriptorSets(RDNA4R.dev, 1, &write, 0, NULL);
            if (tables->shapeRegistry && category < 2) {
                unsigned access = category ? RDNA4NS_ACCESS_STORAGE
                                           : RDNA4NS_ACCESS_SAMPLED;
                uint16_t count = rdna4ns_registry_access_count(
                    &tables->nullShapes, access);
                for (uint16_t ordinal = 0; ordinal < count; ++ordinal) {
                    uint32_t word = 0;
                    rdna4ns_shape shape = {0};
                    if (rdna4ns_registry_access_word(
                            &tables->nullShapes, access, ordinal, &word,
                            &shape) != RDNA4NS_OK ||
                        !rdna4ns_access_is_image(shape.access) ||
                        (shape.access == RDNA4NS_ACCESS_STORAGE) !=
                            (category == 1)) {
                        valid = NO;
                        break;
                    }
                    VkDescriptorImageInfo image = {
                        .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
                    if (!RDNA4R.nullDescriptors) {
                        RDNA4Texture *texture =
                            rdna4_dummy_null_image([self device], &shape);
                        if (!texture) { valid = NO; break; }
                        [self rdna4RetainArgument:texture];
                        prepare(texture, VK_IMAGE_LAYOUT_GENERAL,
                                category == 1);
                        image.imageView = [texture vkView];
                    }
                    VkWriteDescriptorSet tail = {
                        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                        .dstSet = sets[category],
                        .dstBinding = 0,
                        .dstArrayElement = tokenCount + ordinal,
                        .descriptorCount = 1,
                        .descriptorType =
                            rdna4_argument_descriptor_type(category),
                        .pImageInfo = &image,
                    };
                    vkUpdateDescriptorSets(RDNA4R.dev, 1, &tail, 0, NULL);
                }
            } else if (category == 0) for (uint32_t shape = 0; shape < RDNA4_ARGUMENT_NULL_IMAGES; ++shape) {
                if (!(tables->nullImages & (1ull << shape))) continue;
                VkDescriptorImageInfo image = { .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
                if (!RDNA4R.nullDescriptors) {
                    RDNA4Texture *texture = rdna4_dummy_null_texture([self device], shape);
                    if (!texture) { valid = NO; break; }
                    [self rdna4RetainArgument:texture]; prepare(texture, VK_IMAGE_LAYOUT_GENERAL, NO);
                    image.imageView = [texture vkView];
                }
                VkWriteDescriptorSet tail = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[category],
                    .dstBinding = 0, .dstArrayElement = tokenCount + shape, .descriptorCount = 1,
                    .descriptorType = rdna4_argument_descriptor_type(category), .pImageInfo = &image };
                vkUpdateDescriptorSets(RDNA4R.dev, 1, &tail, 0, NULL);
            }
        }
        free(images);
        if (!valid || [self rdna4Error]) { if (!valid) [self rdna4Fail:@"GPU argument dummy resource unavailable"]; return NO; }
    }
    if (tables->shapeRegistry) for (unsigned texel = 0; texel < 2; ++texel) {
        if (!tables->texel[texel]) continue;
        unsigned access = texel ? RDNA4NS_ACCESS_STORAGE_TEXEL
                                : RDNA4NS_ACCESS_UNIFORM_TEXEL;
        uint16_t count = rdna4ns_registry_access_count(
            &tables->nullShapes, access);
        uint32_t tokenCount = tables->dynamicTexel[texel]
            ? tokenCounts[3 + texel] : 0;
        uint32_t descriptorCount = texelDescriptorCounts[texel];
        VkBufferView *views = calloc(descriptorCount, sizeof *views);
        if (!views) {
            [self rdna4Fail:@"GPU argument texel descriptor allocation failed"];
            return NO;
        }
        BOOL valid = YES;
        if (tables->dynamicTexel[texel]) {
            rdna4ns_shape dummyShape = { .dim = RDNA4NS_DIM_BUFFER,
                .component = RDNA4NS_COMP_FLOAT,
                .access = texel ? RDNA4NS_ACCESS_STORAGE_TEXEL
                                : RDNA4NS_ACCESS_UNIFORM_TEXEL };
            for (unsigned component = 0; component <= RDNA4NS_COMP_SINT; ++component)
                if (tables->texelComponents[texel] & (1u << component)) {
                    dummyShape.component = (uint8_t)component;
                    break;
                }
            if (count) {
                if (rdna4ns_registry_access_word(&tables->nullShapes, access,
                        0, NULL, &dummyShape) != RDNA4NS_OK) valid = NO;
            }
            const char *why = NULL;
            if (valid) views[0] = rdna4_dummy_null_texel(
                [self device], &dummyShape, &why);
            if (valid && !views[0]) {
                [self rdna4Fail:why ? @(why)
                    : @"GPU argument texel sentinel dummy unavailable"];
                valid = NO;
            }
            for (NSUInteger index = 1; valid && index < tokenCount; ++index) {
                NSArray *row = texelRows[texel][index - 1];
                RDNA4Texture *texture = row[1];
                MTLTextureUsage usage = [texture usage];
                if (![texture rdna4IsTexel] ||
                    (texel ? ![texture rdna4TexelWritable]
                           : (usage && !(usage & MTLTextureUsageShaderRead))) ||
                    !(views[index] = [texture rdna4TexelView])) {
                    valid = NO;
                    break;
                }
                [self rdna4RetainArgument:texture];
                prepare(texture, VK_IMAGE_LAYOUT_GENERAL, texel != 0);
            }
        }
        for (uint16_t ordinal = 0; valid && ordinal < count; ++ordinal) {
            rdna4ns_shape shape = {0};
            const char *why = NULL;
            if (rdna4ns_registry_access_word(
                    &tables->nullShapes, access, ordinal, NULL,
                    &shape) != RDNA4NS_OK) {
                valid = NO;
                break;
            }
            uint32_t index = ordinal;
            if (tables->dynamicTexel[texel] &&
                rdna4ns_texel_null_index(tokenCount, ordinal, &index) !=
                    RDNA4NS_OK) {
                valid = NO;
                break;
            }
            views[index] = rdna4_dummy_null_texel(
                [self device], &shape, &why);
            if (!views[index]) {
                [self rdna4Fail:why ? @(why)
                    : @"GPU argument texel dummy unavailable"];
                valid = NO;
                break;
            }
        }
        if (valid && tables->texelVariable[texel]) {
            VkWriteDescriptorSet write = {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = sets[texel], .dstBinding = 1,
                .dstArrayElement = 0, .descriptorCount = descriptorCount,
                .descriptorType = texel ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER
                                        : VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
                .pTexelBufferView = views };
            vkUpdateDescriptorSets(RDNA4R.dev, 1, &write, 0, NULL);
        } else if (valid) for (uint16_t ordinal = 0; ordinal < count; ++ordinal) {
            VkWriteDescriptorSet write = {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = sets[texel], .dstBinding = 1,
                .dstArrayElement = ordinal, .descriptorCount = 1,
                .descriptorType = texel ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER
                                        : VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
                .pTexelBufferView = &views[ordinal] };
            vkUpdateDescriptorSets(RDNA4R.dev, 1, &write, 0, NULL);
        }
        free(views);
        if (!valid) {
            if (![self rdna4Error])
                [self rdna4Fail:@"GPU argument texel descriptor is invalid"];
            return NO;
        }
    }
    if (![self rdna4Bytes:tokens.bytes length:tokens.length buffer:&_argumentTable offset:&_argumentTableOffset]) return NO;
    _argumentTableBytes = tokens.length;
    memcpy(_argumentSets, sets, sizeof sets);
    memcpy(_argumentTokenCounts, tokenCounts, sizeof tokenCounts);
    [_curSegment->argumentPools addObject:pool];
    return YES;
}

- (void)rdna4BindArguments:(const RDNA4ArgumentTables *)tables point:(VkPipelineBindPoint)point layout:(VkPipelineLayout)layout {
    if (tables->active) vkCmdBindDescriptorSets(_vk, point, layout, 2, 3, _argumentSets, 0, NULL);
}
- (void)rdna4CopyArgumentSets:(VkDescriptorSet [3])outSets {
    memcpy(outSets, _argumentSets, sizeof _argumentSets);
}

@end
#endif // !defined(RDNA4_ARGUMENT_SUMMARY_RUNTIME_TEST) && !defined(GARNET_ARGUMENT_SUMMARY_OWNERS_ONLY)
#if !defined(RDNA4_ARGUMENT_SUMMARY_RUNTIME_TEST) && !defined(GARNET_ARGUMENT_SUMMARY_OWNERS_ONLY)
#ifndef RDNA4_ARGUMENT_INC
#define RDNA4_ARGUMENT_INC


@protocol RDNA4ICBInternal;
@class RDNA4RenderPipelineState;
@class RDNA4ComputePipelineState;

@implementation RDNA4ICBArgOwners
- (instancetype)init {
    if ((self = [super init])) {
        _icbs = [NSMutableSet set];
        _pipelines = [NSMutableSet set];
        _nestedBuffers = [NSMutableSet set];
    }
    return self;
}
@end

static char rdna4_icb_arg_owners_key;

static void rdna4_icb_arg_note(RDNA4Buffer *dst, id icb, id pso, RDNA4Buffer *nestedBuf) {
    if (!dst || (!icb && !pso && !nestedBuf)) return;
    @synchronized (dst) {
        RDNA4ICBArgOwners *owners = objc_getAssociatedObject(dst, &rdna4_icb_arg_owners_key);
        if (!owners) {
            owners = [RDNA4ICBArgOwners new];
            objc_setAssociatedObject(dst, &rdna4_icb_arg_owners_key, owners, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        }
        if (icb) [owners.icbs addObject:icb];
        if (pso) [owners.pipelines addObject:pso];
        if (nestedBuf && nestedBuf != dst) [owners.nestedBuffers addObject:nestedBuf];
    }
}

static void rdna4_argbuf_icb_snapshot_walk(RDNA4Buffer *buf, NSMutableSet *icbs, NSMutableSet *psos, NSMutableSet *visited) {
    if (!buf || [visited containsObject:buf]) return;
    [visited addObject:buf];
    NSSet *localIcbs = nil, *localPsos = nil, *localNested = nil;
    @synchronized (buf) {
        RDNA4ICBArgOwners *owners = objc_getAssociatedObject(buf, &rdna4_icb_arg_owners_key);
        if (owners) {
            localIcbs = [owners.icbs copy];
            localPsos = [owners.pipelines copy];
            localNested = [owners.nestedBuffers copy];
        }
    }
    if (localIcbs.count) [icbs unionSet:localIcbs];
    if (localPsos.count) [psos unionSet:localPsos];
    for (RDNA4Buffer *nb in localNested)
        rdna4_argbuf_icb_snapshot_walk(nb, icbs, psos, visited);
}

GARNET_INTERNAL void rdna4_argbuf_icb_snapshot(RDNA4Buffer *buf, NSMutableSet *icbs, NSMutableSet *psos) {
    if (!buf) return;
    NSMutableSet *visited = [NSMutableSet set];
    rdna4_argbuf_icb_snapshot_walk(buf, icbs, psos, visited);
}

#define RDNA4_REFLECTION_STITCHING 0x20000000u

// Metal's metadata completion ignores status, so errors need readable data too.
static dispatch_data_t rdna4_reflection_error_data(void) {
    static dispatch_data_t reply;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        uint8_t *bytes = NULL;
        size_t length = 0;
        if (!rdna4_stitch_container(NULL, 0, &bytes, &length))
            [NSException raise:NSMallocException format:@"Cannot allocate empty reflection container"];
        reply = dispatch_data_create(bytes, length, NULL, DISPATCH_DATA_DESTRUCTOR_FREE);
        if (!reply) {
            free(bytes);
            [NSException raise:NSMallocException format:@"Cannot allocate reflection dispatch data"];
        }
    });
    return reply;
}

@implementation RDNA4ReflectionCompiler
- (NSUInteger)compilerFlags { return 0; }
- (void)reflectionWithFunction:(id)function options:(NSUInteger)options sync:(BOOL)sync binaryArchives:(id)archives
             completionHandler:(void (^)(NSUInteger, dispatch_data_t, NSString *))completion {
    (void)sync; (void)archives;
    if (!completion) return;
    NSData *data = rdna4_function_reflection_data(function);
    BOOL visible = [function respondsToSelector:@selector(functionType)] &&
        [(id<MTLFunction>)function functionType] == MTLFunctionTypeVisible;
    if (data && ((options & RDNA4_REFLECTION_STITCHING) || visible)) {
        uint8_t *bytes = NULL;
        size_t length = 0;
        if (rdna4_stitch_reflection(data.bytes, data.length, NULL, &bytes, &length)) {
            dispatch_data_t result = dispatch_data_create(bytes, length, NULL, DISPATCH_DATA_DESTRUCTOR_FREE);
            if (!result) free(bytes);
            completion(result ? 0 : 1, result ?: rdna4_reflection_error_data(),
                       result ? nil : @"Failed to create MTLPSBIN stitching reflection data");
            return;
        }
        // The stitching callback ignores status; it still needs a valid container.
        completion(1, rdna4_reflection_error_data(), @"Failed to convert AIR reflection data to MTLPSBIN stitching reflection");
        return;
    }
    if (!data) {
        completion(1, rdna4_reflection_error_data(), @"Function has no AIR reflection data");
        return;
    }
    dispatch_data_t result = dispatch_data_create(data.bytes, data.length, NULL, ^{ (void)data; });
    completion(result ? 0 : 1, result ?: rdna4_reflection_error_data(),
               result ? nil : @"Failed to create AIR reflection data");
}
- (void)reflectionWithFunction:(id)function options:(NSUInteger)options sync:(BOOL)sync pipelineLibrary:(id)library
             completionHandler:(void (^)(NSUInteger, dispatch_data_t, NSString *))completion {
    [self reflectionWithFunction:function options:options sync:sync binaryArchives:library completionHandler:completion];
}
- (void)reflectionWithFunction:(id)function options:(NSUInteger)options sync:(BOOL)sync
             completionHandler:(void (^)(NSUInteger, dispatch_data_t, NSString *))completion {
    [self reflectionWithFunction:function options:options sync:sync binaryArchives:nil completionHandler:completion];
}
@end

@class RDNA4ArgumentLayout;
@implementation RDNA4ArgumentField
@end

GARNET_INTERNAL BOOL rdna4_argument_scalar(MTLDataType type, NSUInteger *size, NSUInteger *alignment) {
    const struct { MTLDataType first; NSUInteger bytes; } vectors[] = {
        { MTLDataTypeFloat, 4 }, { MTLDataTypeHalf, 2 }, { MTLDataTypeInt, 4 }, { MTLDataTypeUInt, 4 },
        { MTLDataTypeShort, 2 }, { MTLDataTypeUShort, 2 }, { MTLDataTypeChar, 1 },
        { MTLDataTypeUChar, 1 }, { MTLDataTypeBool, 1 }, { MTLDataTypeLong, 8 }, { MTLDataTypeULong, 8 }
    };
    for (unsigned i = 0; i < sizeof vectors / sizeof vectors[0]; ++i) {
        if (type < vectors[i].first || type > vectors[i].first + 3) continue;
        NSUInteger count = type - vectors[i].first + 1;
        *size = *alignment = vectors[i].bytes * (count == 3 ? 4 : count);
        return YES;
    }
    MTLDataType first = type >= MTLDataTypeHalf2x2 && type <= MTLDataTypeHalf4x4 ? MTLDataTypeHalf2x2 : MTLDataTypeFloat2x2;
    if (type >= first && type <= first + 8) {
        NSUInteger columns = (type - first) / 3 + 2, rows = (type - first) % 3 + 2;
        *alignment = (first == MTLDataTypeHalf2x2 ? 2 : 4) * (rows == 3 ? 4 : rows);
        *size = columns * *alignment;
        return YES;
    }
    if (type == MTLDataTypePointer || type == MTLDataTypeTexture || type == MTLDataTypeSampler ||
        type == MTLDataTypeRenderPipeline || type == MTLDataTypeComputePipeline ||
        type == MTLDataTypeIndirectCommandBuffer || rdna4_rt_arg_is_rt_data_type(type)) {
        *size = *alignment = sizeof(uint64_t); return YES;
    }
    return NO;
}


static BOOL rdna4_argument_add(NSMutableDictionary *fields, MTLDataType type, MTLArrayType *array,
                             MTLStructType *structure, MTLPointerType *pointer, NSUInteger offset, NSUInteger index,
                             id device, unsigned depth, NSUInteger *size, NSUInteger *alignment) {
    if (depth > 16 || offset > (64u << 20) || index == NSUIntegerMax || fields.count >= 4096) return NO;
    if (type == MTLDataTypeArray) {
        if (!array || !array.arrayLength || array.arrayLength > 4096 || !array.stride ||
            !array.argumentIndexStride || array.arrayLength > ((64u << 20) - offset) / array.stride) return NO;
        NSUInteger elementSize = 0, elementAlignment = 1;
        for (NSUInteger n = 0; n < array.arrayLength; ++n) {
            if (n > (NSUIntegerMax - index) / array.argumentIndexStride ||
                !rdna4_argument_add(fields, array.elementType, array.elementArrayType, array.elementStructType, array.elementPointerType,
                    offset + n * array.stride, index + n * array.argumentIndexStride, device, depth + 1,
                    &elementSize, &elementAlignment) || elementSize > array.stride) return NO;
        }
        *size = array.arrayLength * array.stride; *alignment = elementAlignment;
        return YES;
    }
    RDNA4ArgumentLayout *nested = nil;
    if (type == MTLDataTypeStruct) {
        nested = [[RDNA4ArgumentLayout alloc] initWithType:structure device:device depth:depth + 1];
        if (!nested) return NO;
        *size = nested.encodedLength; *alignment = nested.alignment;
        if (*size > (64u << 20) - offset) return NO;
        for (NSNumber *key in nested.fields) {
            if (key.unsignedIntegerValue >= NSUIntegerMax - index || fields.count >= 4096) return NO;
            NSNumber *identifier = @(index + key.unsignedIntegerValue);
            if (fields[identifier]) return NO;
            RDNA4ArgumentField *member = nested.fields[key], *field = [RDNA4ArgumentField new];
            field.offset = offset + member.offset; field.size = member.size;
            field.type = member.type; field.nested = member.nested;
            fields[identifier] = field;
        }
        return YES;
    } else if (!rdna4_argument_scalar(type, size, alignment)) return NO;
    if (type == MTLDataTypePointer && pointer.elementIsArgumentBuffer) {
        if (pointer.elementType != MTLDataTypeStruct) return NO;
        nested = [[RDNA4ArgumentLayout alloc] initWithType:pointer.elementStructType device:device depth:depth + 1];
        if (!nested || nested.encodedLength != pointer.dataSize) return NO;
    }
    if (*size > (64u << 20) - offset || fields[@(index)]) return NO;
    RDNA4ArgumentField *field = [RDNA4ArgumentField new];
    field.offset = offset; field.size = *size; field.type = type; field.nested = nested;
    fields[@(index)] = field;
    return YES;
}

@implementation RDNA4ArgumentLayout
- (instancetype)initWithArguments:(NSArray<MTLArgumentDescriptor *> *)arguments device:(id)device {
    if (!(self = [super init]) || !arguments.count || arguments.count > 4096) return nil;
    NSMutableDictionary *fields = [NSMutableDictionary dictionary];
    NSUInteger length = 0, alignment = 1;
    NSArray *ordered = [arguments sortedArrayUsingComparator:^NSComparisonResult(MTLArgumentDescriptor *a, MTLArgumentDescriptor *b) {
        return a.index < b.index ? NSOrderedAscending : a.index > b.index ? NSOrderedDescending : NSOrderedSame;
    }];
    for (MTLArgumentDescriptor *argument in ordered) {
        NSUInteger size = 0, align = 0;
        if (!rdna4_argument_scalar(argument.dataType, &size, &align) || !argument.arrayLength ||
            argument.arrayLength > 4096 || argument.arrayLength > NSUIntegerMax - argument.index) return nil;
        length = (length + align - 1) & ~(align - 1); alignment = MAX(alignment, align);
        for (NSUInteger n = 0; n < argument.arrayLength; ++n) {
            if (!rdna4_argument_add(fields, argument.dataType, nil, nil, nil, length, argument.index + n,
                                  device, 0, &size, &align)) return nil;
            length += size;
        }
    }
    _fields = [fields copy]; _alignment = alignment;
    _length = (length + alignment - 1) & ~(alignment - 1); _device = device;
    return self;
}
- (instancetype)initWithType:(MTLStructType *)type device:(id)device depth:(unsigned)depth {
    if (!(self = [super init]) || !type || depth > 16 || !type.members.count) return nil;
    NSMutableDictionary *fields = [NSMutableDictionary dictionary];
    NSUInteger length = 0, alignment = 1;
    for (MTLStructMember *member in type.members) {
        NSUInteger size = 0, align = 0;
        if (!rdna4_argument_add(fields, member.dataType, member.arrayType, member.structType, member.pointerType, member.offset,
                              member.argumentIndex, device, depth, &size, &align)) return nil;
        length = MAX(length, member.offset + size); alignment = MAX(alignment, align);
    }
    _fields = [fields copy]; _alignment = alignment;
    _length = (length + alignment - 1) & ~(alignment - 1); _device = device;
    return self;
}
- (RDNA4ArgumentField *)field:(NSUInteger)index { return _fields[@(index)]; }
- (NSDictionary<NSNumber *, RDNA4ArgumentField *> *)fields { return _fields; }
- (NSUInteger)encodedLength { return _length; }
- (NSUInteger)alignment { return _alignment; }
- (id)device { return _device; }
- (BOOL)bufferLayoutMatchesFrontEndLayout { return YES; }
// The AIR reader has already assigned the packed offsets used by the translator.
- (void)setStructType:(MTLStructType *)type withDevice:(id)device { (void)type; (void)device; }
- (NSUInteger)constantOffsetAtIndex:(NSUInteger)index {
    RDNA4ArgumentField *field = [self field:index];
    return field && field.type != MTLDataTypePointer && field.type != MTLDataTypeTexture &&
        field.type != MTLDataTypeSampler && field.type != MTLDataTypeRenderPipeline &&
        field.type != MTLDataTypeComputePipeline &&
        field.type != MTLDataTypeIndirectCommandBuffer &&
        !rdna4_rt_arg_is_rt_data_type(field.type) ? field.offset : NSUIntegerMax;
}
- (id)bufferLayoutForResourceAtIndex:(NSUInteger)index { return [self field:index].nested; }
@end


@implementation RDNA4ArgumentSamplerAlias
@end

@implementation RDNA4ArgumentResourceToken
- (instancetype)init {
    if ((self = [super init])) _aliases = [NSMutableArray array];
    return self;
}
@end

static NSMutableDictionary<NSNumber *, RDNA4ArgumentResourceToken *> *rdna4_argument_tokens;
static NSLock *rdna4_argument_tokens_lock;
static _Atomic uint64_t rdna4_argument_token_id;
static char rdna4_argument_token_key;
static void rdna4_argument_tokens_init(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{ rdna4_argument_tokens = [NSMutableDictionary dictionary]; rdna4_argument_tokens_lock = [NSLock new]; });
}

static id rdna4_argument_token_identity(id object, BOOL texture) {
    if (!object || texture) return object;
    SEL selector = NSSelectorFromString(@"rdna4ArgumentIdentity");
    if (![object respondsToSelector:selector]) return object;
    return ((id (*)(id, SEL))objc_msgSend)(object, selector);
}

static void rdna4_argument_token_prune_aliases_locked(RDNA4ArgumentResourceToken *entry) {
    for (RDNA4ArgumentSamplerAlias *alias in [entry.aliases copy])
        if (!alias.object) [entry.aliases removeObject:alias];
}

static id rdna4_argument_token_sampler_alias_locked(RDNA4ArgumentResourceToken *entry,
                                                     id device, NSSet *scope) {
    rdna4_argument_token_prune_aliases_locked(entry);
    for (RDNA4ArgumentSamplerAlias *alias in entry.aliases) {
        id object = alias.object;
        if (!object || alias.device != device) continue;
        if (scope && ![scope containsObject:object]) continue;
        return object;
    }
    return nil;
}

static void rdna4_argument_token_prune_locked(void) {
    for (NSNumber *key in rdna4_argument_tokens.allKeys) {
        RDNA4ArgumentResourceToken *entry = rdna4_argument_tokens[key];
        if (entry.texture) {
            if (!entry.object) [rdna4_argument_tokens removeObjectForKey:key];
        } else {
            rdna4_argument_token_prune_aliases_locked(entry);
            if (!entry.aliases.count) [rdna4_argument_tokens removeObjectForKey:key];
        }
    }
}

static uint64_t rdna4_argument_resource_token(id object, id device, BOOL texture) {
    if (!object) return 0;
    id identity = rdna4_argument_token_identity(object, texture);
    if (!identity) return 0;
    rdna4_argument_tokens_init();
    [rdna4_argument_tokens_lock lock];
    NSNumber *number = objc_getAssociatedObject(identity, &rdna4_argument_token_key);
    uint64_t token = number.unsignedLongLongValue;
    if (!token) {
        uint64_t identifier = atomic_fetch_add(&rdna4_argument_token_id, 1) + 1;
        if (!identifier || identifier > 0x0000ffffffffffffull) { [rdna4_argument_tokens_lock unlock]; return 0; }
        token = 0x5244000000000000ull | identifier;
        number = @(token);
        objc_setAssociatedObject(identity, &rdna4_argument_token_key, number, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }
    RDNA4ArgumentResourceToken *entry = rdna4_argument_tokens[@(token)];
    if (entry && entry.texture != texture) { [rdna4_argument_tokens_lock unlock]; return 0; }
    if (!entry) {
        entry = [RDNA4ArgumentResourceToken new];
        entry.texture = texture;
        rdna4_argument_tokens[@(token)] = entry;
    }
    entry.identity = identity;
    entry.device = device;
    if (texture) {
        entry.object = object;
    } else {
        entry.object = nil;
        rdna4_argument_token_prune_aliases_locked(entry);
        BOOL found = NO;
        for (RDNA4ArgumentSamplerAlias *alias in entry.aliases)
            if (alias.object == object) {
                alias.device = device;
                found = YES;
                break;
            }
        if (!found) {
            RDNA4ArgumentSamplerAlias *alias = [RDNA4ArgumentSamplerAlias new];
            alias.object = object;
            alias.device = device;
            [entry.aliases addObject:alias];
        }
    }
    if ((token & 255u) == 0) {
        rdna4_argument_token_prune_locked();
    }
    [rdna4_argument_tokens_lock unlock];
    return token;
}
GARNET_INTERNAL id rdna4_argument_resource_for_token(uint64_t token, BOOL texture, id device) {
    if (!token) return nil;
    rdna4_argument_tokens_init();
    [rdna4_argument_tokens_lock lock];
    RDNA4ArgumentResourceToken *entry = rdna4_argument_tokens[@(token)];
    id object = nil;
    if (entry && entry.texture == texture) {
        if (texture) {
            if (entry.device == device) object = entry.object;
            if (!entry.object) [rdna4_argument_tokens removeObjectForKey:@(token)];
        } else {
            object = rdna4_argument_token_sampler_alias_locked(entry, device, nil);
            if (!entry.aliases.count) [rdna4_argument_tokens removeObjectForKey:@(token)];
        }
    }
    [rdna4_argument_tokens_lock unlock];
    return object;
}

GARNET_INTERNAL NSArray *rdna4_argument_resource_snapshot(id device, NSSet *declared, NSSet *samplers) {
    rdna4_argument_tokens_init();
    NSMutableArray *rows = [NSMutableArray array];
    [rdna4_argument_tokens_lock lock];
    rdna4_argument_token_prune_locked();
    for (NSNumber *number in rdna4_argument_tokens) {
        RDNA4ArgumentResourceToken *entry = rdna4_argument_tokens[number];
        if (entry.texture) {
            id object = entry.object;
            id heap = object ? [object heap] : nil;
            if (object && entry.device == device && ![object isAliasable] &&
                ([declared containsObject:object] || (heap && [declared containsObject:heap])))
                [rows addObject:@[number, object, @YES]];
        } else {
            id object = rdna4_argument_token_sampler_alias_locked(entry, device, samplers);
            if (object) [rows addObject:@[number, object, @NO]];
        }
    }
    [rdna4_argument_tokens_lock unlock];
    [rows sortUsingComparator:^NSComparisonResult(NSArray *left, NSArray *right) {
        uint64_t a = [left[0] unsignedLongLongValue], b = [right[0] unsignedLongLongValue];
        return a < b ? NSOrderedAscending : a > b ? NSOrderedDescending : NSOrderedSame;
    }];
    return rows;
}

@implementation RDNA4ArgumentWriteIntent
@end

GARNET_INTERNAL BOOL rdna4_argument_sampler_intent_matches(id token, id buffer, NSRange range,
                                                   BOOL fillOnly) {
    RDNA4ArgumentWriteIntent *intent = token;
    return intent && intent->buffer == buffer && NSEqualRanges(intent->range, range) &&
        intent->writeRanges.count == 1 &&
        NSEqualRanges(intent->writeRanges[0].rangeValue, range) &&
        intent->fillOnly == fillOnly;
}

GARNET_INTERNAL BOOL rdna4_argument_sampler_intent_matches_ranges(
        id token, id buffer, NSRange range, NSArray<NSValue *> *writeRanges,
        BOOL fillOnly) {
    RDNA4ArgumentWriteIntent *intent = token;
    if (!intent || intent->buffer != buffer || !NSEqualRanges(intent->range, range) ||
        intent->fillOnly != fillOnly || intent->writeRanges.count != writeRanges.count)
        return NO;
    for (NSUInteger i = 0; i < writeRanges.count; ++i)
        if (!NSEqualRanges(intent->writeRanges[i].rangeValue,
                           writeRanges[i].rangeValue))
            return NO;
    return YES;
}

@implementation RDNA4ArgumentCompletedWrite
@end

@implementation RDNA4ArgumentSamplerOwners
- (instancetype)init {
    if ((self = [super init])) {
        _slots = [NSMutableDictionary dictionary]; _possible = [NSMutableDictionary dictionary];
        _writers = [NSMutableArray array]; _completed = [NSMutableArray array];
    }
    return self;
}
@end

static char rdna4_argument_sampler_owners_key;
static RDNA4ArgumentSamplerOwners *rdna4_argument_sampler_owners(id buffer, BOOL create) {
    RDNA4ArgumentSamplerOwners *owners = objc_getAssociatedObject(buffer, &rdna4_argument_sampler_owners_key);
    if (!owners && create) {
        owners = [RDNA4ArgumentSamplerOwners new];
        objc_setAssociatedObject(buffer, &rdna4_argument_sampler_owners_key, owners, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }
    return owners;
}

static void rdna4_argument_sampler_refine(id buffer);

static void rdna4_argument_sampler_fill_locked(RDNA4ArgumentSamplerOwners *owners, NSRange range) {
    for (NSNumber *key in owners.slots.allKeys)
        if (NSIntersectionRange(NSMakeRange(key.unsignedIntegerValue, sizeof(uint64_t)), range).length)
            [owners.slots removeObjectForKey:key];
    for (NSValue *key in owners.possible.allKeys) {
        NSRange old = key.rangeValue;
        if (!NSIntersectionRange(old, range).length) continue;
        NSSet *samplers = owners.possible[key];
        [owners.possible removeObjectForKey:key];
        if (old.location < range.location) {
            NSValue *leftKey = [NSValue valueWithRange:
                NSMakeRange(old.location, range.location - old.location)];
            NSMutableSet *left = [owners.possible[leftKey] mutableCopy] ?: [NSMutableSet set];
            [left unionSet:samplers];
            owners.possible[leftKey] = left;
        }
        if (NSMaxRange(range) < NSMaxRange(old)) {
            NSValue *rightKey = [NSValue valueWithRange:
                NSMakeRange(NSMaxRange(range), NSMaxRange(old) - NSMaxRange(range))];
            NSMutableSet *right = [owners.possible[rightKey] mutableCopy] ?: [NSMutableSet set];
            [right unionSet:samplers];
            owners.possible[rightKey] = right;
        }
    }
}

static NSDictionary *rdna4_argument_live_samplers(id device) {
    rdna4_argument_tokens_init();
    NSMutableDictionary *live = [NSMutableDictionary dictionary];
    [rdna4_argument_tokens_lock lock];
    rdna4_argument_token_prune_locked();
    for (NSNumber *number in rdna4_argument_tokens) {
        RDNA4ArgumentResourceToken *entry = rdna4_argument_tokens[number];
        if (entry.texture) continue;
        id object = rdna4_argument_token_sampler_alias_locked(entry, device, nil);
        if (object) live[number] = object;
    }
    [rdna4_argument_tokens_lock unlock];
    return live;
}

static rdna4_ae_range rdna4_argument_epoch_range(NSRange range) {
    return (rdna4_ae_range){ range.location, range.length };
}

static NSRange rdna4_argument_epoch_nsrange(rdna4_ae_range range) {
    return NSMakeRange((NSUInteger)range.location, (NSUInteger)range.length);
}

static void rdna4_argument_sampler_add_range(NSMutableArray<NSValue *> *ranges, NSRange range) {
    if (!range.length) return;
    NSMutableArray<NSValue *> *all = [ranges mutableCopy];
    [all addObject:[NSValue valueWithRange:range]];
    [all sortUsingComparator:^NSComparisonResult(NSValue *left, NSValue *right) {
        NSRange a = left.rangeValue, b = right.rangeValue;
        return a.location < b.location ? NSOrderedAscending :
            a.location > b.location ? NSOrderedDescending : NSOrderedSame;
    }];
    [ranges removeAllObjects];
    for (NSValue *value in all) {
        NSRange next = value.rangeValue;
        if (ranges.count && next.location <= NSMaxRange(ranges.lastObject.rangeValue)) {
            NSRange current = ranges.lastObject.rangeValue;
            NSUInteger end = MAX(NSMaxRange(current), NSMaxRange(next));
            ranges[ranges.count - 1] =
                [NSValue valueWithRange:NSMakeRange(current.location, end - current.location)];
        } else {
            [ranges addObject:value];
        }
    }
}

static void rdna4_argument_sampler_subtract_ranges(NSMutableArray<NSValue *> *ranges,
        NSRange cut) {
    if (!cut.length || !ranges.count) return;
    NSArray<NSValue *> *old = [ranges copy];
    [ranges removeAllObjects];
    rdna4_ae_range removed = rdna4_argument_epoch_range(cut);
    for (NSValue *value in old) {
        rdna4_ae_range pieces[2];
        size_t count = rdna4_ae_range_subtract(rdna4_argument_epoch_range(value.rangeValue),
                                               removed, pieces);
        for (size_t i = 0; i < count; ++i)
            [ranges addObject:[NSValue valueWithRange:
                rdna4_argument_epoch_nsrange(pieces[i])]];
    }
}

static void rdna4_argument_sampler_split_completed_locked(
        RDNA4ArgumentSamplerOwners *owners, NSRange cut) {
    if (!cut.length) return;
    NSArray<RDNA4ArgumentCompletedWrite *> *old = [owners.completed copy];
    for (RDNA4ArgumentCompletedWrite *write in old) {
        NSRange overlap = NSIntersectionRange(write->range, cut);
        if (!overlap.length) continue;
        [owners.completed removeObjectIdenticalTo:write];
        NSMutableArray<NSValue *> *outside =
            [NSMutableArray arrayWithObject:[NSValue valueWithRange:write->range]];
        rdna4_argument_sampler_subtract_ranges(outside, overlap);
        for (NSValue *value in outside) {
            RDNA4ArgumentCompletedWrite *piece = [RDNA4ArgumentCompletedWrite new];
            piece->serial = write->serial;
            piece->submissionEpoch = write->submissionEpoch;
            piece->hostGeneration = write->hostGeneration;
            piece->range = value.rangeValue;
            piece->candidates = write->candidates;
            piece->fillOnly = write->fillOnly;
            piece->exclusive = write->exclusive;
            [owners.completed addObject:piece];
        }
        RDNA4ArgumentCompletedWrite *hostProtected =
            [RDNA4ArgumentCompletedWrite new];
        hostProtected->serial = write->serial;
        hostProtected->submissionEpoch = write->submissionEpoch;
        hostProtected->hostGeneration = write->hostGeneration;
        hostProtected->range = overlap;
        hostProtected->candidates = write->candidates;
        hostProtected->fillOnly = write->fillOnly;
        hostProtected->exclusive = NO;
        [owners.completed addObject:hostProtected];
    }
}

// A later CPU setter owns its bytes regardless of an earlier completion callback.
static BOOL rdna4_argument_sampler_note_host_write_locked(
        RDNA4ArgumentSamplerOwners *owners, NSRange range) {
    if (!owners || !range.length) return NO;
    if (owners.hostGeneration != UINT64_MAX) owners.hostGeneration += 1;
    else owners.hostGenerationSaturated = YES;
    for (RDNA4ArgumentWriteIntent *intent in owners.writers) {
        if (!intent->active || !NSIntersectionRange(intent->range, range).length) continue;
        rdna4_argument_sampler_add_range(intent->hostOverlaps,
                                        NSIntersectionRange(intent->range, range));
    }
    rdna4_argument_sampler_split_completed_locked(owners, range);
    return owners.completed.count != 0;
}

// The mapped bytes and owner metadata become one atomic setter operation for readers.
GARNET_INTERNAL void rdna4_argument_sampler_store(id buffer, NSUInteger offset, uint64_t value, id sampler) {
    BOOL refine = NO;
    @synchronized (buffer) {
        uint8_t *bytes = [(id<MTLBuffer>)buffer contents];
        if (!bytes || offset > [buffer length] || sizeof value > [buffer length] - offset) return;
        NSRange range = NSMakeRange(offset, sizeof value);
        RDNA4ArgumentSamplerOwners *owners = rdna4_argument_sampler_owners(buffer, NO);
        if (owners) refine = rdna4_argument_sampler_note_host_write_locked(owners, range);
        memcpy(bytes + offset, &value, sizeof value);
        if (!owners) owners = rdna4_argument_sampler_owners(buffer, sampler != nil);
        if (!owners) return;
        rdna4_argument_sampler_fill_locked(owners, range);
        if (sampler) owners.slots[@(offset)] = @[@(value), sampler];
        refine = refine || owners.completed.count != 0;
    }
    if (refine) rdna4_argument_sampler_refine(buffer);
}

GARNET_INTERNAL void rdna4_argument_sampler_write(id buffer, NSUInteger offset, id sampler, uint64_t token) {
    rdna4_argument_sampler_store(buffer, offset, token, sampler);
}

static NSSet *rdna4_argument_buffer_samplers(id buffer, NSRange range, NSDictionary *live,
                                              BOOL *fullyScanned) {
    NSMutableSet *found = [NSMutableSet set];
    if (fullyScanned) *fullyScanned = NO;
    if (!range.length || range.location > [buffer length] ||
        range.length > [buffer length] - range.location) return found;
    @synchronized (buffer) {
        RDNA4ArgumentSamplerOwners *owners = rdna4_argument_sampler_owners(buffer, NO);
        BOOL pendingWriter = NO;
        // GPU candidates survive independently of mapped bytes, including work that has not executed yet.
        for (NSValue *key in owners.possible)
            if (NSIntersectionRange(key.rangeValue, range).length) [found unionSet:owners.possible[key]];
        for (RDNA4ArgumentWriteIntent *intent in owners.writers)
            if (NSIntersectionRange(intent->range, range).length) {
                [found unionSet:intent->candidates];
                if (intent->active) pendingWriter = YES;
            }
        for (RDNA4ArgumentCompletedWrite *write in owners.completed)
            if (NSIntersectionRange(write->range, range).length) [found unionSet:write->candidates];
        const uint8_t *bytes = [(id<MTLBuffer>)buffer contents];
        if (fullyScanned)
            *fullyScanned = rdna4_ae_copy_source_fully_scanned(bytes != NULL,
                [(id<MTLBuffer>)buffer storageMode] == MTLStorageModeShared, pendingWriter);
        for (NSNumber *key in owners.slots.allKeys) {
            NSUInteger offset = key.unsignedIntegerValue;
            if (!NSIntersectionRange(NSMakeRange(offset, sizeof(uint64_t)), range).length) continue;
            NSArray *slot = owners.slots[key];
            uint64_t token = 0;
            if (bytes) memcpy(&token, bytes + offset, sizeof token);
            if (!bytes || token == [slot[0] unsignedLongLongValue]) [found addObject:slot[1]];
            else [owners.slots removeObjectForKey:key];
        }
        if (bytes) {
            NSUInteger end = range.location + range.length;
            NSUInteger offset = range.location;
            NSUInteger alignment = offset & (sizeof(uint64_t) - 1);
            if (alignment) offset += sizeof(uint64_t) - alignment;
            for (; offset <= end && sizeof(uint64_t) <= end - offset; offset += sizeof(uint64_t)) {
                uint64_t token; memcpy(&token, bytes + offset, sizeof token);
                if ((token >> 48) != 0x5244) continue;
                id sampler = live[@(token)];
                if (sampler) [found addObject:sampler];
            }
        }
    }
    return found;
}

typedef struct {
    __unsafe_unretained RDNA4Buffer *buffer;
    __unsafe_unretained NSDictionary<NSNumber *, id> *live;
    RDNA4ArgumentSamplerOwners *owners;
    NSMutableSet *found;
    BOOL readBytes;
} RDNA4ArgumentScopeTokenScan;

static bool rdna4_argument_scope_visit_token(void *context,
        const rdna4_as_work *work, uint64_t offset, uint64_t width) {
    RDNA4ArgumentScopeTokenScan *scan = context;
    if (!scan || !work || width != RDNA4_AS_TOKEN_BYTES ||
        !scan->readBytes)
        return false;
    RDNA4Buffer *buffer = scan->buffer;
    if (offset > [buffer length] || width > [buffer length] - offset)
        return false;
    uint64_t token = 0;
    const uint8_t *bytes = (const uint8_t *)[(id<MTLBuffer>)buffer contents];
    if (!bytes) return false;
    memcpy(&token, bytes + offset, sizeof token);

    NSNumber *key = @(offset);
    NSArray *slot = scan->owners.slots[key];
    if (slot.count == 2) {
        uint64_t known = [slot[0] unsignedLongLongValue];
        if (token == known) [scan->found addObject:slot[1]];
        else [scan->owners.slots removeObjectForKey:key];
    }
    if ((token >> 48) == 0x5244) {
        id sampler = scan->live[@(token)];
        if (sampler) [scan->found addObject:sampler];
    }
    return true;
}

static BOOL rdna4_argument_scope_intersects(NSRange left, NSRange right) {
    return NSIntersectionRange(left, right).length != 0;
}

typedef struct {
    __unsafe_unretained RDNA4Buffer *buffer;
    NSMutableArray<NSValue *> *ranges;
    BOOL invalid;
} RDNA4ArgumentScopeRanges;

static bool rdna4_argument_scope_collect_range(void *context,
        const rdna4_as_work *work, uint64_t offset, uint64_t width) {
    (void)work;
    RDNA4ArgumentScopeRanges *scan = context;
    if (!scan || !scan->buffer || !scan->ranges || !width) {
        if (scan) scan->invalid = YES;
        return false;
    }
    NSUInteger length = [scan->buffer length];
    if (offset > length || width > length - offset ||
        offset > NSUIntegerMax || width > NSUIntegerMax) {
        scan->invalid = YES;
        return false;
    }
    NSRange range = NSMakeRange((NSUInteger)offset, (NSUInteger)width);
    if (scan->ranges.count) {
        NSRange last = scan->ranges.lastObject.rangeValue;
        if (range.location <= NSMaxRange(last)) {
            NSUInteger end = MAX(NSMaxRange(last), NSMaxRange(range));
            scan->ranges[scan->ranges.count - 1] =
                [NSValue valueWithRange:NSMakeRange(last.location,
                                                    end - last.location)];
            return true;
        }
    }
    [scan->ranges addObject:[NSValue valueWithRange:range]];
    return true;
}

static BOOL rdna4_argument_scope_ranges_intersect(
        NSArray<NSValue *> *ranges, NSRange range) {
    for (NSValue *value in ranges)
        if (rdna4_argument_scope_intersects(value.rangeValue, range))
            return YES;
    return NO;
}

static BOOL rdna4_argument_scope_contains_token(const rdna4_as_work *work,
                                                uint64_t offset) {
    if (!work || offset < work->offset || !work->count || !work->stride ||
        (offset - work->offset) % work->stride)
        return false;
    return (offset - work->offset) / work->stride < work->count;
}

static BOOL rdna4_argument_sampler_scope_work(
        id device, RDNA4Buffer *buffer, const rdna4_as_work *work,
        NSDictionary *__strong *live, NSMutableSet *samplers,
        NSMutableArray<NSDictionary *> *writes, NSString **why) {
    if (!buffer) return YES;
    uint64_t extent = 0;
    if (rdna4_as_work_extent(work, &extent) != RDNA4_AS_OK ||
        work->offset > [buffer length] ||
        extent > [buffer length] - work->offset) {
        if (why) *why = @"argument sampler scope work exceeds its buffer";
        return NO;
    }
    NSRange envelope = NSMakeRange((NSUInteger)work->offset,
                                   (NSUInteger)extent);
    NSMutableArray<NSValue *> *workRanges = [NSMutableArray array];
    RDNA4ArgumentScopeRanges rangeScan = {
        .buffer = buffer,
        .ranges = workRanges,
    };
    rdna4_as_status rangeStatus = rdna4_as_visit_byte_ranges(
        work, rdna4_argument_scope_collect_range, &rangeScan);
    if (rangeStatus != RDNA4_AS_OK || rangeScan.invalid ||
        !workRanges.count) {
        if (why) *why = [NSString stringWithFormat:
            @"argument sampler scope cannot enumerate exact write ranges: %s",
            rdna4_argument_scan_status_name(rangeStatus)];
        return NO;
    }
    NSMutableSet *found = [NSMutableSet set];
    @synchronized (buffer) {
        RDNA4ArgumentSamplerOwners *owners =
            rdna4_argument_sampler_owners(buffer, NO);
        for (NSValue *key in owners.possible)
            if (rdna4_argument_scope_ranges_intersect(
                    workRanges, key.rangeValue))
                [found unionSet:owners.possible[key]];
        for (RDNA4ArgumentWriteIntent *intent in owners.writers)
            if (intent->active &&
                rdna4_argument_scope_ranges_intersect(
                    workRanges, intent->range))
                [found unionSet:intent->candidates];
        for (RDNA4ArgumentCompletedWrite *write in owners.completed)
            if (rdna4_argument_scope_ranges_intersect(
                    workRanges, write->range))
                [found unionSet:write->candidates];

        if (work->mode == RDNA4_AS_SCAN_CPU_MAPPED) {
            if (!*live) *live = rdna4_argument_live_samplers(device);
            RDNA4ArgumentScopeTokenScan tokenScan = {
                .buffer = buffer,
                .live = *live,
                .owners = owners,
                .found = found,
                .readBytes = YES,
            };
            if (![buffer contents] ||
                rdna4_as_visit_tokens(work, rdna4_argument_scope_visit_token,
                                      &tokenScan) != RDNA4_AS_OK) {
                if (why) *why = @"argument sampler scope cannot scan mapped token work";
                return NO;
            }
        } else if (work->mode != RDNA4_AS_SCAN_GPU_SUMMARY &&
                   work->mode != RDNA4_AS_SCAN_OPAQUE_CONSERVATIVE) {
            if (why) *why = @"argument sampler scope has an unknown scan mode";
            return NO;
        } else if (owners) {
            /*
             * Private/opaque work never reads host contents. Exact
             * owner slots are metadata, so they can still contribute
             * without turning the GPU summary or transport range into
             * a CPU parameter decision.
             */
            for (NSNumber *key in owners.slots)
                if (rdna4_argument_scope_contains_token(
                        work, key.unsignedLongLongValue) &&
                    [owners.slots[key] count] == 2)
                    [found addObject:owners.slots[key][1]];
        }
    }
    [samplers unionSet:found];
    if (work->flags & RDNA4_AS_WRITABLE) {
        [writes addObject:@{
            @"buffer": buffer,
            @"range": [NSValue valueWithRange:envelope],
            @"ranges": [workRanges copy],
            @"candidates": [found copy],
            @"fillOnly": @NO,
        }];
    }
    return YES;
}

GARNET_INTERNAL BOOL rdna4_argument_sampler_scope(
        id device, const RDNA4ArgumentScanPrepared *scan,
        NSSet *declaredBuffers, NSSet *writable,
        NSMutableSet *samplers, NSMutableArray<NSDictionary *> *writes,
        NSString **why) {
    if (!scan || !samplers || !writes) {
        if (why) *why = @"argument sampler scope has no scan plan";
        return NO;
    }
    NSDictionary *live = nil;
    BOOL needsDeclaredTransports = NO;
    NSMutableSet *fullyScanned = [NSMutableSet set];
    for (size_t stageIndex = 0; stageIndex < scan->count; ++stageIndex) {
        const RDNA4ArgumentScanStage *stage = &scan->stages[stageIndex];
        if (!stage->state || (stage->count && !stage->works)) {
            if (why) *why = @"argument sampler scope has malformed stage work";
            return NO;
        }
        const rdna4_argument_scan_reflection_plan *reflection = stage->state->scanPlan;
        for (size_t i = 0; reflection && i < reflection->plan.count; ++i) {
            const rdna4_as_span *span = &reflection->plan.spans[i];
            if (span->kind == RDNA4_AS_KIND_BUFFER ||
                span->kind == RDNA4_AS_KIND_UNKNOWN ||
                (span->flags & RDNA4_AS_DYNAMIC))
                needsDeclaredTransports = YES;
        }
        for (size_t workIndex = 0; workIndex < stage->count; ++workIndex) {
            const rdna4_as_work *work = &stage->works[workIndex];
            if (work->root >= RDNA4_AS_ROOTS || work->width != RDNA4_AS_TOKEN_BYTES) {
                if (why) *why = @"argument sampler scope has malformed token work";
                return NO;
            }
            RDNA4Buffer *buffer = stage->state->buf[work->root].b;
            if (!rdna4_argument_sampler_scope_work(device, buffer, work, &live,
                                                    samplers, writes, why)) return NO;
            if (buffer && !work->offset && work->stride == RDNA4_AS_TOKEN_BYTES &&
                work->count == buffer.length / RDNA4_AS_TOKEN_BYTES &&
                !!(work->flags & RDNA4_AS_WRITABLE) ==
                    rdna4_argument_scan_writable(writable, buffer))
                [fullyScanned addObject:buffer];
        }
    }
    // BDA/dynamic children need not occupy a setBuffer slot. Only this command's
    // declared closure is eligible; typed-only plans keep their sparse scans.
    if (needsDeclaredTransports) for (id object in declaredBuffers) {
        if (![object isKindOfClass:[RDNA4Buffer class]]) continue;
        RDNA4Buffer *buffer = object;
        if (buffer.device != device || [buffer isAliasable] ||
            [fullyScanned containsObject:buffer] ||
            buffer.length < RDNA4_AS_TOKEN_BYTES) continue;
        rdna4_as_bound bound = {
            .present = true,
            .mapped = buffer.contents != NULL,
            .storage = rdna4_argument_scan_storage(buffer.storageMode),
        };
        rdna4_as_work work = {
            .kind = RDNA4_AS_KIND_UNKNOWN,
            .flags = RDNA4_AS_TRANSPORT |
                (rdna4_argument_scan_writable(writable, buffer) ? RDNA4_AS_WRITABLE : 0),
            .mode = (uint8_t)rdna4_as_scan_mode_for(&bound),
            .stride = RDNA4_AS_TOKEN_BYTES,
            .count = buffer.length / RDNA4_AS_TOKEN_BYTES,
            .width = RDNA4_AS_TOKEN_BYTES,
        };
        RDNA4LOGR("argscan declared transport: %llu bytes, %llu words, mode %u",
            (unsigned long long)buffer.length, (unsigned long long)work.count,
            (unsigned)work.mode);
        if (!rdna4_argument_sampler_scope_work(device, buffer, &work, &live,
                                                samplers, writes, why)) return NO;
    }
    if (writes.count) {
        NSSet *allCandidates = [samplers copy];
        for (NSUInteger i = 0; i < writes.count; ++i) {
            NSMutableDictionary *entry = [writes[i] mutableCopy];
            entry[@"candidates"] = allCandidates;
            writes[i] = [entry copy];
        }
    }
    return YES;
}

GARNET_INTERNAL RDNA4ArgumentWriteIntent *rdna4_argument_sampler_intent_ranges(id buffer, NSRange range,
        NSArray<NSValue *> *writeRanges, NSSet *candidates, BOOL fillOnly) {
    if (!buffer || !range.length ||
        !rdna4_ae_range_valid(rdna4_argument_epoch_range(range), [buffer length]) ||
        !writeRanges.count) return nil;
    rdna4_ae_range envelope = rdna4_argument_epoch_range(range);
    for (NSValue *value in writeRanges) {
        NSRange exact = value.rangeValue;
        rdna4_ae_range part = rdna4_argument_epoch_range(exact);
        rdna4_ae_range overlap = rdna4_ae_range_intersection(envelope, part);
        if (!part.length || !rdna4_ae_range_valid(part, [buffer length]) ||
            overlap.location != part.location || overlap.length != part.length) return nil;
    }
    static _Atomic uint64_t nextSerial;
    uint64_t previous = atomic_load(&nextSerial);
    do {
        if (previous == UINT64_MAX) return nil;
    } while (!atomic_compare_exchange_weak(&nextSerial, &previous, previous + 1));
    uint64_t serial = previous + 1;
    RDNA4ArgumentWriteIntent *intent = [RDNA4ArgumentWriteIntent new];
    intent->buffer = buffer;
    intent->serial = serial;
    intent->range = range;
    intent->writeRanges = [writeRanges copy];
    intent->hostOverlaps = [NSMutableArray array];
    intent->candidates = [candidates copy] ?: [NSSet set];
    intent->fillOnly = fillOnly;
    intent->active = YES;
    @synchronized (buffer) {
        RDNA4ArgumentSamplerOwners *owners = rdna4_argument_sampler_owners(buffer, YES);
        intent->hostGeneration = owners.hostGeneration;
        [owners.writers addObject:intent];
    }
    return intent;
}

GARNET_INTERNAL RDNA4ArgumentWriteIntent *rdna4_argument_sampler_intent(id buffer, NSRange range,
        NSSet *candidates, BOOL fillOnly) {
    return rdna4_argument_sampler_intent_ranges(buffer, range,
        @[[NSValue valueWithRange:range]], candidates, fillOnly);
}

GARNET_INTERNAL void rdna4_argument_sampler_add_candidates(id token, NSSet *candidates) {
    RDNA4ArgumentWriteIntent *intent = token;
    id buffer = intent ? intent->buffer : nil;
    if (!buffer || !candidates.count) return;
    @synchronized (buffer) {
        if (!intent->active) return;
        NSMutableSet *merged = [intent->candidates mutableCopy];
        [merged unionSet:candidates];
        intent->candidates = merged;
    }
}

GARNET_INTERNAL RDNA4ArgumentWriteIntent *rdna4_argument_sampler_copy(id source, NSUInteger sourceOffset,
        id destination, NSUInteger offset, NSUInteger size) {
    if (!source || !destination || !size ||
        !rdna4_ae_range_valid((rdna4_ae_range){ sourceOffset, size }, [source length]) ||
        !rdna4_ae_range_valid((rdna4_ae_range){ offset, size }, [destination length])) return nil;
    NSRange sourceRange = NSMakeRange(sourceOffset, size);
    NSDictionary *live = rdna4_argument_live_samplers([source device]);
    BOOL fullyScanned = NO;
    NSSet *found = rdna4_argument_buffer_samplers(source, sourceRange, live, &fullyScanned);
    NSUInteger tokenMask = sizeof(uint64_t) - 1;
    BOOL wholeTokenWords = !(sourceOffset & tokenMask) && !(offset & tokenMask) &&
        !(size & tokenMask);
    BOOL fillOnly = rdna4_ae_copy_fill_only(fullyScanned, wholeTokenWords, found.count != 0);
    return rdna4_argument_sampler_intent(destination, NSMakeRange(offset, size), found,
                                         fillOnly);
}

GARNET_INTERNAL uint64_t rdna4_argument_next_submission_epoch(void) {
    static _Atomic uint64_t nextEpoch;
    uint64_t previous = atomic_load(&nextEpoch);
    for (;;) {
        if (previous == UINT64_MAX) return UINT64_MAX;
        if (atomic_compare_exchange_weak(&nextEpoch, &previous, previous + 1))
            return previous + 1;
    }
}

GARNET_INTERNAL void rdna4_argument_sampler_submit(id token, uint64_t submissionEpoch) {
    RDNA4ArgumentWriteIntent *intent = token;
    id buffer = intent ? intent->buffer : nil;
    if (!buffer || !submissionEpoch) return;
    @synchronized (buffer) {
        if (!intent->active || intent->submitted) return;
        intent->submissionEpoch = submissionEpoch;
        intent->submitted = YES;
    }
}

GARNET_INTERNAL void rdna4_argument_sampler_abandon(id token) {
    RDNA4ArgumentWriteIntent *intent = token;
    id buffer = intent ? intent->buffer : nil;
    if (!buffer) return;
    BOOL refine = NO;
    @synchronized (buffer) {
        if (!intent->active) return;
        intent->active = NO;
        RDNA4ArgumentSamplerOwners *owners = rdna4_argument_sampler_owners(buffer, NO);
        [owners.writers removeObjectIdenticalTo:intent];
        refine = owners.completed.count != 0;
    }
    if (refine) rdna4_argument_sampler_refine(buffer);
}

static void rdna4_argument_sampler_add_possible_locked(RDNA4ArgumentSamplerOwners *owners,
        NSRange range, NSSet *candidates) {
    if (!range.length || !candidates.count) return;
    NSValue *key = [NSValue valueWithRange:range];
    NSMutableSet *possible = [owners.possible[key] mutableCopy] ?: [NSMutableSet set];
    [possible unionSet:candidates];
    owners.possible[key] = possible;
}

static void rdna4_argument_sampler_scan_locked(RDNA4ArgumentSamplerOwners *owners,
        NSRange range, const uint8_t *bytes, NSDictionary *live) {
    if (!bytes || !range.length) return;
    NSUInteger end = range.location + range.length;
    NSUInteger offset = range.location;
    NSUInteger alignment = offset & (sizeof(uint64_t) - 1);
    if (alignment) offset += sizeof(uint64_t) - alignment;
    for (; offset <= end && sizeof(uint64_t) <= end - offset; offset += sizeof(uint64_t)) {
        uint64_t token;
        memcpy(&token, bytes + offset, sizeof token);
        if ((token >> 48) != 0x5244) continue;
        id sampler = live[@(token)];
        if (sampler) owners.slots[@(offset)] = @[@(token), sampler];
    }
}

static void rdna4_argument_sampler_refine_locked(id buffer, RDNA4ArgumentSamplerOwners *owners,
        NSDictionary *live) {
    const uint8_t *bytes = [(id<MTLBuffer>)buffer contents];
    for (;;) {
        NSMutableArray<RDNA4ArgumentCompletedWrite *> *group = nil;
        NSRange span = NSMakeRange(0, 0);
        NSArray<RDNA4ArgumentCompletedWrite *> *ordered = [owners.completed
            sortedArrayUsingComparator:^NSComparisonResult(RDNA4ArgumentCompletedWrite *a,
                                                           RDNA4ArgumentCompletedWrite *b) {
                if (a->submissionEpoch != b->submissionEpoch)
                    return a->submissionEpoch < b->submissionEpoch ? NSOrderedAscending :
                        NSOrderedDescending;
                return a->serial < b->serial ? NSOrderedAscending :
                    a->serial > b->serial ? NSOrderedDescending : NSOrderedSame;
            }];
        for (RDNA4ArgumentCompletedWrite *seed in ordered) {
            group = [NSMutableArray arrayWithObject:seed];
            span = seed->range;
            BOOL expanded;
            do {
                expanded = NO;
                for (RDNA4ArgumentCompletedWrite *other in owners.completed) {
                    if ([group containsObject:other] || !NSIntersectionRange(span, other->range).length) continue;
                    [group addObject:other];
                    NSUInteger end = MAX(NSMaxRange(span), NSMaxRange(other->range));
                    span.location = MIN(span.location, other->range.location);
                    span.length = end - span.location;
                    expanded = YES;
                }
            } while (expanded);
            BOOL blocked = NO;
            for (RDNA4ArgumentWriteIntent *writer in owners.writers) {
                if (!writer->active ||
                    NSIntersectionRange(writer->range, span).length) {
                    blocked = YES;
                    break;
                }
            }
            if (!blocked) break;
            group = nil;
        }
        if (!group) return;
        NSMutableSet *candidates = [NSMutableSet set];
        BOOL allFill = YES;
        for (RDNA4ArgumentCompletedWrite *write in group) {
            [candidates unionSet:write->candidates];
            allFill = allFill && write->fillOnly;
        }
        NSMutableArray<NSValue *> *protectedRanges = [NSMutableArray array];
        for (RDNA4ArgumentCompletedWrite *write in group)
            if (!write->exclusive)
                rdna4_argument_sampler_add_range(protectedRanges, write->range);
        NSMutableArray<NSValue *> *exclusiveRanges =
            [NSMutableArray arrayWithObject:[NSValue valueWithRange:span]];
        for (NSValue *value in protectedRanges)
            rdna4_argument_sampler_subtract_ranges(exclusiveRanges, value.rangeValue);

        if (bytes) {
            for (NSValue *value in exclusiveRanges) {
                NSRange exact = value.rangeValue;
                rdna4_argument_sampler_fill_locked(owners, exact);
                rdna4_argument_sampler_scan_locked(owners, exact, bytes, live);
            }
            if (candidates.count)
                for (NSValue *value in protectedRanges)
                    rdna4_argument_sampler_add_possible_locked(owners, value.rangeValue,
                                                              candidates);
        } else if (allFill) {
            for (NSValue *value in exclusiveRanges)
                rdna4_argument_sampler_fill_locked(owners, value.rangeValue);
        } else if (candidates.count) {
            rdna4_argument_sampler_add_possible_locked(owners, span, candidates);
        }
        [owners.completed removeObjectsInArray:group];
    }
}

static void rdna4_argument_sampler_refine(id buffer) {
    if (!buffer) return;
    @synchronized (buffer) {
        RDNA4ArgumentSamplerOwners *owners = rdna4_argument_sampler_owners(buffer, NO);
        if (!owners || !owners.completed.count) return;
        rdna4_argument_sampler_refine_locked(buffer, owners,
            rdna4_argument_live_samplers([buffer device]));
    }
}

GARNET_INTERNAL void rdna4_argument_sampler_complete(id token) {
    RDNA4ArgumentWriteIntent *intent = token;
    id buffer = intent ? intent->buffer : nil;
    if (!buffer) return;
    @synchronized (buffer) {
        if (!intent->active) return;
        // An unaccepted intent must use abandon, never a successful completion.
        if (!intent->submitted) return;
        RDNA4ArgumentSamplerOwners *owners = rdna4_argument_sampler_owners(buffer, NO);
        if (!owners) return;
        intent->active = NO;
        [owners.writers removeObjectIdenticalTo:intent];
        for (NSValue *value in intent->writeRanges) {
            NSMutableArray<NSValue *> *exclusiveRanges =
                [NSMutableArray arrayWithObject:value];
            for (NSValue *overlap in intent->hostOverlaps)
                rdna4_argument_sampler_subtract_ranges(exclusiveRanges, overlap.rangeValue);
            for (NSValue *exclusive in exclusiveRanges) {
                RDNA4ArgumentCompletedWrite *write = [RDNA4ArgumentCompletedWrite new];
                write->serial = intent->serial;
                write->submissionEpoch = intent->submissionEpoch;
                write->hostGeneration = intent->hostGeneration;
                write->range = exclusive.rangeValue;
                write->candidates = intent->candidates;
                write->fillOnly = intent->fillOnly;
                write->exclusive = YES;
                [owners.completed addObject:write];
            }
            for (NSValue *overlap in intent->hostOverlaps) {
                NSRange protectedRange = NSIntersectionRange(value.rangeValue,
                                                             overlap.rangeValue);
                if (!protectedRange.length) continue;
                RDNA4ArgumentCompletedWrite *write = [RDNA4ArgumentCompletedWrite new];
                write->serial = intent->serial;
                write->submissionEpoch = intent->submissionEpoch;
                write->hostGeneration = intent->hostGeneration;
                write->range = protectedRange;
                write->candidates = intent->candidates;
                write->fillOnly = intent->fillOnly;
                write->exclusive = NO;
                [owners.completed addObject:write];
            }
        }
        if (intent->submissionEpoch > owners.lastCompletedEpoch)
            owners.lastCompletedEpoch = intent->submissionEpoch;
        rdna4_argument_sampler_refine_locked(buffer, owners,
            rdna4_argument_live_samplers([buffer device]));
    }
}

@implementation RDNA4ArgumentEncoder
- (instancetype)initWithLayout:(RDNA4ArgumentLayout *)layout device:(id)device {
    if (!(self = [super init]) || !layout || !layout.encodedLength) return nil;
    _layout = layout; _device = device;
    return self;
}
- (id)device { return _device; }
- (NSString *)label { return _label; }
- (void)setLabel:(NSString *)label { _label = [label copy]; }
- (NSUInteger)encodedLength { return _layout.encodedLength; }
- (NSUInteger)alignment { return _layout.alignment; }
- (RDNA4ArgumentLayout *)rdna4Layout { return _layout; }
- (void)setArgumentBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset {
    _buffer = nil; _offset = 0;
    if (!buffer) return;
    if (![buffer isKindOfClass:RDNA4Buffer.class] || buffer.device != _device || !buffer.contents ||
        offset % self.alignment || offset > buffer.length || self.encodedLength > buffer.length - offset)
        [NSException raise:NSInvalidArgumentException format:@"Argument buffer destination is unmapped, misaligned, or too small"];
    _buffer = (RDNA4Buffer *)buffer; _offset = offset;
}
- (void)setArgumentBuffer:(id<MTLBuffer>)buffer startOffset:(NSUInteger)startOffset arrayElement:(NSUInteger)element {
    if (element > (NSUIntegerMax - startOffset) / self.encodedLength)
        [NSException raise:NSInvalidArgumentException format:@"Argument buffer array offset overflows"];
    [self setArgumentBuffer:buffer offset:startOffset + element * self.encodedLength];
}
- (void *)rdna4Destination:(NSUInteger)index type:(MTLDataType)type {
    RDNA4ArgumentField *field = [_layout field:index];
    BOOL resource = field.type == MTLDataTypePointer || field.type == MTLDataTypeTexture ||
        field.type == MTLDataTypeSampler || field.type == MTLDataTypeRenderPipeline ||
        field.type == MTLDataTypeComputePipeline ||
        field.type == MTLDataTypeIndirectCommandBuffer ||
        rdna4_rt_arg_is_rt_data_type(field.type);
    if (!_buffer || !field || (type == MTLDataTypeNone ? resource : field.type != type) ||
        field.offset > self.encodedLength || field.size > self.encodedLength - field.offset ||
        (resource && field.size != sizeof(uint64_t)))
        [NSException raise:NSInvalidArgumentException format:@"Argument ID %lu has no matching destination", (unsigned long)index];
    return (uint8_t *)[(id<MTLBuffer>)_buffer contents] + _offset + field.offset;
}
- (uint64_t)rdna4BufferAddress:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index {
    uint64_t address = 0;
    if (buffer) {
        NSUInteger needed = [_layout field:index].nested.encodedLength;
        if (![buffer isKindOfClass:RDNA4Buffer.class] || buffer.device != _device || offset > buffer.length ||
            needed > buffer.length - offset)
            [NSException raise:NSInvalidArgumentException format:@"Argument buffer resource or offset is invalid"];
        uint64_t base = [(RDNA4Buffer *)buffer gpuAddress];
        if (!base || base > UINT64_MAX - offset)
            [NSException raise:NSInvalidArgumentException format:@"Argument buffer resource or offset is invalid"];
        address = base + offset;
    }
    return address;
}
- (void)setBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index {
    (void)[self rdna4Destination:index type:MTLDataTypePointer];
    uint64_t address = [self rdna4BufferAddress:buffer offset:offset atIndex:index];
    rdna4_argument_sampler_store(_buffer, _offset + [_layout field:index].offset, address, nil);
    if (buffer && ([_layout field:index].nested != nil || objc_getAssociatedObject(buffer, &rdna4_icb_arg_owners_key) != nil))
        rdna4_icb_arg_note(_buffer, nil, nil, (RDNA4Buffer *)buffer);
    if (buffer && ([_layout field:index].nested != nil || objc_getAssociatedObject(buffer, &rdna4_rt_arg_owners_key) != nil))
        rdna4_rt_arg_note(_buffer, nil, nil, (RDNA4Buffer *)buffer);
}
- (void)setBuffers:(const id<MTLBuffer> __unsafe_unretained *)buffers offsets:(const NSUInteger *)offsets withRange:(NSRange)range {
    if ((!buffers || !offsets) && range.length)
        [NSException raise:NSInvalidArgumentException format:@"Argument buffer arrays are null"];
    [self rdna4ValidateRange:range type:MTLDataTypePointer];
    for (NSUInteger n = 0; n < range.length; ++n)
        (void)[self rdna4BufferAddress:buffers[n] offset:offsets[n] atIndex:range.location + n];
    for (NSUInteger n = 0; n < range.length; ++n) [self setBuffer:buffers[n] offset:offsets[n] atIndex:range.location + n];
}
- (uint64_t)rdna4TextureToken:(id<MTLTexture>)texture {
    uint64_t token = 0;
    if (texture) {
        if (![texture isKindOfClass:RDNA4Texture.class] || texture.device != _device)
            [NSException raise:NSInvalidArgumentException format:@"Argument texture has an invalid class or device"];
        token = rdna4_argument_resource_token((RDNA4Texture *)texture, _device, YES);
        if (!token) [NSException raise:NSInvalidArgumentException format:@"Argument texture token could not be allocated"];
    }
    return token;
}
- (void)setTexture:(id<MTLTexture>)texture atIndex:(NSUInteger)index {
    (void)[self rdna4Destination:index type:MTLDataTypeTexture];
    uint64_t token = [self rdna4TextureToken:texture];
    rdna4_argument_sampler_store(_buffer, _offset + [_layout field:index].offset, token, nil);
}
- (uint64_t)rdna4SamplerToken:(id<MTLSamplerState>)sampler {
    uint64_t token = 0;
    if (sampler) {
        if (![sampler isKindOfClass:RDNA4SamplerState.class] || sampler.device != _device)
            [NSException raise:NSInvalidArgumentException format:@"Argument sampler has an invalid class or device"];
        token = rdna4_argument_resource_token((RDNA4SamplerState *)sampler, _device, NO);
        if (!token) [NSException raise:NSInvalidArgumentException format:@"Argument sampler token could not be allocated"];
    }
    return token;
}
- (void)setSamplerState:(id<MTLSamplerState>)sampler atIndex:(NSUInteger)index {
    (void)[self rdna4Destination:index type:MTLDataTypeSampler];
    uint64_t token = [self rdna4SamplerToken:sampler];
    rdna4_argument_sampler_write(_buffer, _offset + [_layout field:index].offset, sampler, token);
}
- (void)rdna4ValidateRange:(NSRange)range type:(MTLDataType)type {
    if (range.length > NSUIntegerMax - range.location || range.length > 4096)
        [NSException raise:NSInvalidArgumentException format:@"Argument resource range overflows"];
    for (NSUInteger n = 0; n < range.length; ++n)
        (void)[self rdna4Destination:range.location + n type:type];
}
- (void)setTextures:(const id<MTLTexture> __unsafe_unretained *)textures withRange:(NSRange)range {
    if (!textures && range.length)
        [NSException raise:NSInvalidArgumentException format:@"Argument texture array is null"];
    [self rdna4ValidateRange:range type:MTLDataTypeTexture];
    for (NSUInteger n = 0; n < range.length; ++n) (void)[self rdna4TextureToken:textures[n]];
    for (NSUInteger n = 0; n < range.length; ++n) [self setTexture:textures[n] atIndex:range.location + n];
}
- (void)setSamplerStates:(const id<MTLSamplerState> __unsafe_unretained *)samplers withRange:(NSRange)range {
    if (!samplers && range.length)
        [NSException raise:NSInvalidArgumentException format:@"Argument sampler array is null"];
    [self rdna4ValidateRange:range type:MTLDataTypeSampler];
    for (NSUInteger n = 0; n < range.length; ++n) (void)[self rdna4SamplerToken:samplers[n]];
    for (NSUInteger n = 0; n < range.length; ++n) [self setSamplerState:samplers[n] atIndex:range.location + n];
}
- (uint64_t)rdna4IndirectCommandBufferAddress:(id<MTLIndirectCommandBuffer>)icb {
    uint64_t address = 0;
    if (icb) {
        Class icbClass = objc_getClass("RDNA4IndirectCommandBuffer");
        if (!icbClass || ![icb isKindOfClass:icbClass] || icb.device != _device)
            [NSException raise:NSInvalidArgumentException format:@"Argument indirect command buffer has an invalid class or device"];
        address = [(id<RDNA4ICBInternal>)icb rdna4EnsureGPUBackingAddress];
        if (!address)
            [NSException raise:NSInvalidArgumentException format:@"Argument indirect command buffer backing GPU address is unavailable"];
    }
    return address;
}
- (void)setIndirectCommandBuffer:(id<MTLIndirectCommandBuffer>)indirectCommandBuffer atIndex:(NSUInteger)index {
    (void)[self rdna4Destination:index type:MTLDataTypeIndirectCommandBuffer];
    uint64_t address = [self rdna4IndirectCommandBufferAddress:indirectCommandBuffer];
    rdna4_argument_sampler_store(_buffer, _offset + [_layout field:index].offset, address, nil);
    if (indirectCommandBuffer) rdna4_icb_arg_note(_buffer, indirectCommandBuffer, nil, nil);
}
- (void)setIndirectCommandBuffers:(const id<MTLIndirectCommandBuffer> __unsafe_unretained *)buffers withRange:(NSRange)range {
    if (!buffers && range.length)
        [NSException raise:NSInvalidArgumentException format:@"Argument indirect command buffer array is null"];
    [self rdna4ValidateRange:range type:MTLDataTypeIndirectCommandBuffer];
    for (NSUInteger n = 0; n < range.length; ++n) (void)[self rdna4IndirectCommandBufferAddress:buffers[n]];
    for (NSUInteger n = 0; n < range.length; ++n) [self setIndirectCommandBuffer:buffers[n] atIndex:range.location + n];
}
- (uint64_t)rdna4RenderPipelineToken:(id<MTLRenderPipelineState>)pipeline {
    uint64_t token = 0;
    if (pipeline) {
        if (![pipeline isKindOfClass:RDNA4RenderPipelineState.class] || pipeline.device != _device ||
            ![pipeline supportIndirectCommandBuffers])
            [NSException raise:NSInvalidArgumentException format:@"Argument render pipeline state is invalid or does not support indirect command buffers"];
        token = [(RDNA4RenderPipelineState *)pipeline rdna4ICBToken];
        if (!token)
            [NSException raise:NSInvalidArgumentException format:@"Argument render pipeline state has no ICB token"];
    }
    return token;
}
- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)pipeline atIndex:(NSUInteger)index {
    (void)[self rdna4Destination:index type:MTLDataTypeRenderPipeline];
    uint64_t token = [self rdna4RenderPipelineToken:pipeline];
    rdna4_argument_sampler_store(_buffer, _offset + [_layout field:index].offset, token, nil);
    if (pipeline) rdna4_icb_arg_note(_buffer, nil, pipeline, nil);
}
- (void)setRenderPipelineStates:(const id<MTLRenderPipelineState> __unsafe_unretained *)pipelines withRange:(NSRange)range {
    if (!pipelines && range.length)
        [NSException raise:NSInvalidArgumentException format:@"Argument render pipeline array is null"];
    [self rdna4ValidateRange:range type:MTLDataTypeRenderPipeline];
    for (NSUInteger n = 0; n < range.length; ++n) (void)[self rdna4RenderPipelineToken:pipelines[n]];
    for (NSUInteger n = 0; n < range.length; ++n) [self setRenderPipelineState:pipelines[n] atIndex:range.location + n];
}
- (uint64_t)rdna4ComputePipelineToken:(id<MTLComputePipelineState>)pipeline {
    uint64_t token = 0;
    if (pipeline) {
        if (![pipeline isKindOfClass:RDNA4ComputePipelineState.class] || pipeline.device != _device ||
            ![pipeline supportIndirectCommandBuffers])
            [NSException raise:NSInvalidArgumentException format:@"Argument compute pipeline state is invalid or does not support indirect command buffers"];
        token = [(RDNA4ComputePipelineState *)pipeline rdna4ICBToken];
        if (!token)
            [NSException raise:NSInvalidArgumentException format:@"Argument compute pipeline state has no ICB token"];
    }
    return token;
}
- (void)setComputePipelineState:(id<MTLComputePipelineState>)pipeline atIndex:(NSUInteger)index {
    (void)[self rdna4Destination:index type:MTLDataTypeComputePipeline];
    uint64_t token = [self rdna4ComputePipelineToken:pipeline];
    rdna4_argument_sampler_store(_buffer, _offset + [_layout field:index].offset, token, nil);
    if (pipeline) rdna4_icb_arg_note(_buffer, nil, pipeline, nil);
}
- (void)setComputePipelineStates:(const id<MTLComputePipelineState> __unsafe_unretained *)pipelines withRange:(NSRange)range {
    if (!pipelines && range.length)
        [NSException raise:NSInvalidArgumentException format:@"Argument compute pipeline array is null"];
    [self rdna4ValidateRange:range type:MTLDataTypeComputePipeline];
    for (NSUInteger n = 0; n < range.length; ++n) (void)[self rdna4ComputePipelineToken:pipelines[n]];
    for (NSUInteger n = 0; n < range.length; ++n) [self setComputePipelineState:pipelines[n] atIndex:range.location + n];
}
- (void)setAccelerationStructure:(id<MTLAccelerationStructure>)accelerationStructure atIndex:(NSUInteger)index {
    RDNA4ArgumentField *field = [_layout field:index];
    if (!field || (field.type != MTLDataTypeInstanceAccelerationStructure && field.type != MTLDataTypePrimitiveAccelerationStructure))
        [NSException raise:NSInvalidArgumentException format:@"Argument ID %lu is not an acceleration structure destination", (unsigned long)index];
    (void)[self rdna4Destination:index type:field.type];
    uint64_t address = rdna4_rt_arg_resolve_as_address(_device, accelerationStructure, field.type,
                                                       _offset + field.offset, field.size, [_buffer length]);
    rdna4_argument_sampler_store(_buffer, _offset + field.offset, address, nil);
    if (accelerationStructure) rdna4_rt_arg_note(_buffer, accelerationStructure, nil, nil);
}
- (void)setIntersectionFunctionTable:(id<MTLIntersectionFunctionTable>)intersectionFunctionTable atIndex:(NSUInteger)index {
    (void)[self rdna4Destination:index type:MTLDataTypeIntersectionFunctionTable];
    RDNA4ArgumentField *field = [_layout field:index];
    uint64_t address = rdna4_rt_arg_resolve_ift_address(_device, intersectionFunctionTable, field.type,
                                                        _offset + field.offset, field.size, [_buffer length]);
    rdna4_argument_sampler_store(_buffer, _offset + field.offset, address, nil);
    if (intersectionFunctionTable) rdna4_rt_arg_note(_buffer, nil, (RDNA4IntersectionFunctionTable *)intersectionFunctionTable, nil);
}
- (void)setIntersectionFunctionTables:(const id<MTLIntersectionFunctionTable> __unsafe_unretained *)intersectionFunctionTables withRange:(NSRange)range {
    if (!intersectionFunctionTables && range.length)
        [NSException raise:NSInvalidArgumentException format:@"Argument intersection function table array is null"];
    [self rdna4ValidateRange:range type:MTLDataTypeIntersectionFunctionTable];
    for (NSUInteger n = 0; n < range.length; ++n) {
        RDNA4ArgumentField *field = [_layout field:range.location + n];
        (void)rdna4_rt_arg_resolve_ift_address(_device, intersectionFunctionTables[n], field.type,
                                               _offset + field.offset, field.size, [_buffer length]);
    }
    for (NSUInteger n = 0; n < range.length; ++n)
        [self setIntersectionFunctionTable:intersectionFunctionTables[n] atIndex:range.location + n];
}
- (void *)constantDataAtIndex:(NSUInteger)index { return [self rdna4Destination:index type:MTLDataTypeNone]; }
- (id)newArgumentEncoderForBufferAtIndex:(NSUInteger)index {
    RDNA4ArgumentLayout *nested = [_layout field:index].nested;
    return nested ? [[RDNA4ArgumentEncoder alloc] initWithLayout:nested device:_device] : nil;
}
@end

static BOOL rdna4_argument_reflection_type(MTLDataType type, MTLStructType *structure,
        MTLArrayType *array, MTLPointerType *pointer, RDNA4ArgumentLayout *layout,
        NSUInteger index, unsigned depth) {
    if (depth > 16) return NO;
    if (type == MTLDataTypeStruct) {
        for (MTLStructMember *member in structure.members) {
            if (member.argumentIndex > NSUIntegerMax - index ||
                !rdna4_argument_reflection_type(member.dataType, member.structType, member.arrayType,
                    member.pointerType, layout, index + member.argumentIndex, depth + 1)) return NO;
        }
    } else if (type == MTLDataTypeArray) {
        if (!array) return NO;
        if (array.elementType != MTLDataTypeStruct && array.elementType != MTLDataTypeArray &&
            array.elementType != MTLDataTypePointer) return YES;
        if (array.arrayLength > 4096 || (array.arrayLength && !array.argumentIndexStride)) return NO;
        for (NSUInteger n = 0; n < array.arrayLength; ++n) {
            if (n > (NSUIntegerMax - index) / array.argumentIndexStride ||
                !rdna4_argument_reflection_type(array.elementType, array.elementStructType, array.elementArrayType,
                    array.elementPointerType, layout, index + n * array.argumentIndexStride, depth + 1)) return NO;
        }
    } else if (type == MTLDataTypePointer && pointer.elementIsArgumentBuffer) {
        RDNA4ArgumentLayout *nested = [layout field:index].nested;
        SEL setter = sel_registerName("setAlignment:dataSize:");
        if (!nested || pointer.elementType != MTLDataTypeStruct || ![pointer respondsToSelector:setter]) return NO;
        // Indirect pointers use the encoded layout, not the top-level constant descriptor's minimum alignment.
        ((void (*)(id, SEL, NSUInteger, NSUInteger))objc_msgSend)(pointer, setter, nested.alignment, nested.encodedLength);
        return rdna4_argument_reflection_type(MTLDataTypeStruct, pointer.elementStructType, nil, nil,
            nested, 0, depth + 1);
    }
    return YES;
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
GARNET_INTERNAL BOOL rdna4_argument_reflection(id<MTLFunction> function, NSArray<MTLArgument *> *arguments) {
    for (MTLArgument *argument in arguments) {
        if (argument.type != MTLArgumentTypeBuffer || !argument.bufferPointerType.elementIsArgumentBuffer) continue;
        id encoder = [function newArgumentEncoderWithBufferIndex:argument.index];
        if (![encoder isKindOfClass:RDNA4ArgumentEncoder.class] ||
            !rdna4_argument_reflection_type(MTLDataTypeStruct, argument.bufferStructType, nil, nil,
                [encoder rdna4Layout], 0, 0)) return NO;
    }
    return YES;
}
#pragma clang diagnostic pop

#endif
#endif // !defined(RDNA4_ARGUMENT_SUMMARY_RUNTIME_TEST) && !defined(GARNET_ARGUMENT_SUMMARY_OWNERS_ONLY)
#if defined(RDNA4_ARGUMENT_SUMMARY_RUNTIME_TEST) || (!defined(GARNET_ARGUMENT_SUMMARY_OWNERS_ONLY))
/*
 * Runtime foundation for the Private live-token summary shader.
 *
 * Include this file from GarnetDevice.m after RDNA4Buffer is declared. The
 * planning/completion half is pure C so test-argument-summary-runtime.m can
 * exercise it without Vulkan, Metal, Foundation, or a GPU. The bridge half
 * records into an existing VkCommandBuffer and deliberately creates no queue,
 * submit, wait, or owner-retirement path.
 *
 * GarnetDevice.m integration point, inside rdna4EndCurrentSegment:
 *
 *   rdna4_full_barrier(_curSegment->vk);
 *   RDNA4ArgumentSummaryAttempt *attempt = nil;
 *   if (!rdna4_argument_summary_encode(task, _curSegment->vk, &attempt, err))
 *       return NO;                       // abort/fail this segment before end
 *   [segment retain the attempt through its fence completion];
 *   vkEndCommandBuffer(_curSegment->vk);
 *
 * After that existing fence completes:
 *
 *   status = rdna4_argument_summary_attempt_complete(attempt, fence_ok,
 *                                                     live_generation);
 *   if (status == RDNA4_ARGUMENT_SUMMARY_COMPLETE)
 *       status = rdna4_argument_summary_attempt_publish(
 *           attempt, live_generation, &publication);
 *
 * Only READY authorizes the lifetime owner to replace owners. The attempt
 * holds the task and every scratch resource; releasing it before the segment
 * fence is invalid. Device teardown must call
 * rdna4_argument_summary_pipeline_destroy() before destroying RDNA4R.dev.
 */

#ifndef RDNA4_ARGUMENT_SUMMARY_RUNTIME_INC
#define RDNA4_ARGUMENT_SUMMARY_RUNTIME_INC

#include "garnet_argument_summary.h"

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define RDNA4_ARGUMENT_SUMMARY_DICTIONARY_ALIGN 32u
#define RDNA4_ARGUMENT_SUMMARY_MIN_COPY_ALIGN 4u

typedef struct {
    uint32_t max_storage_buffer_range;
    uint32_t min_storage_buffer_offset_alignment;
    uint32_t max_compute_workgroup_count_x;
    uint32_t max_compute_workgroup_invocations;
    uint32_t max_compute_workgroup_size_x;
    /* Zero selects the corresponding RDNA4_ARGUMENT_SUMMARY_DEFAULT_* value. */
    uint32_t requested_chunk_bytes;
    uint32_t requested_dictionary_page;
} rdna4_argument_summary_runtime_limits;

typedef struct {
    uint64_t generation;
    uint64_t source_size;
    uint64_t chunk_count;
    uint64_t job_count;
    uint32_t chunk_bytes;
    uint32_t candidate_count;
    uint32_t dictionary_page_size;
    uint32_t dictionary_page_count;
    uint32_t output_word_count;
    uint32_t source_binding_bytes;
    uint32_t dictionary_binding_bytes;
    uint32_t output_page_bytes;
} rdna4_argument_summary_runtime_plan;

typedef struct {
    uint64_t source_offset;
    uint64_t dictionary_offset;
    uint32_t page_index;
    uint32_t dictionary_begin;
    uint32_t dictionary_count;
    uint32_t dictionary_bytes;
    uint32_t output_word_base;
    uint32_t output_word_count;
} rdna4_argument_summary_runtime_page;

typedef struct {
    uint64_t job_index;
    uint64_t chunk_index;
    uint64_t source_offset;
    uint32_t source_count;
    uint32_t source_overlap;
    uint32_t source_end;
    uint32_t source_copy_bytes;
    uint32_t source_binding_bytes;
    rdna4_argument_summary_runtime_page page;
    rdna4_argument_summary_push push;
} rdna4_argument_summary_runtime_job;

typedef struct {
    uint32_t source_staging_bytes;
    uint32_t dictionary_staging_bytes;
    uint32_t output_staging_bytes;
} rdna4_argument_summary_scratch;

typedef struct {
    uint32_t source_begin;
    uint32_t source_count;
    uint32_t group_count;
} rdna4_argument_summary_dispatch;

typedef struct {
    uint32_t source_count;
    uint32_t max_groups;
    uint32_t next_begin;
} rdna4_argument_summary_dispatch_cursor;

static inline bool rdna4_argument_summary_runtime_pow2(uint32_t value) {
    return value && !(value & (value - 1u));
}

static inline uint64_t rdna4_argument_summary_runtime_align_down(
    uint64_t value, uint64_t alignment) {
    return value & ~(alignment - 1u);
}

static inline bool rdna4_argument_summary_runtime_add_u64(
    uint64_t a, uint64_t b, uint64_t *out) {
    if (a > UINT64_MAX - b) return false;
    if (out) *out = a + b;
    return true;
}

static inline bool rdna4_argument_summary_runtime_mul_u64(
    uint64_t a, uint64_t b, uint64_t *out) {
    if (a && b > UINT64_MAX / a) return false;
    if (out) *out = a * b;
    return true;
}

static inline bool rdna4_argument_summary_runtime_round_words(
    uint64_t bytes, uint32_t *out) {
    if (bytes > UINT32_MAX - 3u) return false;
    if (out) *out = (uint32_t)((bytes + 3u) & ~UINT64_C(3));
    return true;
}

/*
 * Runtime job order is dictionary page outer, source chunk inner. That order
 * lets one attempt reuse one dictionary-page scratch and one output-page
 * scratch across every chunk and every page without per-dispatch allocations.
 */
static inline rdna4_argument_summary_status
rdna4_argument_summary_runtime_plan_init(
    rdna4_argument_summary_runtime_plan *plan,
    const rdna4_argument_summary_runtime_limits *limits,
    uint64_t source_size, uint32_t candidate_count, uint64_t generation) {
    if (!plan || !limits || !limits->max_storage_buffer_range ||
        !limits->max_compute_workgroup_count_x ||
        limits->max_compute_workgroup_invocations <
            RDNA4_ARGUMENT_SUMMARY_WORKGROUP_SIZE ||
        limits->max_compute_workgroup_size_x <
            RDNA4_ARGUMENT_SUMMARY_WORKGROUP_SIZE)
        return RDNA4_ARGUMENT_SUMMARY_INVALID;

    uint32_t alignment = limits->min_storage_buffer_offset_alignment;
    if (!alignment) alignment = RDNA4_ARGUMENT_SUMMARY_MIN_COPY_ALIGN;
    if (!rdna4_argument_summary_runtime_pow2(alignment))
        return RDNA4_ARGUMENT_SUMMARY_INVALID;
    if (alignment < RDNA4_ARGUMENT_SUMMARY_MIN_COPY_ALIGN)
        alignment = RDNA4_ARGUMENT_SUMMARY_MIN_COPY_ALIGN;

    uint64_t max_payload = limits->max_storage_buffer_range >
            RDNA4_ARGUMENT_SUMMARY_OVERLAP_BYTES
        ? (uint64_t)limits->max_storage_buffer_range -
              RDNA4_ARGUMENT_SUMMARY_OVERLAP_BYTES
        : 0;
    uint32_t requested_chunk = limits->requested_chunk_bytes
        ? limits->requested_chunk_bytes
        : RDNA4_ARGUMENT_SUMMARY_DEFAULT_CHUNK_BYTES;
    uint64_t chunk_bytes = requested_chunk < max_payload
        ? requested_chunk : max_payload;
    chunk_bytes = rdna4_argument_summary_runtime_align_down(
        chunk_bytes, alignment);
    if (!chunk_bytes || chunk_bytes > UINT32_MAX)
        return RDNA4_ARGUMENT_SUMMARY_INVALID;

    uint32_t source_binding_bytes = 0;
    if (!rdna4_argument_summary_runtime_round_words(
            chunk_bytes + RDNA4_ARGUMENT_SUMMARY_OVERLAP_BYTES,
            &source_binding_bytes) ||
        source_binding_bytes > limits->max_storage_buffer_range)
        return RDNA4_ARGUMENT_SUMMARY_OVERFLOW;

    uint64_t page_capacity =
        (uint64_t)limits->max_storage_buffer_range /
        sizeof(rdna4_argument_summary_candidate);
    page_capacity = rdna4_argument_summary_runtime_align_down(
        page_capacity, RDNA4_ARGUMENT_SUMMARY_DICTIONARY_ALIGN);
    if (page_capacity > UINT32_MAX)
        page_capacity = UINT32_MAX;
    uint32_t requested_page = limits->requested_dictionary_page
        ? limits->requested_dictionary_page
        : RDNA4_ARGUMENT_SUMMARY_DEFAULT_DICTIONARY_PAGE;
    uint32_t dictionary_page_size = requested_page < page_capacity
        ? requested_page : (uint32_t)page_capacity;
    dictionary_page_size = (uint32_t)rdna4_argument_summary_runtime_align_down(
        dictionary_page_size, RDNA4_ARGUMENT_SUMMARY_DICTIONARY_ALIGN);
    if (!dictionary_page_size)
        return RDNA4_ARGUMENT_SUMMARY_INVALID;

    uint64_t dictionary_binding_bytes =
        (uint64_t)dictionary_page_size *
        sizeof(rdna4_argument_summary_candidate);
    uint32_t output_page_bytes =
        dictionary_page_size / RDNA4_ARGUMENT_SUMMARY_DICTIONARY_ALIGN * 4u;
    if (dictionary_binding_bytes > limits->max_storage_buffer_range ||
        output_page_bytes > limits->max_storage_buffer_range)
        return RDNA4_ARGUMENT_SUMMARY_OVERFLOW;

    uint64_t chunk_count = source_size
        ? 1u + (source_size - 1u) / chunk_bytes
        : 0u;
    uint64_t page_count64 = candidate_count
        ? 1u + ((uint64_t)candidate_count - 1u) / dictionary_page_size
        : 0u;
    if (page_count64 > UINT32_MAX)
        return RDNA4_ARGUMENT_SUMMARY_OVERFLOW;
    uint64_t job_count = 0;
    if (chunk_count && page_count64 &&
        !rdna4_argument_summary_runtime_mul_u64(
            chunk_count, page_count64, &job_count))
        return RDNA4_ARGUMENT_SUMMARY_OVERFLOW;

    *plan = (rdna4_argument_summary_runtime_plan){
        .generation = generation,
        .source_size = source_size,
        .chunk_count = chunk_count,
        .job_count = job_count,
        .chunk_bytes = (uint32_t)chunk_bytes,
        .candidate_count = candidate_count,
        .dictionary_page_size = dictionary_page_size,
        .dictionary_page_count = (uint32_t)page_count64,
        .output_word_count =
            rdna4_argument_summary_output_words(candidate_count),
        .source_binding_bytes = source_binding_bytes,
        .dictionary_binding_bytes = (uint32_t)dictionary_binding_bytes,
        .output_page_bytes = output_page_bytes,
    };
    return RDNA4_ARGUMENT_SUMMARY_OK;
}

static inline rdna4_argument_summary_status
rdna4_argument_summary_runtime_page_get(
    const rdna4_argument_summary_runtime_plan *plan, uint32_t page_index,
    rdna4_argument_summary_runtime_page *page) {
    if (!plan || !page || page_index >= plan->dictionary_page_count)
        return RDNA4_ARGUMENT_SUMMARY_INVALID;
    uint64_t begin =
        (uint64_t)page_index * plan->dictionary_page_size;
    if (begin >= plan->candidate_count)
        return RDNA4_ARGUMENT_SUMMARY_OVERFLOW;
    uint64_t remaining = (uint64_t)plan->candidate_count - begin;
    uint64_t count = remaining < plan->dictionary_page_size
        ? remaining : plan->dictionary_page_size;
    uint64_t byte_offset = 0;
    if (!rdna4_argument_summary_runtime_mul_u64(
            begin, sizeof(rdna4_argument_summary_candidate), &byte_offset) ||
        count > UINT32_MAX ||
        count * sizeof(rdna4_argument_summary_candidate) > UINT32_MAX)
        return RDNA4_ARGUMENT_SUMMARY_OVERFLOW;

    *page = (rdna4_argument_summary_runtime_page){
        .dictionary_offset = byte_offset,
        .page_index = page_index,
        .dictionary_begin = (uint32_t)begin,
        .dictionary_count = (uint32_t)count,
        .dictionary_bytes =
            (uint32_t)(count * sizeof(rdna4_argument_summary_candidate)),
        .output_word_base =
            (uint32_t)(begin / RDNA4_ARGUMENT_SUMMARY_DICTIONARY_ALIGN),
        .output_word_count =
            (uint32_t)((count + 31u) / RDNA4_ARGUMENT_SUMMARY_DICTIONARY_ALIGN),
    };
    return RDNA4_ARGUMENT_SUMMARY_OK;
}

static inline rdna4_argument_summary_status
rdna4_argument_summary_runtime_job_get(
    const rdna4_argument_summary_runtime_plan *plan, uint64_t job_index,
    rdna4_argument_summary_runtime_job *job) {
    if (!plan || !job || !plan->chunk_count ||
        !plan->dictionary_page_count || job_index >= plan->job_count)
        return RDNA4_ARGUMENT_SUMMARY_INVALID;

    uint64_t page_index = job_index / plan->chunk_count;
    uint64_t chunk_index = job_index % plan->chunk_count;
    if (page_index > UINT32_MAX)
        return RDNA4_ARGUMENT_SUMMARY_OVERFLOW;
    rdna4_argument_summary_runtime_page page;
    rdna4_argument_summary_status status =
        rdna4_argument_summary_runtime_page_get(
            plan, (uint32_t)page_index, &page);
    if (status != RDNA4_ARGUMENT_SUMMARY_OK)
        return status;

    uint64_t source_offset = 0;
    if (!rdna4_argument_summary_runtime_mul_u64(
            chunk_index, plan->chunk_bytes, &source_offset) ||
        source_offset >= plan->source_size)
        return RDNA4_ARGUMENT_SUMMARY_OVERFLOW;
    uint64_t remaining = plan->source_size - source_offset;
    uint64_t source_count = remaining < plan->chunk_bytes
        ? remaining : plan->chunk_bytes;
    uint64_t after = remaining - source_count;
    uint64_t overlap = after < RDNA4_ARGUMENT_SUMMARY_OVERLAP_BYTES
        ? after : RDNA4_ARGUMENT_SUMMARY_OVERLAP_BYTES;
    uint64_t source_end = source_count + overlap;
    uint32_t binding_bytes = 0;
    if (source_count > UINT32_MAX || source_end > UINT32_MAX ||
        overlap > UINT32_MAX ||
        !rdna4_argument_summary_runtime_round_words(
            source_end, &binding_bytes))
        return RDNA4_ARGUMENT_SUMMARY_OVERFLOW;

    *job = (rdna4_argument_summary_runtime_job){
        .job_index = job_index,
        .chunk_index = chunk_index,
        .source_offset = source_offset,
        .source_count = (uint32_t)source_count,
        .source_overlap = (uint32_t)overlap,
        .source_end = (uint32_t)source_end,
        .source_copy_bytes = (uint32_t)source_end,
        .source_binding_bytes = binding_bytes,
        .page = page,
        .push = {
            .source_begin = 0,
            .source_count = (uint32_t)source_count,
            .source_end = (uint32_t)source_end,
            .dictionary_begin = page.dictionary_begin,
            .dictionary_count = page.dictionary_count,
            .dictionary_page = page.page_index,
            .dictionary_page_count = plan->dictionary_page_count,
            .output_word_base = page.output_word_base,
        },
    };
    return RDNA4_ARGUMENT_SUMMARY_OK;
}

typedef struct {
    uint64_t source_offset;
    uint32_t source_binding_bytes;
    uint32_t copy_bytes;
} rdna4_argument_summary_source_copy;

static inline rdna4_argument_summary_status
rdna4_argument_summary_runtime_source_copy(
    const rdna4_argument_summary_runtime_job *job,
    uint64_t source_range_offset, uint64_t source_range_size,
    rdna4_argument_summary_source_copy *copy) {
    if (!job || !copy || source_range_offset > UINT64_MAX - job->source_offset)
        return RDNA4_ARGUMENT_SUMMARY_INVALID;
    uint64_t begin = source_range_offset + job->source_offset;
    uint64_t range_end = 0;
    uint64_t copy_end = 0;
    if (!rdna4_argument_summary_runtime_add_u64(
            source_range_offset, source_range_size, &range_end) ||
        !rdna4_argument_summary_runtime_add_u64(
            begin, job->source_copy_bytes, &copy_end) ||
        begin > range_end || copy_end > range_end)
        return RDNA4_ARGUMENT_SUMMARY_OVERFLOW;
    *copy = (rdna4_argument_summary_source_copy){
        .source_offset = begin,
        .source_binding_bytes = job->source_binding_bytes,
        .copy_bytes = job->source_copy_bytes,
    };
    return RDNA4_ARGUMENT_SUMMARY_OK;
}

static inline rdna4_argument_summary_scratch
rdna4_argument_summary_runtime_scratch(
    const rdna4_argument_summary_runtime_plan *plan) {
    if (!plan) return (rdna4_argument_summary_scratch){0};
    return (rdna4_argument_summary_scratch){
        .source_staging_bytes =
            plan->job_count ? plan->source_binding_bytes : 0,
        .dictionary_staging_bytes =
            plan->job_count ? plan->dictionary_binding_bytes : 0,
        .output_staging_bytes =
            plan->dictionary_page_count ? plan->output_page_bytes : 0,
    };
}

static inline rdna4_argument_summary_status
rdna4_argument_summary_runtime_dispatch_init(
    rdna4_argument_summary_dispatch_cursor *cursor, uint32_t source_count,
    uint32_t max_groups) {
    if (!cursor || !max_groups) return RDNA4_ARGUMENT_SUMMARY_INVALID;
    *cursor = (rdna4_argument_summary_dispatch_cursor){
        .source_count = source_count,
        .max_groups = max_groups,
    };
    return RDNA4_ARGUMENT_SUMMARY_OK;
}

static inline bool rdna4_argument_summary_runtime_dispatch_next(
    rdna4_argument_summary_dispatch_cursor *cursor,
    rdna4_argument_summary_dispatch *dispatch) {
    if (!cursor || !dispatch || cursor->next_begin >= cursor->source_count)
        return false;
    uint64_t group_bytes =
        (uint64_t)cursor->max_groups *
        RDNA4_ARGUMENT_SUMMARY_WORKGROUP_SIZE;
    uint64_t remaining = cursor->source_count - cursor->next_begin;
    uint64_t count = remaining < group_bytes ? remaining : group_bytes;
    if (count > UINT32_MAX)
        count = UINT32_MAX;
    *dispatch = (rdna4_argument_summary_dispatch){
        .source_begin = cursor->next_begin,
        .source_count = (uint32_t)count,
        .group_count =
            (uint32_t)((count + RDNA4_ARGUMENT_SUMMARY_WORKGROUP_SIZE - 1u) /
                       RDNA4_ARGUMENT_SUMMARY_WORKGROUP_SIZE),
    };
    cursor->next_begin += (uint32_t)count;
    return true;
}

/*
 * Pipeline creation is represented as callbacks so the rollback path can be
 * exercised without Vulkan. A retry after any failed stage starts from zero.
 */
typedef struct {
    void *context;
    int32_t (*create_shader)(void *context, uintptr_t *handle);
    int32_t (*create_descriptor_layout)(void *context, uintptr_t *handle);
    int32_t (*create_pipeline_layout)(void *context, uintptr_t shader,
                                      uintptr_t descriptor_layout,
                                      uintptr_t *handle);
    int32_t (*create_pipeline)(void *context, uintptr_t shader,
                               uintptr_t pipeline_layout, uintptr_t *handle);
    void (*destroy_shader)(void *context, uintptr_t handle);
    void (*destroy_descriptor_layout)(void *context, uintptr_t handle);
    void (*destroy_pipeline_layout)(void *context, uintptr_t handle);
    void (*destroy_pipeline)(void *context, uintptr_t handle);
} rdna4_argument_summary_pipeline_ops;

typedef struct {
    uintptr_t device_key;
    uintptr_t shader;
    uintptr_t descriptor_layout;
    uintptr_t pipeline_layout;
    uintptr_t pipeline;
    bool ready;
} rdna4_argument_summary_pipeline_state;

static inline void rdna4_argument_summary_pipeline_release(
    rdna4_argument_summary_pipeline_state *state,
    const rdna4_argument_summary_pipeline_ops *ops) {
    if (!state) return;
    if (ops) {
        if (state->pipeline && ops->destroy_pipeline)
            ops->destroy_pipeline(ops->context, state->pipeline);
        if (state->pipeline_layout && ops->destroy_pipeline_layout)
            ops->destroy_pipeline_layout(
                ops->context, state->pipeline_layout);
        if (state->descriptor_layout && ops->destroy_descriptor_layout)
            ops->destroy_descriptor_layout(
                ops->context, state->descriptor_layout);
        if (state->shader && ops->destroy_shader)
            ops->destroy_shader(ops->context, state->shader);
    }
    memset(state, 0, sizeof(*state));
}

static inline bool rdna4_argument_summary_pipeline_ops_valid(
    const rdna4_argument_summary_pipeline_ops *ops) {
    return ops && ops->create_shader && ops->create_descriptor_layout &&
        ops->create_pipeline_layout && ops->create_pipeline &&
        ops->destroy_shader && ops->destroy_descriptor_layout &&
        ops->destroy_pipeline_layout && ops->destroy_pipeline;
}

static inline rdna4_argument_summary_status
rdna4_argument_summary_pipeline_ensure(
    rdna4_argument_summary_pipeline_state *state,
    const rdna4_argument_summary_pipeline_ops *ops, uintptr_t device_key) {
    if (!state || !rdna4_argument_summary_pipeline_ops_valid(ops) ||
        !device_key)
        return RDNA4_ARGUMENT_SUMMARY_INVALID;
    if (state->ready) {
        return state->device_key == device_key
            ? RDNA4_ARGUMENT_SUMMARY_OK
            : RDNA4_ARGUMENT_SUMMARY_FAILED;
    }
    if (state->shader || state->descriptor_layout ||
        state->pipeline_layout || state->pipeline)
        rdna4_argument_summary_pipeline_release(state, ops);

    int32_t result = ops->create_shader(ops->context, &state->shader);
    if (result != 0 || !state->shader) goto fail;
    result = ops->create_descriptor_layout(
        ops->context, &state->descriptor_layout);
    if (result != 0 || !state->descriptor_layout) goto fail;
    result = ops->create_pipeline_layout(
        ops->context, state->shader, state->descriptor_layout,
        &state->pipeline_layout);
    if (result != 0 || !state->pipeline_layout) goto fail;
    result = ops->create_pipeline(
        ops->context, state->shader, state->pipeline_layout,
        &state->pipeline);
    if (result != 0 || !state->pipeline) goto fail;

    state->device_key = device_key;
    state->ready = true;
    return RDNA4_ARGUMENT_SUMMARY_OK;

fail:
    rdna4_argument_summary_pipeline_release(state, ops);
    return RDNA4_ARGUMENT_SUMMARY_FAILED;
}

typedef enum {
    RDNA4_ARGUMENT_SUMMARY_COMPLETION_ENCODING = 0,
    RDNA4_ARGUMENT_SUMMARY_COMPLETION_ENCODED,
    RDNA4_ARGUMENT_SUMMARY_COMPLETION_COMPLETE,
    RDNA4_ARGUMENT_SUMMARY_COMPLETION_FAILED,
    RDNA4_ARGUMENT_SUMMARY_COMPLETION_OBSOLETE,
} rdna4_argument_summary_completion_state;

typedef struct {
    uint64_t generation;
    uint64_t job_count;
    uint64_t encoded_jobs;
    rdna4_argument_summary_completion_state state;
} rdna4_argument_summary_completion;

static inline void rdna4_argument_summary_completion_init(
    rdna4_argument_summary_completion *completion,
    const rdna4_argument_summary_runtime_plan *plan) {
    if (!completion) return;
    *completion = (rdna4_argument_summary_completion){
        .generation = plan ? plan->generation : 0,
        .job_count = plan ? plan->job_count : 0,
        .state = RDNA4_ARGUMENT_SUMMARY_COMPLETION_ENCODING,
    };
}

static inline rdna4_argument_summary_status
rdna4_argument_summary_completion_job_encoded(
    rdna4_argument_summary_completion *completion) {
    if (!completion ||
        completion->state != RDNA4_ARGUMENT_SUMMARY_COMPLETION_ENCODING)
        return RDNA4_ARGUMENT_SUMMARY_INVALID;
    if (completion->encoded_jobs >= completion->job_count)
        return RDNA4_ARGUMENT_SUMMARY_OVERFLOW;
    ++completion->encoded_jobs;
    return RDNA4_ARGUMENT_SUMMARY_OK;
}

static inline rdna4_argument_summary_status
rdna4_argument_summary_completion_finish_encode(
    rdna4_argument_summary_completion *completion, bool success) {
    if (!completion ||
        completion->state != RDNA4_ARGUMENT_SUMMARY_COMPLETION_ENCODING)
        return RDNA4_ARGUMENT_SUMMARY_INVALID;
    if (!success) {
        completion->state = RDNA4_ARGUMENT_SUMMARY_COMPLETION_FAILED;
        return RDNA4_ARGUMENT_SUMMARY_FAILED;
    }
    if (completion->encoded_jobs != completion->job_count) {
        completion->state = RDNA4_ARGUMENT_SUMMARY_COMPLETION_FAILED;
        return RDNA4_ARGUMENT_SUMMARY_INCOMPLETE;
    }
    completion->state = RDNA4_ARGUMENT_SUMMARY_COMPLETION_ENCODED;
    return RDNA4_ARGUMENT_SUMMARY_OK;
}

static inline rdna4_argument_summary_status
rdna4_argument_summary_completion_fence(
    rdna4_argument_summary_completion *completion, bool fence_succeeded,
    uint64_t current_generation) {
    if (!completion) return RDNA4_ARGUMENT_SUMMARY_INVALID;
    if (completion->state == RDNA4_ARGUMENT_SUMMARY_COMPLETION_FAILED)
        return RDNA4_ARGUMENT_SUMMARY_FAILED;
    if (completion->state == RDNA4_ARGUMENT_SUMMARY_COMPLETION_OBSOLETE)
        return RDNA4_ARGUMENT_SUMMARY_OBSOLETE;
    if (completion->state == RDNA4_ARGUMENT_SUMMARY_COMPLETION_COMPLETE) {
        if (current_generation != completion->generation) {
            completion->state = RDNA4_ARGUMENT_SUMMARY_COMPLETION_OBSOLETE;
            return RDNA4_ARGUMENT_SUMMARY_STALE;
        }
        return RDNA4_ARGUMENT_SUMMARY_COMPLETE;
    }
    if (completion->state != RDNA4_ARGUMENT_SUMMARY_COMPLETION_ENCODED)
        return RDNA4_ARGUMENT_SUMMARY_INCOMPLETE;
    if (!fence_succeeded) {
        completion->state = RDNA4_ARGUMENT_SUMMARY_COMPLETION_FAILED;
        return RDNA4_ARGUMENT_SUMMARY_FAILED;
    }
    if (current_generation != completion->generation) {
        completion->state = RDNA4_ARGUMENT_SUMMARY_COMPLETION_OBSOLETE;
        return RDNA4_ARGUMENT_SUMMARY_STALE;
    }
    completion->state = RDNA4_ARGUMENT_SUMMARY_COMPLETION_COMPLETE;
    return RDNA4_ARGUMENT_SUMMARY_COMPLETE;
}

static inline rdna4_argument_summary_status
rdna4_argument_summary_completion_publish(
    rdna4_argument_summary_completion *completion,
    const rdna4_argument_summary_runtime_plan *plan,
    uint64_t current_generation,
    rdna4_argument_summary_publication *publication) {
    if (!completion || !plan || !publication)
        return RDNA4_ARGUMENT_SUMMARY_INVALID;
    *publication = (rdna4_argument_summary_publication){0};
    if (completion->state == RDNA4_ARGUMENT_SUMMARY_COMPLETION_FAILED)
        return RDNA4_ARGUMENT_SUMMARY_FAILED;
    if (completion->state == RDNA4_ARGUMENT_SUMMARY_COMPLETION_OBSOLETE)
        return RDNA4_ARGUMENT_SUMMARY_OBSOLETE;
    if (current_generation != completion->generation ||
        current_generation != plan->generation) {
        completion->state = RDNA4_ARGUMENT_SUMMARY_COMPLETION_OBSOLETE;
        return RDNA4_ARGUMENT_SUMMARY_STALE;
    }
    if (completion->state != RDNA4_ARGUMENT_SUMMARY_COMPLETION_COMPLETE)
        return RDNA4_ARGUMENT_SUMMARY_INCOMPLETE;
    if (completion->encoded_jobs != completion->job_count ||
        completion->job_count != plan->job_count)
        return RDNA4_ARGUMENT_SUMMARY_INCOMPLETE;
    *publication = (rdna4_argument_summary_publication){
        .generation = plan->generation,
        .source_size = plan->source_size,
        .completed_jobs = plan->job_count,
        .candidate_count = plan->candidate_count,
        .output_word_count = plan->output_word_count,
        .output_bytes =
            plan->output_word_count * (uint32_t)sizeof(uint32_t),
    };
    return RDNA4_ARGUMENT_SUMMARY_READY;
}

/* Vulkan bit values used by the host simulator and the bridge. */
#define RDNA4_ARGUMENT_SUMMARY_STAGE_TRANSFER 0x00001000u
#define RDNA4_ARGUMENT_SUMMARY_STAGE_COMPUTE 0x00000800u
#define RDNA4_ARGUMENT_SUMMARY_STAGE_HOST 0x00004000u
#define RDNA4_ARGUMENT_SUMMARY_ACCESS_TRANSFER_READ 0x00000800u
#define RDNA4_ARGUMENT_SUMMARY_ACCESS_TRANSFER_WRITE 0x00001000u
#define RDNA4_ARGUMENT_SUMMARY_ACCESS_SHADER_READ 0x00000020u
#define RDNA4_ARGUMENT_SUMMARY_ACCESS_SHADER_WRITE 0x00000040u
#define RDNA4_ARGUMENT_SUMMARY_ACCESS_HOST_READ 0x00002000u

/*
 * The bridge and the host simulator consume this exact operation sequence.
 * All fallible planning happens before recording, so sink callbacks are void.
 */
typedef struct {
    void *context;
    void (*bind)(void *context);
    void (*copy_dictionary)(void *context,
                            const rdna4_argument_summary_runtime_page *page);
    void (*fill_output)(void *context,
                        const rdna4_argument_summary_runtime_page *page,
                        uint32_t bytes);
    void (*copy_source)(void *context,
                        const rdna4_argument_summary_runtime_job *job,
                        const rdna4_argument_summary_source_copy *copy);
    void (*dispatch)(void *context,
                     const rdna4_argument_summary_runtime_job *job,
                     const rdna4_argument_summary_dispatch *dispatch,
                     const rdna4_argument_summary_push *push);
    void (*barrier)(void *context, uint32_t source_stage,
                    uint32_t source_access, uint32_t destination_stage,
                    uint32_t destination_access);
    void (*copy_output)(void *context,
                        const rdna4_argument_summary_runtime_page *page,
                        uint32_t bytes);
} rdna4_argument_summary_sink;

static inline bool rdna4_argument_summary_sink_valid(
    const rdna4_argument_summary_sink *sink) {
    return sink && sink->bind && sink->copy_dictionary &&
        sink->fill_output && sink->copy_source && sink->dispatch &&
        sink->barrier && sink->copy_output;
}

static inline rdna4_argument_summary_status
rdna4_argument_summary_runtime_record(
    const rdna4_argument_summary_runtime_plan *plan,
    rdna4_argument_summary_completion *completion,
    const rdna4_argument_summary_sink *sink, uint32_t max_groups) {
    if (!plan || !completion || !rdna4_argument_summary_sink_valid(sink) ||
        completion->state != RDNA4_ARGUMENT_SUMMARY_COMPLETION_ENCODING ||
        !max_groups)
        return RDNA4_ARGUMENT_SUMMARY_INVALID;

    if (plan->job_count)
        sink->bind(sink->context);
    for (uint32_t page_index = 0;
         page_index < plan->dictionary_page_count; ++page_index) {
        rdna4_argument_summary_runtime_page page;
        rdna4_argument_summary_status status =
            rdna4_argument_summary_runtime_page_get(
                plan, page_index, &page);
        if (status != RDNA4_ARGUMENT_SUMMARY_OK)
            return status;
        if (plan->job_count)
            sink->copy_dictionary(sink->context, &page);

        uint32_t output_bytes =
            page.output_word_count * (uint32_t)sizeof(uint32_t);
        sink->fill_output(sink->context, &page, output_bytes);
        sink->barrier(
            sink->context,
            RDNA4_ARGUMENT_SUMMARY_STAGE_TRANSFER,
            RDNA4_ARGUMENT_SUMMARY_ACCESS_TRANSFER_WRITE,
            RDNA4_ARGUMENT_SUMMARY_STAGE_TRANSFER |
                RDNA4_ARGUMENT_SUMMARY_STAGE_COMPUTE,
            RDNA4_ARGUMENT_SUMMARY_ACCESS_TRANSFER_READ |
                RDNA4_ARGUMENT_SUMMARY_ACCESS_SHADER_READ |
                RDNA4_ARGUMENT_SUMMARY_ACCESS_SHADER_WRITE);

        for (uint64_t chunk_index = 0;
             chunk_index < plan->chunk_count; ++chunk_index) {
            uint64_t job_index =
                (uint64_t)page_index * plan->chunk_count + chunk_index;
            rdna4_argument_summary_runtime_job job;
            status = rdna4_argument_summary_runtime_job_get(
                plan, job_index, &job);
            if (status != RDNA4_ARGUMENT_SUMMARY_OK)
                return status;
            rdna4_argument_summary_source_copy copy;
            status = rdna4_argument_summary_runtime_source_copy(
                &job, 0, plan->source_size, &copy);
            if (status != RDNA4_ARGUMENT_SUMMARY_OK)
                return status;
            sink->copy_source(sink->context, &job, &copy);
            sink->barrier(
                sink->context,
                RDNA4_ARGUMENT_SUMMARY_STAGE_TRANSFER,
                RDNA4_ARGUMENT_SUMMARY_ACCESS_TRANSFER_WRITE,
                RDNA4_ARGUMENT_SUMMARY_STAGE_COMPUTE,
                RDNA4_ARGUMENT_SUMMARY_ACCESS_SHADER_READ);

            rdna4_argument_summary_dispatch_cursor cursor;
            status = rdna4_argument_summary_runtime_dispatch_init(
                &cursor, job.source_count, max_groups);
            if (status != RDNA4_ARGUMENT_SUMMARY_OK)
                return status;
            rdna4_argument_summary_dispatch dispatch;
            while (rdna4_argument_summary_runtime_dispatch_next(
                       &cursor, &dispatch)) {
                rdna4_argument_summary_push push = job.push;
                push.source_begin = dispatch.source_begin;
                push.source_count = dispatch.source_count;
                sink->dispatch(sink->context, &job, &dispatch, &push);
            }
            sink->barrier(
                sink->context,
                RDNA4_ARGUMENT_SUMMARY_STAGE_COMPUTE,
                RDNA4_ARGUMENT_SUMMARY_ACCESS_SHADER_READ |
                    RDNA4_ARGUMENT_SUMMARY_ACCESS_SHADER_WRITE,
                RDNA4_ARGUMENT_SUMMARY_STAGE_TRANSFER,
                RDNA4_ARGUMENT_SUMMARY_ACCESS_TRANSFER_READ |
                    RDNA4_ARGUMENT_SUMMARY_ACCESS_TRANSFER_WRITE);
            status = rdna4_argument_summary_completion_job_encoded(completion);
            if (status != RDNA4_ARGUMENT_SUMMARY_OK)
                return status;
        }

        sink->copy_output(sink->context, &page, output_bytes);
        sink->barrier(
            sink->context,
            RDNA4_ARGUMENT_SUMMARY_STAGE_TRANSFER,
            RDNA4_ARGUMENT_SUMMARY_ACCESS_TRANSFER_READ |
                RDNA4_ARGUMENT_SUMMARY_ACCESS_TRANSFER_WRITE,
            RDNA4_ARGUMENT_SUMMARY_STAGE_TRANSFER |
                RDNA4_ARGUMENT_SUMMARY_STAGE_HOST,
            RDNA4_ARGUMENT_SUMMARY_ACCESS_TRANSFER_WRITE |
                RDNA4_ARGUMENT_SUMMARY_ACCESS_HOST_READ);
    }
    return RDNA4_ARGUMENT_SUMMARY_OK;
}

#if !defined(RDNA4_ARGUMENT_SUMMARY_RUNTIME_TEST)

#include "argument-summary/garnet_argument_summary_spv.h"

#include <pthread.h>

@interface RDNA4ArgumentSummaryTask : NSObject {
@public
    rdna4_argument_summary_runtime_plan _plan;
    RDNA4Buffer *_sourceBuffer;
    RDNA4Buffer *_dictionaryBuffer;
    RDNA4Buffer *_outputBuffer;
    uint64_t _sourceOffset;
    uint64_t _dictionaryOffset;
    uint64_t _outputOffset;
}
- (instancetype)initWithSourceBuffer:(RDNA4Buffer *)sourceBuffer
                          sourceOffset:(NSUInteger)sourceOffset
                            sourceSize:(NSUInteger)sourceSize
                    dictionaryBuffer:(RDNA4Buffer *)dictionaryBuffer
                    dictionaryOffset:(NSUInteger)dictionaryOffset
                      candidateCount:(uint32_t)candidateCount
                        outputBuffer:(RDNA4Buffer *)outputBuffer
                        outputOffset:(NSUInteger)outputOffset
                          generation:(uint64_t)generation
                              limits:(const rdna4_argument_summary_runtime_limits *)limits
                  requestedChunkBytes:(uint32_t)requestedChunkBytes
             requestedDictionaryPage:(uint32_t)requestedDictionaryPage
                               error:(NSError **)error;
- (uint64_t)generation;
- (uint32_t)outputWordCount;
- (RDNA4Buffer *)sourceBuffer;
- (RDNA4Buffer *)dictionaryBuffer;
- (RDNA4Buffer *)outputBuffer;
- (uint64_t)sourceOffset;
- (uint64_t)dictionaryOffset;
- (uint64_t)outputOffset;
- (const rdna4_argument_summary_runtime_plan *)runtimePlan;
@end

@interface RDNA4ArgumentSummaryAttempt : NSObject {
@public
    RDNA4ArgumentSummaryTask *_task;
    rdna4_argument_summary_completion _completion;
    RDNA4Buffer *_sourceScratch;
    RDNA4Buffer *_dictionaryScratch;
    RDNA4Buffer *_outputScratch;
    VkDescriptorPool _descriptorPool;
    VkDescriptorSet _descriptorSet;
}
- (uint64_t)generation;
- (uint64_t)jobCount;
- (uint64_t)encodedJobs;
@end

static pthread_mutex_t rdna4_argument_summary_pipeline_lock =
    PTHREAD_MUTEX_INITIALIZER;
static rdna4_argument_summary_pipeline_state rdna4_argument_summary_pipeline;

static NSError *rdna4_argument_summary_runtime_error(
    rdna4_argument_summary_status status, NSString *message) {
    return rdna4_err(49, [NSString stringWithFormat:@"argument summary (%d): %@",
                          (int)status, message]);
}

static int32_t rdna4_argument_summary_create_shader(
    void *context, uintptr_t *handle) {
    (void)context;
    VkShaderModule module = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof rdna4_argument_summary_cs_spv,
        .pCode = (const uint32_t *)(const void *)rdna4_argument_summary_cs_spv,
    };
    VkResult result = vkCreateShaderModule(
        RDNA4R.dev, &info, NULL, &module);
    *handle = (uintptr_t)module;
    return (int32_t)result;
}

static int32_t rdna4_argument_summary_create_descriptor_layout(
    void *context, uintptr_t *handle) {
    (void)context;
    VkDescriptorSetLayoutBinding bindings[3] = {
        {
            .binding = RDNA4_ARGUMENT_SUMMARY_BINDING_SOURCE,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        },
        {
            .binding = RDNA4_ARGUMENT_SUMMARY_BINDING_DICTIONARY,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        },
        {
            .binding = RDNA4_ARGUMENT_SUMMARY_BINDING_OUTPUT,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        },
    };
    VkDescriptorSetLayoutCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 3,
        .pBindings = bindings,
    };
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    VkResult result = vkCreateDescriptorSetLayout(
        RDNA4R.dev, &info, NULL, &layout);
    *handle = (uintptr_t)layout;
    return (int32_t)result;
}

static int32_t rdna4_argument_summary_create_pipeline_layout(
    void *context, uintptr_t shader, uintptr_t descriptor_layout,
    uintptr_t *handle) {
    (void)context;
    (void)shader;
    VkDescriptorSetLayout set = (VkDescriptorSetLayout)descriptor_layout;
    VkPushConstantRange push = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = sizeof(rdna4_argument_summary_push),
    };
    VkPipelineLayoutCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &set,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push,
    };
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkResult result = vkCreatePipelineLayout(
        RDNA4R.dev, &info, NULL, &layout);
    *handle = (uintptr_t)layout;
    return (int32_t)result;
}

static int32_t rdna4_argument_summary_create_pipeline(
    void *context, uintptr_t shader, uintptr_t pipeline_layout,
    uintptr_t *handle) {
    (void)context;
    VkComputePipelineCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .layout = (VkPipelineLayout)pipeline_layout,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = (VkShaderModule)shader,
            .pName = "main",
        },
    };
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkResult result = vkCreateComputePipelines(
        RDNA4R.dev, RDNA4R.pc, 1, &info, NULL, &pipeline);
    *handle = (uintptr_t)pipeline;
    return (int32_t)result;
}

static void rdna4_argument_summary_destroy_shader(
    void *context, uintptr_t handle) {
    (void)context;
    vkDestroyShaderModule(RDNA4R.dev, (VkShaderModule)handle, NULL);
}

static void rdna4_argument_summary_destroy_descriptor_layout(
    void *context, uintptr_t handle) {
    (void)context;
    vkDestroyDescriptorSetLayout(
        RDNA4R.dev, (VkDescriptorSetLayout)handle, NULL);
}

static void rdna4_argument_summary_destroy_pipeline_layout(
    void *context, uintptr_t handle) {
    (void)context;
    vkDestroyPipelineLayout(
        RDNA4R.dev, (VkPipelineLayout)handle, NULL);
}

static void rdna4_argument_summary_destroy_pipeline(
    void *context, uintptr_t handle) {
    (void)context;
    vkDestroyPipeline(RDNA4R.dev, (VkPipeline)handle, NULL);
}

static const rdna4_argument_summary_pipeline_ops
rdna4_argument_summary_vulkan_pipeline_ops = {
    .create_shader = rdna4_argument_summary_create_shader,
    .create_descriptor_layout =
        rdna4_argument_summary_create_descriptor_layout,
    .create_pipeline_layout = rdna4_argument_summary_create_pipeline_layout,
    .create_pipeline = rdna4_argument_summary_create_pipeline,
    .destroy_shader = rdna4_argument_summary_destroy_shader,
    .destroy_descriptor_layout =
        rdna4_argument_summary_destroy_descriptor_layout,
    .destroy_pipeline_layout = rdna4_argument_summary_destroy_pipeline_layout,
    .destroy_pipeline = rdna4_argument_summary_destroy_pipeline,
};

static rdna4_argument_summary_status
rdna4_argument_summary_ensure_pipeline(NSError **error) {
    rdna4_argument_summary_status status;
    pthread_mutex_lock(&rdna4_argument_summary_pipeline_lock);
    status = rdna4_argument_summary_pipeline_ensure(
        &rdna4_argument_summary_pipeline,
        &rdna4_argument_summary_vulkan_pipeline_ops,
        (uintptr_t)RDNA4R.dev);
    pthread_mutex_unlock(&rdna4_argument_summary_pipeline_lock);
    if (status != RDNA4_ARGUMENT_SUMMARY_OK && error)
        *error = rdna4_argument_summary_runtime_error(
            status, @"lazy shader/descriptor/pipeline creation failed");
    return status;
}

/*
 * RDNA4R has no vkDestroyDevice call site. Keep this teardown hook available
 * for a future explicit device teardown without registering an owner, atexit,
 * or another lifetime path. Until then it is intentionally unreferenced.
 */
static void __attribute__((unused))
rdna4_argument_summary_pipeline_destroy(void) {
    pthread_mutex_lock(&rdna4_argument_summary_pipeline_lock);
    rdna4_argument_summary_pipeline_release(
        &rdna4_argument_summary_pipeline,
        &rdna4_argument_summary_vulkan_pipeline_ops);
    pthread_mutex_unlock(&rdna4_argument_summary_pipeline_lock);
}

static BOOL rdna4_argument_summary_range_fits(
    NSUInteger length, uint64_t offset, uint64_t size) {
    return offset <= length && size <= (uint64_t)length - offset;
}

static BOOL rdna4_argument_summary_ranges_overlap(
    uint64_t a_offset, uint64_t a_size,
    uint64_t b_offset, uint64_t b_size) {
    if (!a_size || !b_size) return NO;
    return a_offset < b_offset + b_size &&
        b_offset < a_offset + a_size;
}

@implementation RDNA4ArgumentSummaryTask
- (instancetype)initWithSourceBuffer:(RDNA4Buffer *)sourceBuffer
                          sourceOffset:(NSUInteger)sourceOffset
                            sourceSize:(NSUInteger)sourceSize
                    dictionaryBuffer:(RDNA4Buffer *)dictionaryBuffer
                    dictionaryOffset:(NSUInteger)dictionaryOffset
                      candidateCount:(uint32_t)candidateCount
                        outputBuffer:(RDNA4Buffer *)outputBuffer
                        outputOffset:(NSUInteger)outputOffset
                          generation:(uint64_t)generation
                              limits:(const rdna4_argument_summary_runtime_limits *)limits
                  requestedChunkBytes:(uint32_t)requestedChunkBytes
             requestedDictionaryPage:(uint32_t)requestedDictionaryPage
                               error:(NSError **)error {
    self = [super init];
    if (!self) return nil;
    if (!sourceBuffer || ![sourceBuffer isKindOfClass:[RDNA4Buffer class]] ||
        !outputBuffer || ![outputBuffer isKindOfClass:[RDNA4Buffer class]] ||
        (candidateCount && (!dictionaryBuffer ||
         ![dictionaryBuffer isKindOfClass:[RDNA4Buffer class]]))) {
        if (error) *error = rdna4_argument_summary_runtime_error(
            RDNA4_ARGUMENT_SUMMARY_INVALID,
            @"source/output/dictionary buffer has the wrong type");
        return nil;
    }
    if (!rdna4_argument_summary_range_fits(
            sourceBuffer.length, sourceOffset, sourceSize)) {
        if (error) *error = rdna4_argument_summary_runtime_error(
            RDNA4_ARGUMENT_SUMMARY_INVALID,
            @"source range is outside the source buffer");
        return nil;
    }
    uint64_t dictionaryBytes =
        (uint64_t)candidateCount *
        sizeof(rdna4_argument_summary_candidate);
    if ((candidateCount &&
         !rdna4_argument_summary_range_fits(
             dictionaryBuffer.length, dictionaryOffset,
             dictionaryBytes))) {
        if (error) *error = rdna4_argument_summary_runtime_error(
            RDNA4_ARGUMENT_SUMMARY_INVALID,
            @"dictionary range is outside the dictionary buffer");
        return nil;
    }
    id device = sourceBuffer.device;
    if ((candidateCount && dictionaryBuffer.device != device) ||
        outputBuffer.device != device) {
        if (error) *error = rdna4_argument_summary_runtime_error(
            RDNA4_ARGUMENT_SUMMARY_INVALID,
            @"summary buffers belong to different devices");
        return nil;
    }
    if (!outputBuffer.contents || [outputBuffer rdna4HostReadOnly] ||
        outputBuffer.storageMode == MTLStorageModePrivate) {
        if (error) *error = rdna4_argument_summary_runtime_error(
            RDNA4_ARGUMENT_SUMMARY_INVALID,
            @"summary output must be a host-coherent Shared/Managed buffer");
        return nil;
    }
    if (outputOffset % 4u) {
        if (error) *error = rdna4_argument_summary_runtime_error(
            RDNA4_ARGUMENT_SUMMARY_INVALID,
            @"summary output range must begin on a 32-bit word");
        return nil;
    }

    rdna4_argument_summary_runtime_limits localLimits;
    if (!limits) {
        localLimits = (rdna4_argument_summary_runtime_limits){
            .max_storage_buffer_range =
                RDNA4R.lim.maxStorageBufferRange,
            .min_storage_buffer_offset_alignment =
                (uint32_t)RDNA4R.lim.minStorageBufferOffsetAlignment,
            .max_compute_workgroup_count_x =
                RDNA4R.lim.maxComputeWorkGroupCount[0],
            .max_compute_workgroup_invocations =
                RDNA4R.lim.maxComputeWorkGroupInvocations,
            .max_compute_workgroup_size_x =
                RDNA4R.lim.maxComputeWorkGroupSize[0],
            .requested_chunk_bytes = requestedChunkBytes,
            .requested_dictionary_page = requestedDictionaryPage,
        };
    } else {
        localLimits = *limits;
        if (requestedChunkBytes)
            localLimits.requested_chunk_bytes = requestedChunkBytes;
        if (requestedDictionaryPage)
            localLimits.requested_dictionary_page =
                requestedDictionaryPage;
    }
    limits = &localLimits;
    rdna4_argument_summary_status status =
        rdna4_argument_summary_runtime_plan_init(
            &_plan, limits, sourceSize, candidateCount, generation);
    if (status != RDNA4_ARGUMENT_SUMMARY_OK) {
        if (error) *error = rdna4_argument_summary_runtime_error(
            status, @"invalid chunk/page limits or plan overflow");
        return nil;
    }
    uint64_t outputBytes =
        (uint64_t)_plan.output_word_count * sizeof(uint32_t);
    if (!rdna4_argument_summary_range_fits(
            outputBuffer.length, outputOffset, outputBytes)) {
        if (error) *error = rdna4_argument_summary_runtime_error(
            RDNA4_ARGUMENT_SUMMARY_INVALID,
            @"output bitset is outside the output buffer");
        return nil;
    }
    if ((outputBuffer == sourceBuffer &&
         rdna4_argument_summary_ranges_overlap(
             outputOffset, outputBytes, sourceOffset, sourceSize)) ||
        (outputBuffer == dictionaryBuffer && candidateCount &&
         rdna4_argument_summary_ranges_overlap(
             outputOffset, outputBytes, dictionaryOffset,
             dictionaryBytes))) {
        if (error) *error = rdna4_argument_summary_runtime_error(
            RDNA4_ARGUMENT_SUMMARY_INVALID,
            @"output bitset overlaps a read-only summary range");
        return nil;
    }

    _sourceBuffer = sourceBuffer;
    _dictionaryBuffer = dictionaryBuffer;
    _outputBuffer = outputBuffer;
    _sourceOffset = sourceOffset;
    _dictionaryOffset = dictionaryOffset;
    _outputOffset = outputOffset;
    return self;
}

- (uint64_t)generation { return _plan.generation; }
- (uint32_t)outputWordCount { return _plan.output_word_count; }
- (RDNA4Buffer *)sourceBuffer { return _sourceBuffer; }
- (RDNA4Buffer *)dictionaryBuffer { return _dictionaryBuffer; }
- (RDNA4Buffer *)outputBuffer { return _outputBuffer; }
- (uint64_t)sourceOffset { return _sourceOffset; }
- (uint64_t)dictionaryOffset { return _dictionaryOffset; }
- (uint64_t)outputOffset { return _outputOffset; }
- (const rdna4_argument_summary_runtime_plan *)runtimePlan { return &_plan; }
@end

static RDNA4ArgumentSummaryTask *rdna4_argument_summary_task_new(
    RDNA4Buffer *sourceBuffer, NSUInteger sourceOffset,
    NSUInteger sourceSize, RDNA4Buffer *dictionaryBuffer,
    NSUInteger dictionaryOffset, uint32_t candidateCount,
    RDNA4Buffer *outputBuffer, NSUInteger outputOffset,
    uint64_t generation,
    const rdna4_argument_summary_runtime_limits *limits,
    uint32_t requestedChunkBytes, uint32_t requestedDictionaryPage,
    NSError **error) {
    return [[RDNA4ArgumentSummaryTask alloc]
        initWithSourceBuffer:sourceBuffer
                sourceOffset:sourceOffset
                  sourceSize:sourceSize
          dictionaryBuffer:dictionaryBuffer
          dictionaryOffset:dictionaryOffset
            candidateCount:candidateCount
              outputBuffer:outputBuffer
              outputOffset:outputOffset
                generation:generation
                    limits:limits
        requestedChunkBytes:requestedChunkBytes
   requestedDictionaryPage:requestedDictionaryPage
                     error:error];
}

static RDNA4Buffer *rdna4_argument_summary_private_scratch(
    id device, uint32_t bytes, NSError **error) {
    if (!bytes) return nil;
    NSUInteger options = MTLResourceStorageModePrivate;
    if (RDNA4P.on) options |= RDNA4_OPT_NOZERO;
    return [[RDNA4Buffer alloc] initWithDevice:device
                                        length:bytes
                                       options:options
                                         error:error];
}

static BOOL rdna4_argument_summary_make_descriptor_set(
    RDNA4ArgumentSummaryAttempt *attempt, VkDevice device, NSError **error) {
    VkDescriptorPoolSize size = {
        .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 3,
    };
    VkDescriptorPoolCreateInfo poolInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &size,
    };
    VkResult result = vkCreateDescriptorPool(
        device, &poolInfo, NULL, &attempt->_descriptorPool);
    if (result != VK_SUCCESS) {
        if (error) *error = rdna4_argument_summary_runtime_error(
            RDNA4_ARGUMENT_SUMMARY_FAILED,
            [NSString stringWithFormat:
                @"vkCreateDescriptorPool(summary) = %d", result]);
        return NO;
    }
    VkDescriptorSetLayout setLayout =
        (VkDescriptorSetLayout)
            rdna4_argument_summary_pipeline.descriptor_layout;
    VkDescriptorSetAllocateInfo allocate = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = attempt->_descriptorPool,
        .descriptorSetCount = 1,
        .pSetLayouts = &setLayout,
    };
    result = vkAllocateDescriptorSets(
        device, &allocate, &attempt->_descriptorSet);
    if (result != VK_SUCCESS) {
        if (error) *error = rdna4_argument_summary_runtime_error(
            RDNA4_ARGUMENT_SUMMARY_FAILED,
            [NSString stringWithFormat:
                @"vkAllocateDescriptorSets(summary) = %d", result]);
        return NO;
    }

    RDNA4ArgumentSummaryTask *task = attempt->_task;
    VkDescriptorBufferInfo buffers[3] = {
        {
            .buffer = [attempt->_sourceScratch vkBuffer],
            .offset = [attempt->_sourceScratch rdna4VkOffset],
            .range = task->_plan.source_binding_bytes,
        },
        {
            .buffer = [attempt->_dictionaryScratch vkBuffer],
            .offset = [attempt->_dictionaryScratch rdna4VkOffset],
            .range = task->_plan.dictionary_binding_bytes,
        },
        {
            .buffer = [attempt->_outputScratch vkBuffer],
            .offset = [attempt->_outputScratch rdna4VkOffset],
            .range = task->_plan.output_page_bytes,
        },
    };
    VkWriteDescriptorSet writes[3];
    for (uint32_t i = 0; i < 3; ++i) {
        writes[i] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = attempt->_descriptorSet,
            .dstBinding = i,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &buffers[i],
        };
    }
    vkUpdateDescriptorSets(device, 3, writes, 0, NULL);
    return YES;
}

@implementation RDNA4ArgumentSummaryAttempt
- (instancetype)initWithTask:(RDNA4ArgumentSummaryTask *)task
                      device:(VkDevice)device
                       error:(NSError **)error {
    self = [super init];
    if (!self) return nil;
    _task = task;
    rdna4_argument_summary_completion_init(&_completion, &task->_plan);
    rdna4_argument_summary_scratch scratch =
        rdna4_argument_summary_runtime_scratch(&task->_plan);
    id metalDevice = task->_sourceBuffer.device;

    if (scratch.source_staging_bytes) {
        _sourceScratch = rdna4_argument_summary_private_scratch(
            metalDevice, scratch.source_staging_bytes, error);
        if (!_sourceScratch) return nil;
    }
    if (scratch.dictionary_staging_bytes) {
        _dictionaryScratch = rdna4_argument_summary_private_scratch(
            metalDevice, scratch.dictionary_staging_bytes, error);
        if (!_dictionaryScratch) return nil;
    }
    if (scratch.output_staging_bytes) {
        _outputScratch = rdna4_argument_summary_private_scratch(
            metalDevice, scratch.output_staging_bytes, error);
        if (!_outputScratch) return nil;
    }
    if (task->_plan.job_count &&
        !rdna4_argument_summary_make_descriptor_set(self, device, error))
        return nil;
    return self;
}

- (void)dealloc {
    if (_descriptorPool && RDNA4R.ok)
        vkDestroyDescriptorPool(RDNA4R.dev, _descriptorPool, NULL);
}

- (uint64_t)generation { return _completion.generation; }
- (uint64_t)jobCount { return _completion.job_count; }
- (uint64_t)encodedJobs { return _completion.encoded_jobs; }
@end

static BOOL rdna4_argument_summary_preflight(
    RDNA4ArgumentSummaryTask *task, NSError **error) {
    const rdna4_argument_summary_runtime_plan *plan = &task->_plan;
    rdna4_argument_summary_scratch scratch =
        rdna4_argument_summary_runtime_scratch(plan);
    if (scratch.source_staging_bytes > plan->source_binding_bytes ||
        scratch.dictionary_staging_bytes > plan->dictionary_binding_bytes ||
        scratch.output_staging_bytes > plan->output_page_bytes) {
        if (error) *error = rdna4_argument_summary_runtime_error(
            RDNA4_ARGUMENT_SUMMARY_INVALID,
            @"scratch sizes exceed their descriptor ranges");
        return NO;
    }
    for (uint32_t page_index = 0;
         page_index < plan->dictionary_page_count; ++page_index) {
        rdna4_argument_summary_runtime_page page = {0};
        rdna4_argument_summary_status status =
            rdna4_argument_summary_runtime_page_get(
                plan, page_index, &page);
        uint32_t pageBytes = status == RDNA4_ARGUMENT_SUMMARY_OK
            ? page.output_word_count * (uint32_t)sizeof(uint32_t) : 0;
        uint64_t dictionaryBegin =
            task->_dictionaryOffset + page.dictionary_offset;
        uint64_t outputBegin =
            task->_outputOffset +
            (uint64_t)page.output_word_base * sizeof(uint32_t);
        if (status != RDNA4_ARGUMENT_SUMMARY_OK ||
            (page.dictionary_bytes &&
             !rdna4_argument_summary_range_fits(
                 task->_dictionaryBuffer.length, dictionaryBegin,
                 page.dictionary_bytes)) ||
            (pageBytes &&
             !rdna4_argument_summary_range_fits(
                 task->_outputBuffer.length, outputBegin, pageBytes)) ||
            page.output_word_base + page.output_word_count >
                plan->output_word_count) {
            if (error) *error = rdna4_argument_summary_runtime_error(
                RDNA4_ARGUMENT_SUMMARY_INVALID,
                @"dictionary/output page is outside its final range");
            return NO;
        }
    }
    for (uint64_t index = 0; index < plan->job_count; ++index) {
        rdna4_argument_summary_runtime_job job;
        rdna4_argument_summary_status status =
            rdna4_argument_summary_runtime_job_get(plan, index, &job);
        if (status != RDNA4_ARGUMENT_SUMMARY_OK) {
            if (error) *error = rdna4_argument_summary_runtime_error(
                status, @"invalid runtime job");
            return NO;
        }
        rdna4_argument_summary_source_copy copy;
        status = rdna4_argument_summary_runtime_source_copy(
            &job, task->_sourceOffset,
            plan->source_size, &copy);
        if (status != RDNA4_ARGUMENT_SUMMARY_OK ||
            copy.source_binding_bytes > plan->source_binding_bytes ||
            copy.copy_bytes > copy.source_binding_bytes) {
            if (error) *error = rdna4_argument_summary_runtime_error(
                RDNA4_ARGUMENT_SUMMARY_INVALID,
                @"source copy or padded binding is out of bounds");
            return NO;
        }
        rdna4_argument_summary_dispatch_cursor dispatch;
        status = rdna4_argument_summary_runtime_dispatch_init(
            &dispatch, job.source_count,
            RDNA4R.lim.maxComputeWorkGroupCount[0]);
        if (status != RDNA4_ARGUMENT_SUMMARY_OK) {
            if (error) *error = rdna4_argument_summary_runtime_error(
                status, @"invalid compute workgroup-count limit");
            return NO;
        }
        rdna4_argument_summary_dispatch part;
        while (rdna4_argument_summary_runtime_dispatch_next(
                   &dispatch, &part)) {
            if (!part.source_count || !part.group_count ||
                part.group_count >
                    RDNA4R.lim.maxComputeWorkGroupCount[0]) {
                if (error) *error = rdna4_argument_summary_runtime_error(
                    RDNA4_ARGUMENT_SUMMARY_INVALID,
                    @"dispatch partition exceeds the device limit");
                return NO;
            }
        }
    }
    return YES;
}

static void rdna4_argument_summary_barrier(
    VkCommandBuffer command, VkPipelineStageFlags source_stage,
    VkAccessFlags source_access, VkPipelineStageFlags destination_stage,
    VkAccessFlags destination_access) {
    VkMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = source_access,
        .dstAccessMask = destination_access,
    };
    vkCmdPipelineBarrier(
        command, source_stage, destination_stage, 0,
        1, &barrier, 0, NULL, 0, NULL);
}

static BOOL rdna4_argument_summary_vulkan_offset(
    RDNA4Buffer *buffer, uint64_t logical_offset, uint64_t size,
    VkDeviceSize *offset) {
    if (!buffer || !offset ||
        !rdna4_argument_summary_range_fits(
            buffer.length, logical_offset, size))
        return NO;
    uint64_t vk_offset = [buffer rdna4VkOffset];
    if (vk_offset > UINT64_MAX - logical_offset)
        return NO;
    *offset = (VkDeviceSize)(vk_offset + logical_offset);
    return YES;
}

_Static_assert(RDNA4_ARGUMENT_SUMMARY_STAGE_TRANSFER ==
                   VK_PIPELINE_STAGE_TRANSFER_BIT,
               "summary transfer-stage ABI");
_Static_assert(RDNA4_ARGUMENT_SUMMARY_STAGE_COMPUTE ==
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
               "summary compute-stage ABI");
_Static_assert(RDNA4_ARGUMENT_SUMMARY_STAGE_HOST == VK_PIPELINE_STAGE_HOST_BIT,
               "summary host-stage ABI");
_Static_assert(RDNA4_ARGUMENT_SUMMARY_ACCESS_TRANSFER_READ ==
                   VK_ACCESS_TRANSFER_READ_BIT,
               "summary transfer-read ABI");
_Static_assert(RDNA4_ARGUMENT_SUMMARY_ACCESS_TRANSFER_WRITE ==
                   VK_ACCESS_TRANSFER_WRITE_BIT,
               "summary transfer-write ABI");
_Static_assert(RDNA4_ARGUMENT_SUMMARY_ACCESS_SHADER_READ ==
                   VK_ACCESS_SHADER_READ_BIT,
               "summary shader-read ABI");
_Static_assert(RDNA4_ARGUMENT_SUMMARY_ACCESS_SHADER_WRITE ==
                   VK_ACCESS_SHADER_WRITE_BIT,
               "summary shader-write ABI");
_Static_assert(RDNA4_ARGUMENT_SUMMARY_ACCESS_HOST_READ ==
                   VK_ACCESS_HOST_READ_BIT,
               "summary host-read ABI");

typedef struct {
    RDNA4ArgumentSummaryTask *task;
    RDNA4ArgumentSummaryAttempt *attempt;
    VkCommandBuffer command;
} rdna4_argument_summary_sink_context;

static void rdna4_argument_summary_sink_bind(void *opaque) {
    rdna4_argument_summary_sink_context *context = opaque;
    vkCmdBindPipeline(
        context->command, VK_PIPELINE_BIND_POINT_COMPUTE,
        (VkPipeline)rdna4_argument_summary_pipeline.pipeline);
    vkCmdBindDescriptorSets(
        context->command, VK_PIPELINE_BIND_POINT_COMPUTE,
        (VkPipelineLayout)rdna4_argument_summary_pipeline.pipeline_layout,
        0, 1, &context->attempt->_descriptorSet, 0, NULL);
}

static void rdna4_argument_summary_sink_copy_dictionary(
    void *opaque, const rdna4_argument_summary_runtime_page *page) {
    rdna4_argument_summary_sink_context *context = opaque;
    RDNA4ArgumentSummaryTask *task = context->task;
    RDNA4ArgumentSummaryAttempt *attempt = context->attempt;
    VkDeviceSize source = 0;
    VkDeviceSize destination = 0;
    (void)rdna4_argument_summary_vulkan_offset(
        task->_dictionaryBuffer,
        task->_dictionaryOffset + page->dictionary_offset,
        page->dictionary_bytes, &source);
    (void)rdna4_argument_summary_vulkan_offset(
        attempt->_dictionaryScratch, 0, page->dictionary_bytes,
        &destination);
    VkBufferCopy copy = {
        .srcOffset = source,
        .dstOffset = destination,
        .size = page->dictionary_bytes,
    };
    vkCmdCopyBuffer(
        context->command, [task->_dictionaryBuffer vkBuffer],
        [attempt->_dictionaryScratch vkBuffer], 1, &copy);
}

static void rdna4_argument_summary_sink_fill_output(
    void *opaque, const rdna4_argument_summary_runtime_page *page,
    uint32_t bytes) {
    (void)page;
    rdna4_argument_summary_sink_context *context = opaque;
    VkDeviceSize offset = 0;
    (void)rdna4_argument_summary_vulkan_offset(
        context->attempt->_outputScratch, 0, bytes, &offset);
    vkCmdFillBuffer(
        context->command,
        [context->attempt->_outputScratch vkBuffer],
        offset, bytes, 0);
}

static void rdna4_argument_summary_sink_copy_source(
    void *opaque, const rdna4_argument_summary_runtime_job *job,
    const rdna4_argument_summary_source_copy *copy) {
    (void)job;
    rdna4_argument_summary_sink_context *context = opaque;
    RDNA4ArgumentSummaryTask *task = context->task;
    uint64_t logical = task->_sourceOffset + copy->source_offset;
    VkDeviceSize source = 0;
    VkDeviceSize destination = 0;
    (void)rdna4_argument_summary_vulkan_offset(
        task->_sourceBuffer, logical, copy->copy_bytes, &source);
    (void)rdna4_argument_summary_vulkan_offset(
        context->attempt->_sourceScratch, 0, copy->copy_bytes,
        &destination);
    VkBufferCopy region = {
        .srcOffset = source,
        .dstOffset = destination,
        .size = copy->copy_bytes,
    };
    vkCmdCopyBuffer(
        context->command, [task->_sourceBuffer vkBuffer],
        [context->attempt->_sourceScratch vkBuffer], 1, &region);
}

static void rdna4_argument_summary_sink_dispatch(
    void *opaque, const rdna4_argument_summary_runtime_job *job,
    const rdna4_argument_summary_dispatch *dispatch,
    const rdna4_argument_summary_push *push) {
    (void)job;
    rdna4_argument_summary_sink_context *context = opaque;
    vkCmdPushConstants(
        context->command,
        (VkPipelineLayout)rdna4_argument_summary_pipeline.pipeline_layout,
        VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof *push, push);
    vkCmdDispatch(
        context->command, dispatch->group_count, 1, 1);
}

static void rdna4_argument_summary_sink_barrier(
    void *opaque, uint32_t source_stage, uint32_t source_access,
    uint32_t destination_stage, uint32_t destination_access) {
    rdna4_argument_summary_sink_context *context = opaque;
    rdna4_argument_summary_barrier(
        context->command, (VkPipelineStageFlags)source_stage,
        (VkAccessFlags)source_access,
        (VkPipelineStageFlags)destination_stage,
        (VkAccessFlags)destination_access);
}

static void rdna4_argument_summary_sink_copy_output(
    void *opaque, const rdna4_argument_summary_runtime_page *page,
    uint32_t bytes) {
    rdna4_argument_summary_sink_context *context = opaque;
    RDNA4ArgumentSummaryTask *task = context->task;
    VkDeviceSize source = 0;
    VkDeviceSize destination = 0;
    (void)rdna4_argument_summary_vulkan_offset(
        context->attempt->_outputScratch, 0, bytes, &source);
    (void)rdna4_argument_summary_vulkan_offset(
        task->_outputBuffer,
        task->_outputOffset +
            (uint64_t)page->output_word_base * sizeof(uint32_t),
        bytes, &destination);
    VkBufferCopy copy = {
        .srcOffset = source,
        .dstOffset = destination,
        .size = bytes,
    };
    vkCmdCopyBuffer(
        context->command,
        [context->attempt->_outputScratch vkBuffer],
        [task->_outputBuffer vkBuffer], 1, &copy);
}

static rdna4_argument_summary_status rdna4_argument_summary_record(
    RDNA4ArgumentSummaryTask *task, RDNA4ArgumentSummaryAttempt *attempt,
    VkCommandBuffer command) {
    rdna4_argument_summary_sink_context context = {
        .task = task,
        .attempt = attempt,
        .command = command,
    };
    const rdna4_argument_summary_sink sink = {
        .context = &context,
        .bind = rdna4_argument_summary_sink_bind,
        .copy_dictionary = rdna4_argument_summary_sink_copy_dictionary,
        .fill_output = rdna4_argument_summary_sink_fill_output,
        .copy_source = rdna4_argument_summary_sink_copy_source,
        .dispatch = rdna4_argument_summary_sink_dispatch,
        .barrier = rdna4_argument_summary_sink_barrier,
        .copy_output = rdna4_argument_summary_sink_copy_output,
    };
    return rdna4_argument_summary_runtime_record(
        &task->_plan, &attempt->_completion, &sink,
        RDNA4R.lim.maxComputeWorkGroupCount[0]);
}

static BOOL rdna4_argument_summary_encode(
    RDNA4ArgumentSummaryTask *task, VkCommandBuffer command,
    RDNA4ArgumentSummaryAttempt **outAttempt, NSError **error) {
    if (outAttempt) *outAttempt = nil;
    if (!task || !command || !outAttempt) {
        if (error) *error = rdna4_argument_summary_runtime_error(
            RDNA4_ARGUMENT_SUMMARY_INVALID,
            @"encode needs a task, command buffer and attempt output");
        return NO;
    }
    if (!rdna4_argument_summary_preflight(task, error))
        return NO;
    if (task->_plan.job_count &&
        rdna4_argument_summary_ensure_pipeline(error) !=
            RDNA4_ARGUMENT_SUMMARY_OK)
        return NO;
    RDNA4ArgumentSummaryAttempt *attempt =
        [[RDNA4ArgumentSummaryAttempt alloc]
            initWithTask:task device:RDNA4R.dev error:error];
    if (!attempt) return NO;
    rdna4_argument_summary_status status =
        rdna4_argument_summary_record(task, attempt, command);
    if (status != RDNA4_ARGUMENT_SUMMARY_OK) {
        if (error) *error = rdna4_argument_summary_runtime_error(
            status, @"immutable summary plan failed during recording");
        return NO;
    }
    status =
        rdna4_argument_summary_completion_finish_encode(
            &attempt->_completion, true);
    if (status != RDNA4_ARGUMENT_SUMMARY_OK) {
        if (error) *error = rdna4_argument_summary_runtime_error(
            status, @"summary encode did not complete every job");
        return NO;
    }
    *outAttempt = attempt;
    return YES;
}

static rdna4_argument_summary_status
rdna4_argument_summary_attempt_complete(
    RDNA4ArgumentSummaryAttempt *attempt, BOOL fenceSucceeded,
    uint64_t currentGeneration) {
    if (!attempt) return RDNA4_ARGUMENT_SUMMARY_INVALID;
    return rdna4_argument_summary_completion_fence(
        &attempt->_completion, fenceSucceeded != NO, currentGeneration);
}

static rdna4_argument_summary_status
rdna4_argument_summary_attempt_publish(
    RDNA4ArgumentSummaryAttempt *attempt, uint64_t currentGeneration,
    rdna4_argument_summary_publication *publication) {
    if (!attempt) return RDNA4_ARGUMENT_SUMMARY_INVALID;
    rdna4_argument_summary_status status =
        rdna4_argument_summary_completion_publish(
        &attempt->_completion, &attempt->_task->_plan,
        currentGeneration, publication);
    if (status == RDNA4_ARGUMENT_SUMMARY_READY) {
        publication->source_offset = attempt->_task->_sourceOffset;
        publication->output_offset = attempt->_task->_outputOffset;
    }
    return status;
}

#endif

#endif
#endif // RDNA4_ARGUMENT_SUMMARY_RUNTIME_TEST
#if defined(GARNET_ARGUMENT_SUMMARY_OWNERS_ONLY) || (!defined(RDNA4_ARGUMENT_SUMMARY_RUNTIME_TEST))
/*
 * Private-buffer sampler-owner publication for the argument-summary GPU pass.
 *
 * Include after rdna4_argument.inc and rdna4_argument_summary.inc. Encoding
 * snapshots one complete buffer's current owner candidates, but publication is
 * ordered by the command segment's accepted submission epoch, never by the
 * encode-time order. The command buffer keeps the returned attempt through its
 * existing fence; this file creates no queue, submit, or wait.
 */

#ifndef RDNA4_ARGUMENT_SUMMARY_OWNERS_INC
#define RDNA4_ARGUMENT_SUMMARY_OWNERS_INC

#include "garnet_argument_summary.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct {
    uint64_t next_capture_serial;
    uint64_t published_epoch;
} rdna4_argument_summary_owner_state;

static inline rdna4_argument_summary_status
rdna4_argument_summary_owner_next_capture(
    rdna4_argument_summary_owner_state *state, uint64_t *capture_serial) {
    if (!state || !capture_serial ||
        state->next_capture_serial == UINT64_MAX)
        return RDNA4_ARGUMENT_SUMMARY_OVERFLOW;
    *capture_serial = ++state->next_capture_serial;
    return RDNA4_ARGUMENT_SUMMARY_OK;
}

/*
 * A segment epoch is assigned only after its submission is accepted. A later
 * completed writer invalidates an older summary even when the older callback
 * runs last. Encoded-only writers do not advance last_completed_epoch.
 */
static inline bool rdna4_argument_summary_owner_publish_admissible(
    uint64_t accepted_epoch, uint64_t latest_summary_epoch,
    uint64_t last_completed_epoch, uint64_t capture_host_generation,
    uint64_t current_host_generation, bool host_generation_saturated) {
    return accepted_epoch && accepted_epoch != UINT64_MAX &&
        accepted_epoch > latest_summary_epoch &&
        last_completed_epoch <= accepted_epoch &&
        capture_host_generation != UINT64_MAX &&
        !host_generation_saturated &&
        capture_host_generation == current_host_generation;
}

static inline bool rdna4_argument_summary_owner_completed_prunable(
    uint64_t write_epoch, uint64_t accepted_epoch,
    uint64_t last_completed_epoch, bool all_candidates_captured,
    bool ordered_before_summary) {
    return all_candidates_captured && ordered_before_summary && write_epoch &&
        write_epoch <= accepted_epoch &&
        last_completed_epoch <= accepted_epoch;
}

/*
 * Capture-time completion already happened before the summary was encoded.
 * A writer that was still active at capture is also ordered before this
 * summary only when it belongs to this segment and carries the segment's
 * accepted submission epoch; encode order alone proves nothing across queues.
 */
static inline bool rdna4_argument_summary_owner_ordered_before_summary(
    bool completed_at_capture, bool in_segment,
    uint64_t write_epoch, uint64_t summary_epoch) {
    return completed_at_capture ||
        (in_segment && write_epoch && write_epoch == summary_epoch);
}

/*
 * A stale/obsolete generation is a normal no-publish result and leaves the
 * previous owners untouched for a later summary. Every other unexpected
 * completion/publish status is a bounded failure; overflow, invalid metadata
 * and incomplete jobs must never be silently treated as optional work.
 */
static inline bool rdna4_argument_summary_owner_status_needs_error(
    rdna4_argument_summary_status status,
    rdna4_argument_summary_status expected) {
    return status != expected &&
        status != RDNA4_ARGUMENT_SUMMARY_STALE &&
        status != RDNA4_ARGUMENT_SUMMARY_OBSOLETE;
}

static inline size_t rdna4_argument_summary_owner_candidate_index(
    const uint64_t *tokens, size_t count, uint64_t token) {
    size_t begin = 0, end = count;
    while (begin < end) {
        size_t middle = begin + (end - begin) / 2u;
        if (tokens[middle] < token)
            begin = middle + 1u;
        else
            end = middle;
    }
    return begin;
}

static inline bool rdna4_argument_summary_owner_candidate_found(
    const uint32_t *output_words, size_t output_word_count,
    size_t candidate_index) {
    if (!output_words) return false;
    size_t word = candidate_index / 32u;
    return word < output_word_count &&
        (output_words[word] & (UINT32_C(1) << (candidate_index % 32u))) != 0;
}

static inline __attribute__((unused)) bool rdna4_argument_summary_owner_token_found(
    const uint64_t *tokens, size_t candidate_count,
    const uint32_t *output_words, size_t output_word_count,
    uint64_t token) {
    if (!tokens) return false;
    size_t index = rdna4_argument_summary_owner_candidate_index(
        tokens, candidate_count, token);
    return index < candidate_count && tokens[index] == token &&
        rdna4_argument_summary_owner_candidate_found(
            output_words, output_word_count, index);
}

static inline bool rdna4_argument_summary_owner_token_captured(
    const uint64_t *tokens, size_t candidate_count, uint64_t token) {
    if (!tokens || !token) return false;
    size_t index = rdna4_argument_summary_owner_candidate_index(
        tokens, candidate_count, token);
    return index < candidate_count && tokens[index] == token;
}

static inline bool rdna4_argument_summary_owner_exact_range_retained(
    bool token_captured) {
    /*
     * The GPU result has no offsets. A captured candidate's old slot or range
     * cannot prove where its found token now lives; only uncaptured owners are
     * exact enough to retain.
     */
    return !token_captured;
}

/*
 * A Private transfer by itself is not summary work. The GPU pass is useful
 * only when the transfer can add, remove or refine sampler ownership state.
 */
static inline bool rdna4_argument_summary_owner_state_needed(
    bool private_target, bool incoming_candidates, bool slot_candidates,
    bool possible_candidates, bool active_writer_candidates,
    bool completed_candidates) {
    return private_target &&
        (incoming_candidates || slot_candidates || possible_candidates ||
         active_writer_candidates || completed_candidates);
}

static inline bool rdna4_argument_summary_owner_ranges_overlap(
    uint64_t left_start, uint64_t left_length,
    uint64_t right_start, uint64_t right_length) {
    if (!left_length || !right_length) return false;
    return left_start <= right_start ?
        right_start - left_start < left_length :
        left_start - right_start < right_length;
}

#if !defined(RDNA4_ARGUMENT_SUMMARY_OWNERS_TEST)

@interface RDNA4ArgumentSummaryOwnerState : NSObject
@property uint64_t nextCaptureSerial;
@end
@implementation RDNA4ArgumentSummaryOwnerState
@end

static char rdna4_argument_summary_owner_state_key;
static RDNA4ArgumentSummaryOwnerState *
rdna4_argument_summary_owner_state_for_buffer(id buffer, BOOL create) {
    RDNA4ArgumentSummaryOwnerState *state =
        objc_getAssociatedObject(buffer, &rdna4_argument_summary_owner_state_key);
    if (!state && create) {
        state = [RDNA4ArgumentSummaryOwnerState new];
        objc_setAssociatedObject(buffer, &rdna4_argument_summary_owner_state_key,
                                 state, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }
    return state;
}

@interface RDNA4ArgumentSummaryOwnerCapture : NSObject {
@public
    RDNA4Buffer *buffer;
    RDNA4ArgumentSummaryTask *task;
    RDNA4ArgumentSummaryAttempt *attempt;
    NSArray<NSNumber *> *tokens;
    NSArray *tokenObjects;
    NSSet<NSNumber *> *completedSerials;
    NSSet<NSNumber *> *segmentSerials;
    uint64_t *tokenValues;
    size_t tokenCount;
    uint64_t captureSerial;
    uint64_t hostGeneration;
}
@end
@implementation RDNA4ArgumentSummaryOwnerCapture
- (void)dealloc {
    free(tokenValues);
}
@end

@interface RDNA4ArgumentSummaryOwnerAttempt : NSObject {
@public
    NSArray<RDNA4ArgumentSummaryOwnerCapture *> *captures;
    BOOL finished;
}
- (BOOL)rdna4FinishWithSubmissionEpoch:(uint64_t)submissionEpoch
                        fenceSucceeded:(BOOL)fenceSucceeded
                                 error:(NSError **)error;
@end

static NSError *rdna4_argument_summary_owner_error(
    NSString *message) {
    return rdna4_err(68, [NSString stringWithFormat:
        @"argument summary owner: %@", message]);
}

static NSError *rdna4_argument_summary_owner_status_error(
    NSString *phase, rdna4_argument_summary_status status) {
    return rdna4_argument_summary_owner_error(
        [NSString stringWithFormat:@"%@ returned status %d", phase,
                                   (int)status]);
}

static uint64_t rdna4_argument_summary_owner_token_for_object(
    id device, id object) {
    if (!object) return 0;
    id identity = rdna4_argument_token_identity(object, NO);
    if (!identity) return 0;
    rdna4_argument_tokens_init();
    uint64_t token = 0;
    [rdna4_argument_tokens_lock lock];
    rdna4_argument_token_prune_locked();
    for (NSNumber *key in rdna4_argument_tokens) {
        RDNA4ArgumentResourceToken *entry = rdna4_argument_tokens[key];
        if (entry.texture) continue;
        if (entry.identity == identity && entry.device == device) {
            token = key.unsignedLongLongValue;
            break;
        }
    }
    [rdna4_argument_tokens_lock unlock];
    return token;
}

static BOOL rdna4_argument_summary_owner_add_candidate(
    NSMutableDictionary<NSNumber *, id> *candidates, uint64_t token,
    id object) {
    if (!object || (token >> 48u) != RDNA4_ARGUMENT_SUMMARY_TOKEN_PREFIX)
        return NO;
    NSNumber *key = @(token);
    id previous = candidates[key];
    if (!previous) candidates[key] = object;
    return YES;
}

/*
 * Read one intent and the persistent owners under the destination buffer's
 * lock. Only candidate-bearing state that can affect this intent's envelope is
 * relevant; unrelated transfer records do not turn ordinary buffer traffic
 * into summary work.
 */
static BOOL rdna4_argument_summary_owner_single_intent_needed(
    RDNA4ArgumentWriteIntent *intent) {
    RDNA4Buffer *buffer = intent ? intent->buffer : nil;
    if (!buffer || [buffer storageMode] != MTLStorageModePrivate) return NO;
    BOOL needed = NO;
    @synchronized (buffer) {
        RDNA4ArgumentSamplerOwners *owners =
            rdna4_argument_sampler_owners(buffer, NO);
        if (!owners) return NO;
        bool incoming = intent->candidates.count != 0;
        bool slots = false, possible = false;
        bool writers = false, completed = false;
        NSRange range = intent->range;
        for (NSNumber *key in owners.slots) {
            uint64_t offset = key.unsignedLongLongValue;
            if (rdna4_argument_summary_owner_ranges_overlap(
                    offset, sizeof(uint64_t), range.location, range.length)) {
                slots = true;
                break;
            }
        }
        for (NSValue *key in owners.possible) {
            NSRange candidateRange = key.rangeValue;
            if ([owners.possible[key] count] &&
                rdna4_argument_summary_owner_ranges_overlap(
                    candidateRange.location, candidateRange.length,
                    range.location, range.length)) {
                possible = true;
                break;
            }
        }
        for (RDNA4ArgumentWriteIntent *writer in owners.writers) {
            if (!writer->active || !writer->candidates.count ||
                !rdna4_argument_summary_owner_ranges_overlap(
                    writer->range.location, writer->range.length,
                    range.location, range.length))
                continue;
            writers = true;
            break;
        }
        for (RDNA4ArgumentCompletedWrite *write in owners.completed) {
            if (!write->candidates.count ||
                !rdna4_argument_summary_owner_ranges_overlap(
                    write->range.location, write->range.length,
                    range.location, range.length))
                continue;
            completed = true;
            break;
        }
        needed = rdna4_argument_summary_owner_state_needed(
            true, incoming, slots, possible, writers, completed);
    }
    return needed;
}

GARNET_INTERNAL BOOL rdna4_argument_summary_owners_needed(NSArray *intents) {
    if (![intents isKindOfClass:[NSArray class]]) return NO;
    for (id token in intents) {
        if (![token isKindOfClass:
                RDNA4ArgumentWriteIntent.class])
            continue;
        if (rdna4_argument_summary_owner_single_intent_needed(token))
            return YES;
    }
    return NO;
}

/*
 * Called with the buffer lock held. The dictionary covers the complete
 * persistent buffer, not only the ranges selected by this dispatch.
 */
static BOOL rdna4_argument_summary_owner_snapshot_locked(
    RDNA4Buffer *buffer,
    NSArray<NSNumber *> * __strong *outTokens,
    NSArray * __strong *outObjects,
    NSSet<NSNumber *> * __strong *outCompletedSerials,
    uint64_t *outHostGeneration, uint64_t *outCaptureSerial,
    NSError **error) {
    RDNA4ArgumentSamplerOwners *owners =
        rdna4_argument_sampler_owners(buffer, NO);
    if (!owners) {
        if (error) *error = rdna4_argument_summary_owner_error(
            @"write intent has no sampler-owner state");
        return NO;
    }
    if (owners.hostGenerationSaturated ||
        owners.hostGeneration == UINT64_MAX) {
        if (error) *error = rdna4_argument_summary_owner_error(
            @"host generation is saturated");
        return NO;
    }
    RDNA4ArgumentSummaryOwnerState *state =
        rdna4_argument_summary_owner_state_for_buffer(buffer, YES);
    rdna4_argument_summary_owner_state pure = {
        .next_capture_serial = state.nextCaptureSerial,
        .published_epoch = owners.latestSummaryEpoch,
    };
    uint64_t serial = 0;
    if (rdna4_argument_summary_owner_next_capture(
            &pure, &serial) != RDNA4_ARGUMENT_SUMMARY_OK) {
        if (error) *error = rdna4_argument_summary_owner_error(
            @"capture serial is exhausted");
        return NO;
    }

    NSMutableDictionary<NSNumber *, id> *candidates =
        [NSMutableDictionary dictionary];
    for (NSNumber *key in owners.slots) {
        NSArray *slot = owners.slots[key];
        if (slot.count != 2 ||
            !rdna4_argument_summary_owner_add_candidate(
                candidates, [slot[0] unsignedLongLongValue], slot[1])) {
            if (error) *error = rdna4_argument_summary_owner_error(
                @"persistent slot has an invalid token or sampler");
            return NO;
        }
    }
    /*
     * Owner ranges carry sampler objects but not their tokens. Resolve the
     * stable token once, from the same identity registry used by the setters.
     */
    for (NSValue *key in owners.possible) {
        for (id object in owners.possible[key]) {
            uint64_t token = rdna4_argument_summary_owner_token_for_object(
                buffer.device, object);
            if (!token ||
                !rdna4_argument_summary_owner_add_candidate(
                    candidates, token, object)) {
                if (error) *error = rdna4_argument_summary_owner_error(
                    @"possible sampler has no stable token");
                return NO;
            }
        }
    }
    for (RDNA4ArgumentWriteIntent *intent in owners.writers) {
        if (!intent->active) continue;
        for (id object in intent->candidates) {
            uint64_t token = rdna4_argument_summary_owner_token_for_object(
                buffer.device, object);
            if (!token ||
                !rdna4_argument_summary_owner_add_candidate(
                    candidates, token, object)) {
                if (error) *error = rdna4_argument_summary_owner_error(
                    @"pending writer sampler has no stable token");
                return NO;
            }
        }
    }
    for (RDNA4ArgumentCompletedWrite *write in owners.completed) {
        for (id object in write->candidates) {
            uint64_t token = rdna4_argument_summary_owner_token_for_object(
                buffer.device, object);
            if (!token ||
                !rdna4_argument_summary_owner_add_candidate(
                    candidates, token, object)) {
                if (error) *error = rdna4_argument_summary_owner_error(
                    @"completed writer sampler has no stable token");
                return NO;
            }
        }
    }
    NSArray<NSNumber *> *ordered = [candidates.allKeys
        sortedArrayUsingComparator:^NSComparisonResult(NSNumber *left, NSNumber *right) {
            uint64_t a = left.unsignedLongLongValue;
            uint64_t b = right.unsignedLongLongValue;
            return a < b ? NSOrderedAscending :
                a > b ? NSOrderedDescending : NSOrderedSame;
        }];
    NSMutableArray *objects = [NSMutableArray arrayWithCapacity:ordered.count];
    for (NSNumber *key in ordered) [objects addObject:candidates[key]];
    NSMutableSet<NSNumber *> *completedSerials = [NSMutableSet set];
    for (RDNA4ArgumentCompletedWrite *write in owners.completed)
        [completedSerials addObject:@(write->serial)];

    state.nextCaptureSerial = pure.next_capture_serial;
    *outTokens = ordered;
    *outObjects = objects;
    *outCompletedSerials = completedSerials;
    *outHostGeneration = owners.hostGeneration;
    *outCaptureSerial = serial;
    return YES;
}

static RDNA4ArgumentSummaryOwnerCapture *
rdna4_argument_summary_owner_capture(
    RDNA4Buffer *buffer, NSSet<NSNumber *> *segmentSerials,
    VkCommandBuffer command, NSError **error) {
    NSArray<NSNumber *> *tokens = nil;
    NSArray *objects = nil;
    NSSet<NSNumber *> *completedSerials = nil;
    uint64_t hostGeneration = 0, captureSerial = 0;
    @synchronized (buffer) {
        if (!rdna4_argument_summary_owner_snapshot_locked(
                buffer, &tokens, &objects, &completedSerials,
                &hostGeneration, &captureSerial, error))
            return nil;
    }
    if (tokens.count > UINT32_MAX) {
        if (error) *error = rdna4_argument_summary_owner_error(
            @"candidate dictionary exceeds 32-bit indexing");
        return nil;
    }
    NSUInteger dictionaryBytes =
        tokens.count * sizeof(rdna4_argument_summary_candidate);
    if (tokens.count &&
        dictionaryBytes / sizeof(rdna4_argument_summary_candidate) !=
            tokens.count) {
        if (error) *error = rdna4_argument_summary_owner_error(
            @"candidate dictionary size overflows");
        return nil;
    }
    NSError *allocationError = nil;
    RDNA4Buffer *dictionary = nil;
    if (dictionaryBytes) {
        dictionary = [[RDNA4Buffer alloc]
            initWithDevice:buffer.device length:dictionaryBytes
                   options:MTLResourceStorageModeShared
                     error:&allocationError];
        if (!dictionary) {
            if (error) *error = allocationError ?:
                rdna4_argument_summary_owner_error(
                    @"candidate dictionary allocation failed");
            return nil;
        }
        rdna4_argument_summary_candidate *entries =
            (rdna4_argument_summary_candidate *)dictionary.contents;
        for (NSUInteger index = 0; index < tokens.count; ++index) {
            uint64_t token = tokens[index].unsignedLongLongValue;
            entries[index] = (rdna4_argument_summary_candidate){
                .lo = (uint32_t)token,
                .hi = (uint32_t)(token >> 32u),
            };
        }
    }
    uint32_t outputWords = rdna4_argument_summary_output_words(
        (uint32_t)tokens.count);
    NSUInteger outputBytes =
        outputWords ? (NSUInteger)outputWords * sizeof(uint32_t) :
        sizeof(uint32_t);
    RDNA4Buffer *output = [[RDNA4Buffer alloc]
        initWithDevice:buffer.device length:outputBytes
               options:MTLResourceStorageModeShared
                 error:&allocationError];
    if (!output) {
        if (error) *error = allocationError ?:
            rdna4_argument_summary_owner_error(
                @"summary output allocation failed");
        return nil;
    }
    RDNA4ArgumentSummaryTask *task = rdna4_argument_summary_task_new(
        buffer, 0, buffer.length, dictionary, 0,
        (uint32_t)tokens.count, output, 0, captureSerial, NULL, 0, 0,
        &allocationError);
    if (!task) {
        if (error) *error = allocationError ?:
            rdna4_argument_summary_owner_error(
                @"summary task creation failed");
        return nil;
    }
    RDNA4ArgumentSummaryAttempt *attempt = nil;
    if (!rdna4_argument_summary_encode(task, command, &attempt,
                                       &allocationError)) {
        if (error) *error = allocationError ?:
            rdna4_argument_summary_owner_error(
                @"summary recording failed; abort the segment");
        return nil;
    }
    RDNA4ArgumentSummaryOwnerCapture *capture =
        [RDNA4ArgumentSummaryOwnerCapture new];
    capture->tokenValues = calloc(tokens.count, sizeof(uint64_t));
    if (tokens.count && !capture->tokenValues) {
        if (error) *error = rdna4_argument_summary_owner_error(
            @"token index allocation failed");
        return nil;
    }
    for (NSUInteger index = 0; index < tokens.count; ++index)
        capture->tokenValues[index] = tokens[index].unsignedLongLongValue;
    capture->tokenCount = tokens.count;
    capture->buffer = buffer;
    capture->task = task;
    capture->attempt = attempt;
    capture->tokens = tokens;
    capture->tokenObjects = objects;
    capture->completedSerials = completedSerials;
    capture->segmentSerials = [segmentSerials copy];
    capture->captureSerial = captureSerial;
    capture->hostGeneration = hostGeneration;
    return capture;
}

static BOOL rdna4_argument_summary_owner_apply(
    RDNA4ArgumentSummaryOwnerCapture *capture, uint64_t submissionEpoch,
    NSError **error) {
    RDNA4Buffer *buffer = capture->buffer;
    @synchronized (buffer) {
        RDNA4ArgumentSamplerOwners *owners =
            rdna4_argument_sampler_owners(buffer, NO);
        if (!owners) return YES;
        if (!rdna4_argument_summary_owner_publish_admissible(
                submissionEpoch, owners.latestSummaryEpoch,
                owners.lastCompletedEpoch, capture->hostGeneration,
                owners.hostGeneration,
                owners.hostGenerationSaturated != NO))
            return YES;

        uint32_t wordCount = rdna4_argument_summary_output_words(
            (uint32_t)capture->tokens.count);
        NSUInteger outputBytes =
            (NSUInteger)wordCount * sizeof(uint32_t);
        RDNA4Buffer *summaryOutput = capture->task.outputBuffer;
        if (!summaryOutput ||
            outputBytes > [(id<MTLBuffer>)summaryOutput length] ||
            (outputBytes && !summaryOutput.contents)) {
            if (error) *error = rdna4_argument_summary_owner_error(
                @"summary output is unavailable");
            return NO;
        }
        const uint32_t *words = (const uint32_t *)summaryOutput.contents;
        NSArray<NSNumber *> *tokens = capture->tokens;
        NSArray *objects = capture->tokenObjects;
        NSMutableSet *foundObjects = [NSMutableSet set];
        for (NSUInteger index = 0; index < tokens.count; ++index) {
            if (rdna4_argument_summary_owner_candidate_found(
                    words, wordCount, index))
                [foundObjects addObject:objects[index]];
        }
        /*
         * The output is a whole-buffer membership bitset, not an offset map.
         * Every covered exact range/possible set therefore loses its old
         * placement; only found candidates survive and they are re-added as
         * whole-buffer possible. Owners absent from this capture are retained.
         */
        for (NSNumber *key in owners.slots.allKeys) {
            NSArray *slot = owners.slots[key];
            uint64_t token = slot.count ? [slot[0] unsignedLongLongValue] : 0;
            if (!slot.count ||
                !rdna4_argument_summary_owner_exact_range_retained(
                    rdna4_argument_summary_owner_token_captured(
                        capture->tokenValues, capture->tokenCount, token)))
                [owners.slots removeObjectForKey:key];
        }
        for (NSValue *key in owners.possible.allKeys) {
            NSMutableSet *retained = [NSMutableSet set];
            for (id object in owners.possible[key]) {
                uint64_t token =
                    rdna4_argument_summary_owner_token_for_object(
                        buffer.device, object);
                if (!token ||
                    rdna4_argument_summary_owner_exact_range_retained(
                        rdna4_argument_summary_owner_token_captured(
                            capture->tokenValues, capture->tokenCount, token)))
                    [retained addObject:object];
            }
            if (retained.count) {
                owners.possible[key] = retained;
            } else {
                [owners.possible removeObjectForKey:key];
            }
        }
        if (foundObjects.count)
            rdna4_argument_sampler_add_possible_locked(
                owners, NSMakeRange(0, buffer.length), foundObjects);

        if (owners.lastCompletedEpoch <= submissionEpoch) {
            NSMutableArray *drop = [NSMutableArray array];
            for (RDNA4ArgumentCompletedWrite *write in owners.completed) {
                BOOL allCaptured = YES;
                for (id object in write->candidates) {
                    uint64_t token =
                        rdna4_argument_summary_owner_token_for_object(
                            buffer.device, object);
                    if (!token ||
                        !rdna4_argument_summary_owner_token_captured(
                            capture->tokenValues, capture->tokenCount,
                            token)) {
                        allCaptured = NO;
                        break;
                    }
                }
                BOOL orderedBeforeSummary =
                    rdna4_argument_summary_owner_ordered_before_summary(
                        [capture->completedSerials containsObject:
                            @(write->serial)],
                        [capture->segmentSerials containsObject:
                            @(write->serial)],
                        write->submissionEpoch, submissionEpoch);
                if (rdna4_argument_summary_owner_completed_prunable(
                        write->submissionEpoch, submissionEpoch,
                        owners.lastCompletedEpoch, allCaptured,
                        orderedBeforeSummary))
                    [drop addObject:write];
            }
            [owners.completed removeObjectsInArray:drop];
        }
        owners.latestSummaryEpoch = submissionEpoch;
        return YES;
    }
}

@implementation RDNA4ArgumentSummaryOwnerAttempt
- (BOOL)rdna4FinishWithSubmissionEpoch:(uint64_t)submissionEpoch
                        fenceSucceeded:(BOOL)fenceSucceeded
                                 error:(NSError **)error {
    if (finished) return YES;
    finished = YES;
    if (!fenceSucceeded) {
        captures = @[];
        return YES;
    }
    BOOL succeeded = YES;
    for (RDNA4ArgumentSummaryOwnerCapture *capture in captures) {
        rdna4_argument_summary_status status =
            rdna4_argument_summary_attempt_complete(
                capture->attempt, YES, capture->captureSerial);
        if (status != RDNA4_ARGUMENT_SUMMARY_COMPLETE) {
            if (!rdna4_argument_summary_owner_status_needs_error(
                    status, RDNA4_ARGUMENT_SUMMARY_COMPLETE))
                continue;
            if (error) *error = rdna4_argument_summary_owner_status_error(
                @"summary completion", status);
            succeeded = NO;
            break;
        }
        rdna4_argument_summary_publication publication = {0};
        status = rdna4_argument_summary_attempt_publish(
            capture->attempt, capture->captureSerial, &publication);
        if (status != RDNA4_ARGUMENT_SUMMARY_READY) {
            if (!rdna4_argument_summary_owner_status_needs_error(
                    status, RDNA4_ARGUMENT_SUMMARY_READY))
                continue;
            if (error) *error = rdna4_argument_summary_owner_status_error(
                @"summary publication", status);
            succeeded = NO;
            break;
        }
        if (!rdna4_argument_summary_owner_apply(
                capture, submissionEpoch, error)) {
            succeeded = NO;
            break;
        }
    }
    /*
     * Finish is the final reader of every capture. Releasing the complete
     * dictionary/task/output graph here avoids extending its lifetime through
     * an autoreleased owner-attempt wrapper after the segment clears its strong
     * attempt array.
     */
    captures = @[];
    return succeeded;
}
@end

GARNET_INTERNAL BOOL rdna4_argument_summary_owners_finish(
    id attempt, BOOL fenceSucceeded, uint64_t submissionEpoch,
    NSError **error) {
    if (!attempt) return YES;
    if (![attempt isKindOfClass:
            RDNA4ArgumentSummaryOwnerAttempt.class]) {
        if (error) *error = rdna4_argument_summary_owner_error(
            @"finish received a non-summary owner attempt");
        return NO;
    }
    return [(RDNA4ArgumentSummaryOwnerAttempt *)attempt
        rdna4FinishWithSubmissionEpoch:submissionEpoch
                        fenceSucceeded:fenceSucceeded
                                 error:error];
}

GARNET_INTERNAL BOOL rdna4_argument_summary_owners_encode(
    NSArray<RDNA4ArgumentWriteIntent *> *intents,
    VkCommandBuffer command,
    NSArray * __strong *outAttempts,
    NSError **error) {
    if (outAttempts) *outAttempts = @[];
    if (!command) {
        if (error) *error = rdna4_argument_summary_owner_error(
            @"encode needs a command buffer");
        return NO;
    }
    NSMutableArray *attempts =
        [NSMutableArray array];
    NSMutableArray<RDNA4ArgumentSummaryOwnerCapture *> *created =
        [NSMutableArray array];
    NSMutableArray<RDNA4Buffer *> *buffers = [NSMutableArray array];
    NSMutableArray<NSMutableArray<RDNA4ArgumentWriteIntent *> *> *groups =
        [NSMutableArray array];
    for (RDNA4ArgumentWriteIntent *intent in intents) {
        RDNA4Buffer *buffer = intent ? intent->buffer : nil;
        if (!buffer) {
            if (error) *error = rdna4_argument_summary_owner_error(
                @"write intent lost its destination buffer");
            return NO;
        }
        if (!intent->serial) {
            if (error) *error = rdna4_argument_summary_owner_error(
                @"write intent has no serial");
            return NO;
        }
        if ([buffer storageMode] != MTLStorageModePrivate) continue;
        NSUInteger index = [buffers indexOfObjectIdenticalTo:buffer];
        if (index == NSNotFound) {
            [buffers addObject:buffer];
            [groups addObject:[NSMutableArray array]];
            index = buffers.count - 1u;
        }
        [groups[index] addObject:intent];
    }
    for (NSUInteger index = 0; index < buffers.count; ++index) {
        RDNA4Buffer *buffer = buffers[index];
        NSArray<RDNA4ArgumentWriteIntent *> *group = groups[index];
        if (!rdna4_argument_summary_owners_needed(group)) continue;
        NSMutableSet<NSNumber *> *segmentSerials = [NSMutableSet set];
        for (RDNA4ArgumentWriteIntent *intent in group)
            [segmentSerials addObject:@(intent->serial)];
        RDNA4ArgumentSummaryOwnerCapture *capture =
            rdna4_argument_summary_owner_capture(
                buffer, segmentSerials, command, error);
        if (!capture) {
            /*
             * Some summary commands may already be recorded. The caller must
             * abort this segment; no owner is published before its fence.
             */
            return NO;
        }
        [created addObject:capture];
    }
    if (!created.count) return YES;
    for (RDNA4ArgumentSummaryOwnerCapture *capture in created) {
        RDNA4ArgumentSummaryOwnerAttempt *attempt =
            [RDNA4ArgumentSummaryOwnerAttempt new];
        attempt->captures = @[capture];
        [attempts addObject:attempt];
    }
    if (outAttempts) *outAttempts = attempts;
    return YES;
}

#endif

#endif
#endif // GARNET_ARGUMENT_SUMMARY_OWNERS_ONLY
