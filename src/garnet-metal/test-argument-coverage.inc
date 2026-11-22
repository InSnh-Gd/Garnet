// A8 pins valid layouts to a7-ref.log. Invalid mixed-type ranges remain
// observations: Apple's captured calls wrote before the bound argument region.
static const char *const argumentCoverageCases[] = {
    "bulk.texture_sampler", "bulk.texture_buffer", "bulk.texture_gap", "bulk.sampler_buffer",
    "nested.pointer_array", "nested.inline_array", "nested.wrapper_array", "stride.padded",
    "metadata.1d", "metadata.cube", "metadata.2d_array", "metadata.msaa", "metadata.read_write",
    "descriptor.1d", "descriptor.cube", "descriptor.2d_array", "descriptor.msaa", "descriptor.read_write"
};

typedef struct { NSUInteger index; MTLDataType type; NSUInteger offset; } CoverageSlot;
typedef struct {
    const char *name;
    NSUInteger length, alignment;
    const CoverageSlot *slots;
    NSUInteger count;
} CoverageLayout;

static const CoverageSlot coverageMixed[] = {
    {0,MTLDataTypeTexture,0}, {1,MTLDataTypeSampler,8}, {2,MTLDataTypePointer,16},
    {4,MTLDataTypeTexture,24}, {5,MTLDataTypeTexture,32}
};
static const CoverageSlot coverageMixedBuffer[] = {
    {0,MTLDataTypeTexture,0}, {1,MTLDataTypePointer,8}, {2,MTLDataTypeSampler,16},
    {4,MTLDataTypeTexture,24}, {5,MTLDataTypeTexture,32}
};
static const CoverageSlot coverageParent[] = {{3,MTLDataTypePointer,0}, {12,MTLDataTypePointer,8}};
static const CoverageSlot coverageChild[] = {
    {1,MTLDataTypeTexture,0}, {2,MTLDataTypeTexture,8}, {4,MTLDataTypeSampler,16}
};
static const CoverageSlot coverageInline[] = {
    {4,MTLDataTypeTexture,0}, {5,MTLDataTypeTexture,8}, {7,MTLDataTypeSampler,16}, {12,MTLDataTypePointer,24}
};
static const CoverageSlot coverageWrappers[] = {
    {4,MTLDataTypeTexture,0}, {5,MTLDataTypeTexture,8}, {7,MTLDataTypeSampler,16},
    {9,MTLDataTypeTexture,24}, {10,MTLDataTypeTexture,32}, {12,MTLDataTypeSampler,40}, {20,MTLDataTypePointer,48}
};
static const CoverageSlot coveragePadded[] = {
    {1,MTLDataTypeTexture,0}, {3,MTLDataTypeFloat4,16}, {5,MTLDataTypeUInt,32},
    {6,MTLDataTypeTexture,48}, {8,MTLDataTypeFloat4,64}, {10,MTLDataTypeUInt,80}, {20,MTLDataTypePointer,96}
};
static const CoverageSlot coveragePaddedChild[] = {
    {0,MTLDataTypeTexture,0}, {2,MTLDataTypeFloat4,16}, {4,MTLDataTypeUInt,32}
};
static const CoverageSlot coverageMetadata[] = {
    {1,MTLDataTypeTexture,0}, {3,MTLDataTypeTexture,8}, {4,MTLDataTypeTexture,16}, {7,MTLDataTypeSampler,24}
};
static const CoverageSlot coverageDescriptors[] = {{1,MTLDataTypeTexture,0}, {2,MTLDataTypeTexture,8}};
#define COVERAGE_LAYOUT(name, length, alignment, slots) {name, length, alignment, slots, sizeof slots / sizeof slots[0]}
static const CoverageLayout coverageLayouts[] = {
    COVERAGE_LAYOUT("bulk.texture_sampler",40,8,coverageMixed),
    COVERAGE_LAYOUT("bulk.texture_buffer",40,8,coverageMixedBuffer),
    COVERAGE_LAYOUT("bulk.texture_gap",40,8,coverageMixed),
    COVERAGE_LAYOUT("bulk.sampler_buffer",40,8,coverageMixed),
    COVERAGE_LAYOUT("nested.pointer_array",16,8,coverageParent),
    COVERAGE_LAYOUT("nested.pointer_array.child",24,8,coverageChild),
    COVERAGE_LAYOUT("nested.inline_array",32,8,coverageInline),
    COVERAGE_LAYOUT("nested.wrapper_array",56,8,coverageWrappers),
    COVERAGE_LAYOUT("stride.padded",112,16,coveragePadded),
    COVERAGE_LAYOUT("stride.padded.child",48,16,coveragePaddedChild),
    COVERAGE_LAYOUT("metadata.1d",32,8,coverageMetadata),
    COVERAGE_LAYOUT("metadata.cube",32,8,coverageMetadata),
    COVERAGE_LAYOUT("metadata.2d_array",32,8,coverageMetadata),
    COVERAGE_LAYOUT("metadata.msaa",32,8,coverageMetadata),
    COVERAGE_LAYOUT("metadata.read_write",32,8,coverageMetadata),
    COVERAGE_LAYOUT("descriptor.1d",16,8,coverageDescriptors),
    COVERAGE_LAYOUT("descriptor.cube",16,8,coverageDescriptors),
    COVERAGE_LAYOUT("descriptor.2d_array",16,8,coverageDescriptors),
    COVERAGE_LAYOUT("descriptor.msaa",16,8,coverageDescriptors),
    COVERAGE_LAYOUT("descriptor.read_write",16,8,coverageDescriptors)
};
#undef COVERAGE_LAYOUT

static const CoverageLayout *coverageLayout(NSString *name) {
    for (NSUInteger n = 0; n < sizeof coverageLayouts / sizeof coverageLayouts[0]; ++n)
        if (!strcmp(name.UTF8String, coverageLayouts[n].name)) return &coverageLayouts[n];
    require(NO, [@"missing Apple oracle: " stringByAppendingString:name]);
    return NULL;
}

static void coverageCheckLayout(NSString *name, id<MTLArgumentEncoder> encoder) {
    const CoverageLayout *expected = coverageLayout(name);
    require(encoder && encoder.encodedLength == expected->length && encoder.alignment == expected->alignment,
        [@"A8 layout differs from Apple: " stringByAppendingString:name]);
}

static void coverageExpectedTexture(NSString *path, NSUInteger *type, NSUInteger *access) {
    // Values measured on Apple, including read_write access == 1.
    *type = 2; *access = 0;
    if ([path hasPrefix:@"metadata.1d"] || [path hasPrefix:@"descriptor.1d"]) *type = 0;
    else if ([path hasPrefix:@"metadata.cube"] || [path hasPrefix:@"descriptor.cube"]) *type = 5;
    else if ([path hasPrefix:@"metadata.2d_array"] || [path hasPrefix:@"descriptor.2d_array"]) *type = 3;
    else if ([path hasPrefix:@"metadata.msaa"] || [path hasPrefix:@"descriptor.msaa"]) *type = 4;
    else if ([path hasPrefix:@"metadata.read_write"] || [path hasPrefix:@"descriptor.read_write"]) *access = 1;
}

static NSArray<NSDictionary *> *coverageOracleSlots(NSString *name, NSArray<NSDictionary *> *reflected) {
    const CoverageLayout *expected = coverageLayout(name);
    require(!reflected.count || reflected.count == expected->count, @"A8 reflected slot count differs from Apple");
    NSMutableArray<NSDictionary *> *result = [NSMutableArray array];
    NSUInteger textureType = 0, access = 0;
    coverageExpectedTexture(name, &textureType, &access);
    for (NSUInteger n = 0; n < expected->count; ++n) {
        CoverageSlot slot = expected->slots[n];
        NSMutableDictionary *entry = [@{@"index":@(slot.index), @"type":@(slot.type),
            @"expected_offset":@(slot.offset), @"path":[name stringByAppendingFormat:@".id_%lu", (unsigned long)slot.index],
            @"texture_type":@(textureType), @"access":@(access)} mutableCopy];
        BOOL found = NO;
        for (NSDictionary *value in reflected) if ([value[@"index"] unsignedIntegerValue] == slot.index) {
            require(!found && [value[@"type"] unsignedIntegerValue] == slot.type, @"A8 reflected ID/type differs from Apple");
            found = YES;
            entry[@"path"] = value[@"path"];
            if (value[@"pointer"]) entry[@"pointer"] = value[@"pointer"];
        }
        require(!reflected.count || found, @"A8 missing reflected argument ID");
        [result addObject:entry];
    }
    return result;
}

static void coverageCheckBytes(NSString *name, id<MTLBuffer> storage, NSUInteger offset, NSUInteger size, BOOL zero) {
    require(offset <= storage.length && size <= storage.length - offset, @"A8 oracle span exceeds storage");
    const uint8_t *bytes = storage.contents;
    for (NSUInteger n = 0; n < storage.length; ++n)
        if (n < offset || n - offset >= size)
            require(bytes[n] == 0xa7, [@"A8 write outside Apple slot: " stringByAppendingString:name]);
        else if (zero) require(bytes[n] == 0, [@"A8 nil/constant span differs from Apple: " stringByAppendingString:name]);
    if (!zero) {
        uint64_t token = 0;
        require(size == sizeof token, @"A8 resource token width differs from Apple");
        memcpy(&token, bytes + offset, sizeof token);
        require(token != 0 && token != UINT64_C(0xa7a7a7a7a7a7a7a7), @"A8 nonnil resource did not encode a token");
    }
}

static void coverageException(NSString *name, NSString *operation, NSException *exception) {
    printf("ARG7_EXCEPTION case=%s op=%s name=%s reason=%s\n", name.UTF8String, operation.UTF8String,
        exception.name.UTF8String, exception.reason.UTF8String ?: "");
}

static void coverageChanges(NSString *name, NSString *operation, id<MTLBuffer> storage, NSUInteger base) {
    const uint8_t *bytes = storage.contents;
    NSUInteger first = storage.length, last = 0, changed = 0;
    for (NSUInteger i = 0; i < storage.length; ++i) if (bytes[i] != 0xa7) {
        if (first == storage.length) first = i;
        last = i; ++changed;
    }
    printf("ARG7_BYTES case=%s op=%s first=%ld relative=%ld span=%lu changed=%lu\n",
        name.UTF8String, operation.UTF8String, changed ? (long)first : -1L,
        changed ? (long)first - (long)base : -1L,
        (unsigned long)(changed ? last - first + 1 : 0), (unsigned long)changed);
    for (NSUInteger i = 0; i < storage.length;) {
        if (bytes[i] == 0xa7) { ++i; continue; }
        NSUInteger begin = i;
        while (i < storage.length && bytes[i] != 0xa7) ++i;
        printf("ARG7_RUN case=%s op=%s relative=%ld length=%lu hex=", name.UTF8String,
            operation.UTF8String, (long)begin - (long)base, (unsigned long)(i - begin));
        for (NSUInteger j = begin; j < i && j - begin < 16; ++j) printf("%02x", bytes[j]);
        puts(i - begin > 16 ? "..." : "");
    }
}

static void coverageTextureInfo(NSString *path, MTLTextureReferenceType *texture) {
    require(texture != nil, [@"A8 missing texture-reference metadata: " stringByAppendingString:path]);
    // The SDK declares this getter using the deprecated MTLArgumentAccess alias.
    // Read its integer value without naming that alias or suppressing warnings.
    NSUInteger access = ((NSUInteger (*)(id, SEL))objc_msgSend)(texture, @selector(access));
    printf("ARG7_TEXTURE path=%s available=1 texture_type=%lu data_type=%lu access=%lu\n",
        path.UTF8String, (unsigned long)texture.textureType, (unsigned long)texture.textureDataType,
        (unsigned long)access);
    NSUInteger expectedType = 0, expectedAccess = 0;
    coverageExpectedTexture(path, &expectedType, &expectedAccess);
    require(texture.textureType == expectedType && texture.textureDataType == MTLDataTypeFloat && access == expectedAccess,
        [@"A8 texture metadata differs from Apple: " stringByAppendingString:path]);
}

static void coverageType(MTLDataType type, MTLStructType *structure, MTLArrayType *array,
                         MTLPointerType *pointer, MTLTextureReferenceType *texture,
                         NSString *path, NSUInteger offset, NSUInteger index,
                         NSMutableArray<NSDictionary *> *slots, unsigned depth) {
    if (depth > 16 || slots.count >= 128) {
        printf("ARG7_TRUNCATED path=%s\n", path.UTF8String); return;
    }
    if (type == MTLDataTypeStruct) {
        printf("ARG7_STRUCT path=%s available=%d members=%lu\n", path.UTF8String,
            structure != nil, (unsigned long)structure.members.count);
        for (MTLStructMember *member in structure.members) {
            NSString *child = [path stringByAppendingFormat:@".%@", member.name];
            printf("ARG7_MEMBER path=%s data_type=%lu offset=%lu argument_index=%lu\n",
                child.UTF8String, (unsigned long)member.dataType, (unsigned long)member.offset,
                (unsigned long)member.argumentIndex);
            if (member.argumentIndex == NSUIntegerMax || member.offset > NSUIntegerMax - offset ||
                member.argumentIndex > NSUIntegerMax - index) {
                printf("ARG7_INDEX_UNAVAILABLE path=%s\n", child.UTF8String); continue;
            }
            coverageType(member.dataType, member.dataType == MTLDataTypeStruct ? member.structType : nil,
                member.dataType == MTLDataTypeArray ? member.arrayType : nil,
                member.dataType == MTLDataTypePointer ? member.pointerType : nil,
                member.dataType == MTLDataTypeTexture ? member.textureReferenceType : nil,
                child, offset + member.offset, index + member.argumentIndex, slots, depth + 1);
        }
        return;
    }
    if (type == MTLDataTypeArray) {
        printf("ARG7_ARRAY path=%s available=%d count=%lu stride=%lu argument_index_stride=%lu element_type=%lu\n",
            path.UTF8String, array != nil, (unsigned long)array.arrayLength, (unsigned long)array.stride,
            (unsigned long)array.argumentIndexStride, (unsigned long)array.elementType);
        require(array && array.arrayLength == 2, @"A8 array length differs from Apple");
        if (array.elementType == MTLDataTypeTexture)
            require(array.stride == 0 && array.argumentIndexStride == 1, @"A8 texture-array metadata differs from Apple");
        else if (array.elementType == MTLDataTypeStruct)
            require(array.stride == ([path hasPrefix:@"stride.padded"] ? 48 : 0) && array.argumentIndexStride == 5,
                @"A8 struct-array metadata differs from Apple");
        for (NSUInteger n = 0; n < array.arrayLength && n < 64; ++n) {
            if ((array.stride && n > (NSUIntegerMax - offset) / array.stride) ||
                (array.argumentIndexStride && n > (NSUIntegerMax - index) / array.argumentIndexStride)) break;
            coverageType(array.elementType,
                array.elementType == MTLDataTypeStruct ? array.elementStructType : nil,
                array.elementType == MTLDataTypeArray ? array.elementArrayType : nil,
                array.elementType == MTLDataTypePointer ? array.elementPointerType : nil,
                array.elementType == MTLDataTypeTexture ? array.elementTextureReferenceType : nil,
                [path stringByAppendingFormat:@"[%lu]", (unsigned long)n],
                offset + n * array.stride, index + n * array.argumentIndexStride, slots, depth + 1);
        }
        return;
    }
    NSMutableDictionary *slot = [@{@"index":@(index), @"type":@(type), @"path":path} mutableCopy];
    printf("ARG7_LEAF path=%s data_type=%lu computed_index=%lu absolute_offset=%lu\n",
        path.UTF8String, (unsigned long)type, (unsigned long)index, (unsigned long)offset);
    if (type == MTLDataTypeTexture) {
        coverageTextureInfo(path, texture);
        if (texture) {
            slot[@"texture_type"] = @(texture.textureType);
            slot[@"access"] = @(((NSUInteger (*)(id, SEL))objc_msgSend)(texture, @selector(access)));
        }
    } else if (type == MTLDataTypePointer && pointer) {
        printf("ARG7_POINTER path=%s element_type=%lu data_size=%lu alignment=%lu argument_buffer=%d\n",
            path.UTF8String, (unsigned long)pointer.elementType, (unsigned long)pointer.dataSize,
            (unsigned long)pointer.alignment, pointer.elementIsArgumentBuffer);
        slot[@"pointer"] = pointer;
    }
    [slots addObject:slot];
}

static id<MTLTexture> coverageTexture(id<MTLDevice> device, NSDictionary *slot) {
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor new];
    descriptor.textureType = slot[@"texture_type"] ? [slot[@"texture_type"] unsignedIntegerValue] : MTLTextureType2D;
    descriptor.pixelFormat = MTLPixelFormatRGBA8Unorm;
    descriptor.width = 4;
    descriptor.height = descriptor.textureType == MTLTextureType1D ? 1 : 4;
    descriptor.arrayLength = descriptor.textureType == MTLTextureType2DArray ? 2 : 1;
    descriptor.usage = MTLTextureUsageShaderRead;
    if ([slot[@"access"] unsignedIntegerValue] != MTLBindingAccessReadOnly)
        descriptor.usage |= MTLTextureUsageShaderWrite;
    if (descriptor.textureType == MTLTextureType2DMultisample) {
        NSUInteger samples = [device supportsTextureSampleCount:4] ? 4 : [device supportsTextureSampleCount:2] ? 2 : 0;
        if (!samples) return nil;
        descriptor.sampleCount = samples;
        descriptor.storageMode = MTLStorageModePrivate;
        descriptor.usage |= MTLTextureUsageRenderTarget;
    }
    return [device newTextureWithDescriptor:descriptor];
}

static void coverageSet(id<MTLArgumentEncoder> encoder, NSDictionary *slot, BOOL nonnil,
                        id<MTLBuffer> buffer, id<MTLTexture> texture, id<MTLSamplerState> sampler) {
    NSUInteger index = [slot[@"index"] unsignedIntegerValue];
    switch ([slot[@"type"] unsignedIntegerValue]) {
    case MTLDataTypeTexture: [encoder setTexture:nonnil ? texture : nil atIndex:index]; break;
    case MTLDataTypeSampler: [encoder setSamplerState:nonnil ? sampler : nil atIndex:index]; break;
    case MTLDataTypePointer: [encoder setBuffer:nonnil ? buffer : nil offset:0 atIndex:index]; break;
    default: {
        void *constant = [encoder constantDataAtIndex:index];
        if (constant) memset(constant, 0, 1);
        break;
    }
    }
}

static void coverageReject(NSString *operation, id<MTLBuffer> storage, void (^call)(void)) {
    memset(storage.contents, 0xa7, storage.length);
    BOOL rejected = NO;
    @try { call(); }
    @catch (NSException *exception) {
        require([exception.name isEqualToString:@"NSInvalidArgumentException"], @"A8 unexpected rejection exception");
        rejected = YES;
    }
    require(rejected, [@"A8 invalid operation accepted: " stringByAppendingString:operation]);
    coverageCheckBytes(operation, storage, 0, 0, YES);
    printf("ARG8_REJECT_PASS op=%s storage_unchanged=1\n", operation.UTF8String);
}

static void coverageNestedBounds(id<MTLDevice> device, NSString *name, id<MTLArgumentEncoder> encoder,
                                 NSUInteger index, NSUInteger needed) {
    id<MTLBuffer> destination = [device newBufferWithLength:encoder.encodedLength + 256 options:MTLResourceStorageModeShared];
    id<MTLBuffer> buffer = [device newBufferWithLength:needed + 32 options:MTLResourceStorageModeShared];
    require(destination.contents && buffer, @"A8 nested bounds resources unavailable");
    [encoder setArgumentBuffer:destination offset:0];
    [encoder setBuffer:buffer offset:buffer.length - needed atIndex:index];
    coverageReject([name stringByAppendingString:@".short_tail"], destination, ^{
        [encoder setBuffer:buffer offset:buffer.length - needed + 1 atIndex:index];
    });
    coverageReject([name stringByAppendingString:@".past_end"], destination, ^{
        [encoder setBuffer:buffer offset:buffer.length atIndex:index];
    });
    coverageReject([name stringByAppendingString:@".overflow_offset"], destination, ^{
        [encoder setBuffer:buffer offset:NSUIntegerMax atIndex:index];
    });
    [encoder setBuffer:nil offset:NSUIntegerMax atIndex:index];
    printf("ARG8_NESTED_BOUNDS_PASS case=%s required=%lu\n", name.UTF8String, (unsigned long)needed);
}

static void coverageBulkObjects(id<MTLDevice> device) {
    id<MTLBuffer> resource = [device newBufferWithLength:256 options:MTLResourceStorageModeShared];
    id<MTLTexture> texture = coverageTexture(device, @{@"texture_type":@2, @"access":@0});
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new]; sd.supportArgumentBuffers = YES;
    id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:sd];
    require(resource && texture && sampler, @"A8 bulk resources unavailable");
    const MTLDataType types[] = {MTLDataTypeTexture, MTLDataTypeSampler, MTLDataTypePointer};
    for (NSUInteger k = 0; k < sizeof types / sizeof types[0]; ++k) {
        MTLArgumentDescriptor *descriptor = [MTLArgumentDescriptor argumentDescriptor];
        descriptor.index = 1; descriptor.arrayLength = 2; descriptor.dataType = types[k];
        descriptor.access = MTLBindingAccessReadOnly;
        if (types[k] == MTLDataTypeTexture) descriptor.textureType = MTLTextureType2D;
        id<MTLArgumentEncoder> encoder = [device newArgumentEncoderWithArguments:@[descriptor]];
        require(encoder != nil && encoder.encodedLength == 16, @"A8 bulk encoder unavailable");
        id<MTLBuffer> storage = [device newBufferWithLength:256 options:MTLResourceStorageModeShared];
        require(storage.contents != NULL, @"A8 bulk destination unavailable");
        [encoder setArgumentBuffer:storage offset:0];
        if (types[k] == MTLDataTypeTexture) coverageReject(@"bulk.late_bad_texture", storage, ^{
            id<MTLTexture> objects[] = {texture, (id)[NSObject new]};
            [encoder setTextures:objects withRange:NSMakeRange(1, 2)];
        });
        else if (types[k] == MTLDataTypeSampler) coverageReject(@"bulk.late_bad_sampler", storage, ^{
            id<MTLSamplerState> objects[] = {sampler, (id)[NSObject new]};
            [encoder setSamplerStates:objects withRange:NSMakeRange(1, 2)];
        });
        else {
            coverageReject(@"bulk.late_bad_buffer", storage, ^{
                id<MTLBuffer> objects[] = {resource, (id)[NSObject new]};
                NSUInteger offsets[] = {0, 0};
                [encoder setBuffers:objects offsets:offsets withRange:NSMakeRange(1, 2)];
            });
            coverageReject(@"bulk.late_bad_offset", storage, ^{
                id<MTLBuffer> objects[] = {resource, resource};
                NSUInteger offsets[] = {0, resource.length + 1};
                [encoder setBuffers:objects offsets:offsets withRange:NSMakeRange(1, 2)];
            });
        }
    }
    puts("ARG8_BULK_OBJECTS_PASS types=texture,sampler,buffer");
}

static void coverageIndependent(id<MTLDevice> device, NSString *name, id<MTLArgumentEncoder> first,
                                 id<MTLArgumentEncoder> second, NSDictionary *slot) {
    coverageCheckLayout(name, first); coverageCheckLayout(name, second);
    require(first != second, @"A8 child encoders share mutable binding state");
    id<MTLBuffer> a = [device newBufferWithLength:256 + first.encodedLength options:MTLResourceStorageModeShared];
    id<MTLBuffer> b = [device newBufferWithLength:256 + second.encodedLength options:MTLResourceStorageModeShared];
    require(a.contents && b.contents, @"A8 child destinations unavailable");
    memset(a.contents, 0xa7, a.length); memset(b.contents, 0xa7, b.length);
    [first setArgumentBuffer:a offset:256]; [second setArgumentBuffer:b offset:256];
    NSUInteger offset = 256 + [slot[@"expected_offset"] unsignedIntegerValue];
    coverageSet(first, slot, NO, nil, nil, nil);
    coverageCheckBytes(name, a, offset, 8, YES); coverageCheckBytes(name, b, 0, 0, YES);
    coverageSet(second, slot, NO, nil, nil, nil);
    coverageCheckBytes(name, a, offset, 8, YES); coverageCheckBytes(name, b, offset, 8, YES);
    printf("ARG8_INDEPENDENT_PASS case=%s\n", name.UTF8String);
}

static void coverageEncoder(id<MTLDevice> device, NSString *name, id<MTLArgumentEncoder> encoder,
                            NSArray<NSDictionary *> *slots, unsigned depth) {
    printf("ARG7_ENCODER case=%s available=%d length=%lu alignment=%lu slots=%lu\n", name.UTF8String,
        encoder != nil, (unsigned long)encoder.encodedLength, (unsigned long)encoder.alignment,
        (unsigned long)slots.count);
    coverageCheckLayout(name, encoder);
    if (depth > 4) return;
    slots = coverageOracleSlots(name, slots);
    NSUInteger length = encoder.encodedLength, alignment = encoder.alignment;
    if (!length || length > (1u << 20) || !alignment || alignment > 4096) {
        printf("ARG7_MEASUREMENT_UNAVAILABLE case=%s reason=layout_safety_limit\n", name.UTF8String); return;
    }
    NSUInteger base = ((256 + alignment - 1) / alignment) * alignment;
    id<MTLBuffer> storage = [device newBufferWithLength:base + length * 4 + 256 options:MTLResourceStorageModeShared];
    id<MTLBuffer> resource = [device newBufferWithLength:256 options:MTLResourceStorageModeShared];
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.supportArgumentBuffers = YES;
    id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:sd];
    if (!storage.contents || !resource) {
        printf("ARG7_MEASUREMENT_UNAVAILABLE case=%s reason=buffer_allocation\n", name.UTF8String);
        require(NO, @"A8 measurement buffers unavailable");
    }
    [encoder setArgumentBuffer:storage offset:base];
    for (NSDictionary *slot in slots) {
        for (unsigned pass = 0; pass < 2; ++pass) {
            NSString *operation = [NSString stringWithFormat:@"%@.%@", slot[@"path"], pass ? @"nonnil" : @"nil"];
            memset(storage.contents, 0xa7, storage.length);
            @try {
                id<MTLTexture> texture = nil;
                if (pass && [slot[@"type"] unsignedIntegerValue] == MTLDataTypeTexture) {
                    texture = coverageTexture(device, slot);
                    if (!texture) {
                        printf("ARG7_RESOURCE_UNAVAILABLE case=%s op=%s\n", name.UTF8String, operation.UTF8String);
                        require(NO, @"A8 captured texture kind unavailable");
                    }
                }
                if (pass && [slot[@"type"] unsignedIntegerValue] == MTLDataTypeSampler && !sampler) {
                    printf("ARG7_RESOURCE_UNAVAILABLE case=%s op=%s\n", name.UTF8String, operation.UTF8String);
                    require(NO, @"A8 captured sampler unavailable");
                }
                NSUInteger type = [slot[@"type"] unsignedIntegerValue];
                if (type != MTLDataTypeTexture && type != MTLDataTypeSampler && type != MTLDataTypePointer) {
                    void *constant = [encoder constantDataAtIndex:[slot[@"index"] unsignedIntegerValue]];
                    printf("ARG7_CONSTANT case=%s path=%s relative=%ld write_bytes=1\n", name.UTF8String,
                        [slot[@"path"] UTF8String], constant ? (long)((uintptr_t)constant - (uintptr_t)storage.contents) - (long)base : -1L);
                    require(constant && (uintptr_t)constant == (uintptr_t)storage.contents + base +
                        [slot[@"expected_offset"] unsignedIntegerValue], @"A8 constant offset differs from Apple");
                }
                coverageSet(encoder, slot, pass != 0, resource, texture, sampler);
                printf("ARG7_RETURN case=%s op=%s\n", name.UTF8String, operation.UTF8String);
            } @catch (NSException *exception) {
                coverageException(name, operation, exception); require(NO, @"A8 valid setter raised an exception");
            }
            coverageChanges(name, operation, storage, base);
            NSUInteger type = [slot[@"type"] unsignedIntegerValue];
            BOOL resourceType = type == MTLDataTypeTexture || type == MTLDataTypeSampler || type == MTLDataTypePointer;
            coverageCheckBytes(name, storage, base + [slot[@"expected_offset"] unsignedIntegerValue],
                resourceType ? 8 : 1, !pass || !resourceType);
        }
    }
    // The captured element placements pin the stride independently of the driver.
    for (NSUInteger element = 0; element < 3; ++element) {
        NSString *operation = [NSString stringWithFormat:@"array_element_%lu", (unsigned long)element];
        memset(storage.contents, 0xa7, storage.length);
        @try {
            [encoder setArgumentBuffer:storage startOffset:base arrayElement:element];
            if (slots.count) coverageSet(encoder, slots[0], NO, nil, nil, nil);
            printf("ARG7_ARRAY_BIND case=%s element=%lu start=%lu returned=1 write_probe=%d\n",
                name.UTF8String, (unsigned long)element, (unsigned long)base, slots.count != 0);
        } @catch (NSException *exception) {
            coverageException(name, operation, exception); require(NO, @"A8 array placement raised an exception");
        }
        coverageChanges(name, operation, storage, base);
        require(slots.count != 0, @"A8 array placement has no write probe");
        NSUInteger type = [slots[0][@"type"] unsignedIntegerValue];
        BOOL resourceType = type == MTLDataTypeTexture || type == MTLDataTypeSampler || type == MTLDataTypePointer;
        coverageCheckBytes(name, storage, base + element * coverageLayout(name)->length +
            [slots[0][@"expected_offset"] unsignedIntegerValue], resourceType ? 8 : 1, YES);
    }
    for (NSDictionary *slot in slots) {
        MTLPointerType *pointer = slot[@"pointer"];
        if (!pointer.elementIsArgumentBuffer || pointer.elementType != MTLDataTypeStruct) continue;
        NSString *childName = [name stringByAppendingString:@".child"];
        NSMutableArray<NSDictionary *> *children = [NSMutableArray array];
        coverageType(MTLDataTypeStruct, pointer.elementStructType, nil, nil, nil,
            childName, 0, 0, children, 0);
        NSUInteger index = [slot[@"index"] unsignedIntegerValue];
        require(pointer.dataSize == coverageLayout(childName)->length &&
            pointer.alignment == coverageLayout(childName)->alignment, @"A8 child pointer metadata differs from Apple");
        @try {
            id<MTLArgumentEncoder> first = [encoder newArgumentEncoderForBufferAtIndex:index];
            coverageEncoder(device, childName, first, children, depth + 1);
            id<MTLArgumentEncoder> second = [encoder newArgumentEncoderForBufferAtIndex:index];
            printf("ARG7_CHILD case=%s id=%lu first=%d second=%d same_object=%d\n", name.UTF8String,
                (unsigned long)index, first != nil, second != nil, first && first == second);
            coverageIndependent(device, childName, first, second, coverageOracleSlots(childName, children)[0]);
        } @catch (NSException *exception) {
            coverageException(childName, @"child_encoder", exception); require(NO, @"A8 child encoder raised an exception");
        }
    }
}

static NSString *coverageSource(NSString *name, NSUInteger *textureType, NSUInteger *access) {
    NSString *header = @"#include <metal_stdlib>\nusing namespace metal;\n";
    if ([name hasPrefix:@"bulk."]) {
        NSString *fields = [name isEqualToString:@"bulk.texture_buffer"] ?
            @"device float4 *b [[id(1)]]; sampler s [[id(2)]];" :
            @"sampler s [[id(1)]]; device float4 *b [[id(2)]];";
        return [header stringByAppendingFormat:
            @"struct Args { texture2d<float> t [[id(0)]]; %@ array<texture2d<float>,2> more [[id(4)]]; };\n"
             "kernel void a7(constant Args &a [[buffer(2)]], device float4 *o [[buffer(0)]]) { o[0]=a.t.sample(a.s,float2(0.5))+a.more[0].sample(a.s,float2(0.5))+a.more[1].sample(a.s,float2(0.5))+a.b[0]; }\n", fields];
    }
    if ([name hasPrefix:@"nested."]) {
        NSString *pair = @"struct Pair { array<texture2d<float>,2> t [[id(1)]]; sampler s [[id(4)]]; };\n";
        NSString *body = nil;
        if ([name isEqualToString:@"nested.pointer_array"]) body =
            @"struct Args { constant Pair *inner [[id(3)]]; device float4 *b [[id(12)]]; };\n"
             "kernel void a7(constant Args &a [[buffer(2)]], device float4 *o [[buffer(0)]]) { o[0]=a.inner->t[0].sample(a.inner->s,float2(0.5))+a.inner->t[1].sample(a.inner->s,float2(0.5))+a.b[0]; }\n";
        else if ([name isEqualToString:@"nested.inline_array"]) body =
            @"struct Args { Pair inner [[id(3)]]; device float4 *b [[id(12)]]; };\n"
             "kernel void a7(constant Args &a [[buffer(2)]], device float4 *o [[buffer(0)]]) { o[0]=a.inner.t[0].sample(a.inner.s,float2(0.5))+a.inner.t[1].sample(a.inner.s,float2(0.5))+a.b[0]; }\n";
        else body =
            @"struct Args { array<Pair,2> inner [[id(3)]]; device float4 *b [[id(20)]]; };\n"
             "kernel void a7(constant Args &a [[buffer(2)]], device float4 *o [[buffer(0)]]) { o[0]=a.inner[0].t[0].sample(a.inner[0].s,float2(0.5))+a.inner[0].t[1].sample(a.inner[0].s,float2(0.5))+a.inner[1].t[0].sample(a.inner[1].s,float2(0.5))+a.inner[1].t[1].sample(a.inner[1].s,float2(0.5))+a.b[0]; }\n";
        return [[header stringByAppendingString:pair] stringByAppendingString:body];
    }
    if ([name isEqualToString:@"stride.padded"]) return [header stringByAppendingString:
        @"struct Padded { texture2d<float> t [[id(0)]]; float4 tint [[id(2)]]; uint tag [[id(4)]]; };\n"
         "struct Args { array<Padded,2> items [[id(1)]]; constant Padded *child [[id(20)]]; };\n"
         "kernel void a7(constant Args &a [[buffer(2)]], device float4 *o [[buffer(0)]]) { o[0]=a.items[0].t.read(uint2(0))+a.items[1].t.read(uint2(0))+a.child->t.read(uint2(0))+a.items[0].tint+a.items[1].tint+a.child->tint+float(a.items[0].tag+a.items[1].tag+a.child->tag); }\n"];
    NSString *type = nil, *expression = nil;
    *access = MTLBindingAccessReadOnly;
    if ([name isEqualToString:@"metadata.1d"]) {
        *textureType = MTLTextureType1D; type = @"texture1d<float, access::read>";
        expression = @"a.t.read(0u)";
    } else if ([name isEqualToString:@"metadata.cube"]) {
        *textureType = MTLTextureTypeCube; type = @"texturecube<float>";
        expression = @"a.t.sample(a.s,float3(1,0,0))";
    } else if ([name isEqualToString:@"metadata.2d_array"]) {
        *textureType = MTLTextureType2DArray; type = @"texture2d_array<float>";
        expression = @"a.t.sample(a.s,float2(0.5),0u)";
    } else if ([name isEqualToString:@"metadata.msaa"]) {
        *textureType = MTLTextureType2DMultisample; type = @"texture2d_ms<float, access::read>";
        expression = @"a.t.read(uint2(0),0u)";
    } else if ([name isEqualToString:@"metadata.read_write"]) {
        *textureType = MTLTextureType2D; *access = MTLBindingAccessReadWrite;
        type = @"texture2d<float, access::read_write>"; expression = @"a.t.read(uint2(0))";
    } else return nil;
    NSString *reads = [NSString stringWithFormat:@"(%@)+(%@)+(%@)", expression,
        [expression stringByReplacingOccurrencesOfString:@"a.t" withString:@"a.more[0]"],
        [expression stringByReplacingOccurrencesOfString:@"a.t" withString:@"a.more[1]"]];
    return [header stringByAppendingFormat:
        @"struct Args { %@ t [[id(1)]]; array<%@,2> more [[id(3)]]; sampler s [[id(7)]]; };\n"
         "kernel void a7(constant Args &a [[buffer(2)]], device float4 *o [[buffer(0)]]) { float4 v=%@; %@ o[0]=v; }\n",
        type, type, reads, *access == MTLBindingAccessReadWrite ?
            @"a.t.write(v,uint2(0)); a.more[0].write(v,uint2(0)); a.more[1].write(v,uint2(0));" : @""];
}

static void coverageBulk(id<MTLDevice> device, NSString *name, id<MTLArgumentEncoder> encoder) {
    coverageCheckLayout(name, encoder);
    if (!encoder || !encoder.encodedLength || encoder.encodedLength > (1u << 20) ||
        !encoder.alignment || encoder.alignment > 4096) {
        printf("ARG7_BULK case=%s available=0\n", name.UTF8String); return;
    }
    NSUInteger base = ((256 + encoder.alignment - 1) / encoder.alignment) * encoder.alignment;
    id<MTLBuffer> storage = [device newBufferWithLength:base + encoder.encodedLength + 256 options:MTLResourceStorageModeShared];
    require(storage.contents != NULL, @"A8 bulk destination unavailable");
    [encoder setArgumentBuffer:storage offset:base];
    memset(storage.contents, 0xa7, storage.length);
    NSUInteger start = [name isEqualToString:@"bulk.texture_gap"] ? 4 :
        [name isEqualToString:@"bulk.sampler_buffer"] ? 1 : 0;
    NSUInteger count = [name isEqualToString:@"bulk.texture_gap"] ? 3 : 2;
    // All calls have a nonnull input array; nil elements are intentional.
    id<MTLTexture> textures[3] = {nil, nil, nil};
    id<MTLSamplerState> samplers[3] = {nil, nil, nil};
    printf("ARG7_BULK case=%s start=%lu count=%lu begin=1\n", name.UTF8String,
        (unsigned long)start, (unsigned long)count);
    BOOL rejected = NO;
    @try {
        if ([name isEqualToString:@"bulk.sampler_buffer"])
            [encoder setSamplerStates:samplers withRange:NSMakeRange(start, count)];
        else [encoder setTextures:textures withRange:NSMakeRange(start, count)];
        printf("ARG7_RETURN case=%s op=bulk\n", name.UTF8String);
    } @catch (NSException *exception) {
        coverageException(name, @"bulk", exception);
        require([exception.name isEqualToString:@"NSInvalidArgumentException"], @"A8 unexpected bulk exception");
        rejected = YES;
    }
    coverageChanges(name, @"bulk", storage, base);
    if ([encoder isKindOfClass:objc_getClass("RDNA4ArgumentEncoder")]) {
        require(rejected, @"A8 driver accepted a mixed-type or missing-ID bulk range");
        coverageCheckBytes(name, storage, 0, 0, YES);
    }
}

static void coverageDescriptor(id<MTLDevice> device, NSString *name, NSUInteger textureType, NSUInteger access) {
    MTLArgumentDescriptor *descriptor = [MTLArgumentDescriptor argumentDescriptor];
    descriptor.index = 1; descriptor.dataType = MTLDataTypeTexture;
    descriptor.textureType = textureType; descriptor.access = (MTLBindingAccess)access;
    descriptor.arrayLength = 2;
    printf("ARG7_DESCRIPTOR case=%s data_type=%lu texture_type=%lu access=%lu array_length=%lu\n",
        name.UTF8String, (unsigned long)descriptor.dataType, (unsigned long)descriptor.textureType,
        (unsigned long)descriptor.access, (unsigned long)descriptor.arrayLength);
    NSMutableArray<NSDictionary *> *slots = [NSMutableArray array];
    for (NSUInteger n = 0; n < descriptor.arrayLength; ++n)
        [slots addObject:@{@"index":@(descriptor.index + n), @"type":@(MTLDataTypeTexture),
            @"path":[NSString stringWithFormat:@"texture[%lu]", (unsigned long)n],
            @"texture_type":@(textureType), @"access":@(access)}];
    coverageEncoder(device, name, [device newArgumentEncoderWithArguments:@[descriptor]], slots, 0);
}

static int argumentCoverageProbe(const char *target, const char *caseName) {
    setvbuf(stdout, NULL, _IONBF, 0);
    NSString *name = @(caseName);
    printf("ARG7_BEGIN case=%s mode=%s\n", caseName, !strcmp(target, "--reference") ? "reference" : "bundle");
    @try {
        NSError *error = nil;
        id<MTLDevice> device = nil;
        if (!strcmp(target, "--reference")) device = MTLCreateSystemDefaultDevice();
        else {
            setenv("RDNA4M_ALLOW", "1", 1); setenv("RDNA4M_TEST_INPROC", "1", 1);
            if (![[NSBundle bundleWithPath:@(target)] loadAndReturnError:&error]) {
                printf("ARG7_UNAVAILABLE case=%s stage=bundle error=%s\n", caseName, error.description.UTF8String ?: ""); return 1;
            }
            device = [[objc_getClass("GarnetDevice") alloc] init];
            if (device) ((void (*)(id, SEL))objc_msgSend)(device, sel_registerName("initLimits"));
        }
        if (!device) { printf("ARG7_UNAVAILABLE case=%s stage=device\n", caseName); return 1; }
        NSUInteger textureType = MTLTextureType2D, access = MTLBindingAccessReadOnly;
        BOOL descriptorCase = [name hasPrefix:@"descriptor."];
        NSString *sourceName = descriptorCase ? [name stringByReplacingOccurrencesOfString:@"descriptor." withString:@"metadata."] : name;
        NSString *source = coverageSource(sourceName, &textureType, &access);
        if (!source) { printf("ARG7_UNAVAILABLE case=%s stage=case_name\n", caseName); return 1; }
        if (descriptorCase) {
            coverageDescriptor(device, name, textureType, access);
            if (strcmp(target, "--reference") && [name isEqualToString:@"descriptor.1d"]) coverageBulkObjects(device);
            printf("ARG7_DONE case=%s\n", caseName); return 0;
        }
        id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
        if (!library) {
            printf("ARG7_UNAVAILABLE case=%s stage=compile error=%s\nARG7_SOURCE case=%s\n%s\n",
                caseName, error.description.UTF8String ?: "", caseName, source.UTF8String); return 1;
        }
        id<MTLFunction> function = [library newFunctionWithName:@"a7"];
        if (!function) { printf("ARG7_UNAVAILABLE case=%s stage=function\n", caseName); return 1; }
        id<MTLArgumentEncoder> encoder = nil;
        @try { encoder = [function newArgumentEncoderWithBufferIndex:2]; }
        @catch (NSException *exception) { coverageException(name, @"function_encoder", exception); }
        printf("ARG7_FUNCTION_ENCODER case=%s available=%d length=%lu alignment=%lu\n", caseName,
            encoder != nil, (unsigned long)encoder.encodedLength, (unsigned long)encoder.alignment);
        coverageCheckLayout(name, encoder);
        if ([name hasPrefix:@"bulk."]) coverageBulk(device, name, encoder);
        BOOL childCase = [name isEqualToString:@"nested.pointer_array"] || [name isEqualToString:@"stride.padded"];
        NSUInteger childIndex = [name isEqualToString:@"stride.padded"] ? 20 : 3;
        id<MTLArgumentEncoder> declaredChild = nil;
        if (childCase) {
            @try {
                declaredChild = [encoder newArgumentEncoderForBufferAtIndex:childIndex];
                printf("ARG7_DECLARED_CHILD case=%s index=%lu available=%d length=%lu alignment=%lu\n", caseName,
                    (unsigned long)childIndex, declaredChild != nil, (unsigned long)declaredChild.encodedLength,
                    (unsigned long)declaredChild.alignment);
                coverageCheckLayout([name stringByAppendingString:@".child"], declaredChild);
            } @catch (NSException *exception) { coverageException(name, @"declared_child", exception); }
        }
        NSMutableArray<NSDictionary *> *slots = [NSMutableArray array];
        @try {
            MTLComputePipelineReflection *reflection = nil;
            error = nil;
            id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function
                options:MTLPipelineOptionBindingInfo | MTLPipelineOptionBufferTypeInfo reflection:&reflection error:&error];
            printf("ARG7_REFLECTION case=%s pipeline=%d available=%d bindings=%lu error=%s\n", caseName,
                pipeline != nil, reflection != nil, (unsigned long)reflection.bindings.count, error.description.UTF8String ?: "");
            for (id<MTLBinding> binding in reflection.bindings) {
                if (binding.type != MTLBindingTypeBuffer || binding.index != 2) continue;
                id<MTLBufferBinding> buffer = (id)binding;
                printf("ARG7_BUFFER case=%s data_type=%lu data_size=%lu alignment=%lu access=%lu\n", caseName,
                    (unsigned long)buffer.bufferDataType, (unsigned long)buffer.bufferDataSize,
                    (unsigned long)buffer.bufferAlignment, (unsigned long)buffer.access);
                coverageType(MTLDataTypeStruct, buffer.bufferStructType, nil, nil, nil, name, 0, 0, slots, 0);
            }
        } @catch (NSException *exception) { coverageException(name, @"reflection", exception); }
        if (![name hasPrefix:@"bulk."]) {
            BOOL reflectedSlots = slots.count != 0;
            if (!reflectedSlots) printf("ARG7_SLOTS case=%s reflected=0 fallback=Apple_a7_table\n", caseName);
            coverageEncoder(device, name, encoder, slots, 0);
            if (!reflectedSlots && childCase) {
                NSString *childName = [name stringByAppendingString:@".child"];
                NSArray<NSDictionary *> *childSlots = coverageOracleSlots(childName, @[]);
                coverageEncoder(device, childName, declaredChild, childSlots, 0);
                coverageIndependent(device, childName, declaredChild,
                    [encoder newArgumentEncoderForBufferAtIndex:childIndex], childSlots[0]);
            }
            if (childCase && strcmp(target, "--reference"))
                coverageNestedBounds(device, name, encoder, childIndex, coverageLayout([name stringByAppendingString:@".child"])->length);
        }
        printf("ARG7_DONE case=%s\n", caseName);
        return 0;
    } @catch (NSException *exception) { coverageException(name, @"case", exception); return 1; }
}

static void argumentCoverageSurvey(const char *executable, const char *target) {
    NSString *path = [NSBundle mainBundle].executablePath ?: @(executable);
    if (![path isAbsolutePath]) path = [[[NSFileManager defaultManager] currentDirectoryPath] stringByAppendingPathComponent:path];
    NSUInteger count = sizeof argumentCoverageCases / sizeof argumentCoverageCases[0];
    NSUInteger failures = 0;
    printf("ARG7_SURVEY_BEGIN expectations=Apple_a7 cases=%lu isolated=1\n", (unsigned long)count);
    for (NSUInteger n = 0; n < count; ++n) {
        NSString *name = @(argumentCoverageCases[n]);
        NSTask *task = [NSTask new];
        task.executableURL = [NSURL fileURLWithPath:path];
        task.arguments = @[@(target), @"--a7-probe", name];
        task.standardOutput = [NSFileHandle fileHandleWithStandardOutput];
        task.standardError = [NSFileHandle fileHandleWithStandardError];
        NSError *error = nil;
        if (![task launchAndReturnError:&error]) {
            printf("ARG7_CHILD_EXIT case=%s launch=0 error=%s\n", name.UTF8String, error.description.UTF8String ?: "");
            ++failures;
            continue;
        }
        [task waitUntilExit];
        printf("ARG7_CHILD_EXIT case=%s launch=1 reason=%s status=%d\n", name.UTF8String,
            task.terminationReason == NSTaskTerminationReasonUncaughtSignal ? "signal" : "exit", task.terminationStatus);
        if (task.terminationStatus || task.terminationReason == NSTaskTerminationReasonUncaughtSignal) ++failures;
    }
    printf("ARG7_SURVEY_DONE expectations=Apple_a7 cases=%lu failures=%lu no_gpu_dispatch=1\n",
        (unsigned long)count, (unsigned long)failures);
    require(failures == 0, @"A8 coverage child failed; see ARG7_CHILD_EXIT");
    puts("ARG8_COVERAGE_PASS cases=18 children=2 expected_table=Apple_a7");
}
