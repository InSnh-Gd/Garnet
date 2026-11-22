- (BOOL)rdna4PrepareArguments:(const RDNA4ArgumentTables *)tables resources:(NSSet *)resources writable:(NSSet *)writable scan:(const RDNA4ArgumentScanPrepared *)scan textures:(void (^)(RDNA4Texture *, VkImageLayout, BOOL))prepare {
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
    [self rdna4RetainArgument:bufferSnapshot];
    _bufferAddressResources = bufferSnapshot;
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
                BOOL storage = [texture rdna4TexelWritable];
                unsigned category = storage ? 1u : 0u;
                if (tables->dynamicTexel[category])
                    [texelRows[category] addObject:row];
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
                if (![texture rdna4IsTexel] ||
                    [texture rdna4TexelWritable] != (texel != 0) ||
                    !(views[index] = [texture rdna4TexelView])) {
                    valid = NO;
                    break;
                }
                [self rdna4RetainArgument:texture];
                prepare(texture, VK_IMAGE_LAYOUT_GENERAL,
                        [texture rdna4TexelWritable]);
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
