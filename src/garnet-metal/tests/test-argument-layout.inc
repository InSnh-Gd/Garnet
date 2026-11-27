// Native values captured in scratch/ua/audit/a5vm/tae-ref.log.
typedef struct {
    NSUInteger index;
    MTLDataType type;
} RDNA4SurveySlot;

static void surveySet(id<MTLArgumentEncoder> encoder, RDNA4SurveySlot slot,
                      id<MTLBuffer> buffer, id<MTLTexture> texture, id<MTLSamplerState> sampler) {
    switch (slot.type) {
    case MTLDataTypePointer: [encoder setBuffer:buffer offset:0 atIndex:slot.index]; break;
    case MTLDataTypeTexture: [encoder setTexture:texture atIndex:slot.index]; break;
    case MTLDataTypeSampler: [encoder setSamplerState:sampler atIndex:slot.index]; break;
    default: require(NO, @"unknown survey resource type");
    }
}

static void surveyGuards(id<MTLBuffer> storage, NSUInteger base, NSUInteger length) {
    const uint8_t *bytes = storage.contents;
    for (NSUInteger i = 0; i < storage.length; ++i)
        if (i < base || i >= base + length)
            require(bytes[i] == 0xa7, @"resource setter crossed destination guards");
}

static void surveyReject(NSString *name, void (^operation)(void), id<MTLBuffer> storage) {
    NSData *before = [NSData dataWithBytes:storage.contents length:storage.length];
    BOOL rejected = NO;
    @try { operation(); }
    @catch (NSException *exception) {
        require([exception.name isEqualToString:NSInvalidArgumentException], @"unexpected validation exception");
        rejected = YES;
    }
    require(rejected, [@"invalid encoder operation accepted: " stringByAppendingString:name]);
    require(!memcmp(storage.contents, before.bytes, storage.length), @"rejected setter modified storage");
}

static void surveyEncoder(id<MTLDevice> device, NSString *name, id<MTLArgumentEncoder> encoder,
                          const RDNA4SurveySlot *slots, NSUInteger count) {
    require(encoder != nil, [@"encoder missing: " stringByAppendingString:name]);
    NSUInteger length = encoder.encodedLength, alignment = encoder.alignment;
    printf("ARG_LAYOUT case=%s length=%lu alignment=%lu\n", name.UTF8String,
        (unsigned long)length, (unsigned long)alignment);
    NSDictionary<NSString *, NSNumber *> *expectedLengths = @{
        @"mixed":@24, @"textures":@32, @"nested":@16, @"nested.inner":@16,
        @"samplers":@24, @"descriptor.mixed":@24, @"descriptor.textures":@32, @"descriptor.samplers":@24
    };
    require(expectedLengths[name] && length == expectedLengths[name].unsignedIntegerValue && alignment == 8,
        [@"layout differs from Apple: " stringByAppendingString:name]);
    require(length && length <= (1u << 20) && alignment && alignment <= 4096 &&
        !(alignment & (alignment - 1)), @"invalid survey encoder layout");
    NSUInteger base = ((256 + alignment - 1) / alignment) * alignment;
    id<MTLBuffer> storage = [device newBufferWithLength:base + length + 256 options:MTLResourceStorageModeShared];
    id<MTLBuffer> resource = [device newBufferWithLength:256 options:MTLResourceStorageModeShared];
    require(storage.contents && resource.contents, @"survey buffers unavailable");
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
        width:4 height:4 mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [device newTextureWithDescriptor:td];
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.supportArgumentBuffers = YES;
    id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:sd];
    require(texture && sampler, @"survey texture/sampler unavailable");
    [encoder setArgumentBuffer:storage offset:base];
    require(count <= 8, @"too many survey slots");
    uint64_t tokens[8] = {0};
    for (NSUInteger k = 0; k < count; ++k) {
        // Nil encodings change a known nonzero fill, revealing the whole slot.
        memset(storage.contents, 0xa7, storage.length);
        surveySet(encoder, slots[k], nil, nil, nil);
        surveyGuards(storage, base, length);
        const uint8_t *bytes = (const uint8_t *)storage.contents + base;
        NSUInteger first = length, last = 0, changed = 0;
        for (NSUInteger i = 0; i < length; ++i) if (bytes[i] != 0xa7) {
            if (first == length) first = i;
            last = i; ++changed;
        }
        printf("ARG_SLOT case=%s id=%lu type=%lu nil_first=%ld nil_span=%lu changed=%lu\n",
            name.UTF8String, (unsigned long)slots[k].index, (unsigned long)slots[k].type,
            first == length ? -1L : (long)first, (unsigned long)(changed ? last - first + 1 : 0),
            (unsigned long)changed);
        // Every captured case packs its enumerated IDs into consecutive 8-byte
        // slots, even when the numeric IDs have gaps.
        NSUInteger expectedOffset = k * 8;
        require(first == expectedOffset && last == expectedOffset + 7 && changed == 8,
            @"nil setter write span differs from Apple");
        for (NSUInteger i = 0; i < length; ++i)
            require(bytes[i] == (i >= expectedOffset && i < expectedOffset + 8 ? 0 : 0xa7),
                @"nil setter did not zero exactly one resource slot");
        // Measure nonnil changes independently; opaque bytes vary across runs.
        memset(storage.contents, 0xa7, storage.length);
        surveySet(encoder, slots[k], resource, texture, sampler);
        surveyGuards(storage, base, length);
        first = length; last = changed = 0;
        for (NSUInteger i = 0; i < length; ++i) if (bytes[i] != 0xa7) {
            if (first == length) first = i;
            last = i; ++changed;
        }
        printf("ARG_WRITE case=%s id=%lu first=%ld span=%lu changed=%lu\n", name.UTF8String,
            (unsigned long)slots[k].index, first == length ? -1L : (long)first,
            (unsigned long)(changed ? last - first + 1 : 0), (unsigned long)changed);
        require(changed && first >= expectedOffset && last < expectedOffset + 8,
            @"nonnil setter wrote outside its resource slot or wrote nothing");
        uint64_t token = 0;
        memcpy(&token, bytes + expectedOffset, sizeof token);
        require(token != 0, @"nonnil resource encoded a null token");
        tokens[k] = token;
        // Check repeatability without comparing opaque resource tokens to Apple.
        surveySet(encoder, slots[k], resource, texture, sampler);
        require(!memcmp(bytes + expectedOffset, &token, sizeof token), @"resource token changed on repeat encoding");
    }
    // Bulk calls cover each contiguous run of same-kind resource IDs.
    for (NSUInteger k = 0; k < count;) {
        NSUInteger end = k + 1;
        while (end < count && slots[end].type == slots[k].type && slots[end].index == slots[end - 1].index + 1) ++end;
        require(end - k <= 8, @"survey bulk run too large");
        id<MTLTexture> textures[8] = {nil};
        id<MTLSamplerState> samplers[8] = {nil};
        id<MTLBuffer> buffers[8] = {nil};
        NSUInteger offsets[8] = {0};
        NSRange range = NSMakeRange(slots[k].index, end - k);
        for (unsigned pass = 0; pass < 2; ++pass) {
            for (NSUInteger j = 0; j < range.length; ++j) {
                textures[j] = pass ? texture : nil;
                samplers[j] = pass ? sampler : nil;
                buffers[j] = pass ? resource : nil;
            }
            memset(storage.contents, 0xa7, storage.length);
            if (slots[k].type == MTLDataTypeTexture) [encoder setTextures:textures withRange:range];
            else if (slots[k].type == MTLDataTypeSampler) [encoder setSamplerStates:samplers withRange:range];
            else [encoder setBuffers:buffers offsets:offsets withRange:range];
            surveyGuards(storage, base, length);
            const uint8_t *bytes = (const uint8_t *)storage.contents + base;
            for (NSUInteger j = 0; j < length; ++j) {
                BOOL inRun = j >= k * 8 && j < end * 8;
                if (!inRun) require(bytes[j] == 0xa7, @"bulk setter changed another argument");
                else if (!pass) require(bytes[j] == 0, @"bulk nil setter did not zero its slots");
            }
            if (pass) for (NSUInteger j = k; j < end; ++j)
                require(!memcmp(bytes + j * 8, &tokens[j], sizeof tokens[j]),
                    @"bulk resource token differs from single setter");
        }
        k = end;
    }
    // Deliberately exercise null arrays despite the SDK's nonnull annotation.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnonnull"
    [encoder setTextures:NULL withRange:NSMakeRange(0, 0)];
    [encoder setSamplerStates:NULL withRange:NSMakeRange(0, 0)];
#pragma clang diagnostic pop
    if ([encoder isKindOfClass:NSClassFromString(@"RDNA4ArgumentEncoder")]) {
        surveyReject(@"absent ID", ^{ [encoder setTexture:nil atIndex:NSUIntegerMax]; }, storage);
        surveyReject(@"wrong texture class", ^{
            for (NSUInteger k = 0; k < count; ++k) if (slots[k].type == MTLDataTypeTexture) {
                [encoder setTexture:(id)[NSObject new] atIndex:slots[k].index]; return;
            }
            [encoder setTexture:nil atIndex:slots[0].index];
        }, storage);
        surveyReject(@"wrong sampler class", ^{
            for (NSUInteger k = 0; k < count; ++k) if (slots[k].type == MTLDataTypeSampler) {
                [encoder setSamplerState:(id)[NSObject new] atIndex:slots[k].index]; return;
            }
            [encoder setSamplerState:nil atIndex:slots[0].index];
        }, storage);
        surveyReject(@"resource as constant", ^{ (void)[encoder constantDataAtIndex:slots[0].index]; }, storage);
        surveyReject(@"range overflow", ^{
            id<MTLTexture> __unsafe_unretained nilTexture = nil;
            [encoder setTextures:&nilTexture withRange:NSMakeRange(NSUIntegerMax, 2)];
        }, storage);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnonnull"
        surveyReject(@"null sampler array", ^{ [encoder setSamplerStates:NULL withRange:NSMakeRange(0, 1)]; }, storage);
#pragma clang diagnostic pop
        surveyReject(@"undersized destination", ^{ [encoder setArgumentBuffer:storage offset:storage.length]; }, storage);
        [encoder setArgumentBuffer:storage offset:base];
        [encoder setArgumentBuffer:nil offset:0];
        surveyReject(@"unbound destination", ^{ surveySet(encoder, slots[0], nil, nil, nil); }, storage);
        [encoder setArgumentBuffer:storage offset:base];
        puts("ARG_SETTERS_INVALID_PASS");
    }
    puts("ARG_SETTERS_GUARDS_PASS");
}

static int argumentLayoutSurvey(const char *target) {
    setvbuf(stdout, NULL, _IONBF, 0);
    NSError *error = nil;
    id<MTLDevice> device = nil;
    if (!strcmp(target, "--reference")) device = MTLCreateSystemDefaultDevice();
    else {
        setenv("RDNA4M_ALLOW", "1", 1);
        setenv("RDNA4M_TEST_INPROC", "1", 1);
        require([[NSBundle bundleWithPath:@(target)] loadAndReturnError:&error],
            error.description ?: @"bundle load failed");
        device = [[objc_getClass("GarnetDevice") alloc] init];
        require(device != nil, @"bundle device unavailable");
        ((void (*)(id, SEL))objc_msgSend)(device, sel_registerName("initLimits"));
    }
    require(device != nil, @"survey device unavailable");
    NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct Resources { device float *b [[id(0)]]; texture2d<float> t [[id(1)]]; sampler s [[id(2)]]; };\n"
        "kernel void mixed(constant Resources &a [[buffer(2)]], device float4 *o [[buffer(0)]]) { o[0]=a.t.sample(a.s,float2(0.5))+a.b[0]; }\n"
        "struct Textures { array<texture2d<float>,3> t [[id(2)]]; sampler s [[id(7)]]; };\n"
        "kernel void textures(constant Textures &a [[buffer(2)]], device float4 *o [[buffer(0)]]) { o[0]=a.t[0].sample(a.s,float2(0.5))+a.t[1].sample(a.s,float2(0.5))+a.t[2].sample(a.s,float2(0.5)); }\n"
        "struct Inner { texture2d<float> t [[id(1)]]; sampler s [[id(4)]]; };\n"
        "struct Outer { constant Inner *inner [[id(3)]]; device float *b [[id(8)]]; };\n"
        "kernel void nested(constant Outer &a [[buffer(2)]], device float4 *o [[buffer(0)]]) { o[0]=a.inner->t.sample(a.inner->s,float2(0.5))+a.b[0]; }\n"
        "struct Samplers { sampler a [[id(0)]]; array<sampler,2> b [[id(3)]]; };\n"
        "kernel void samplers(constant Samplers &a [[buffer(2)]], texture2d<float> t [[texture(0)]], device float4 *o [[buffer(0)]]) { o[0]=t.sample(a.a,float2(0.5))+t.sample(a.b[0],float2(0.5))+t.sample(a.b[1],float2(0.5)); }\n";
    id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
    require(library != nil, error.description ?: @"survey shader compilation failed");
    const RDNA4SurveySlot mixed[] = {{0,MTLDataTypePointer},{1,MTLDataTypeTexture},{2,MTLDataTypeSampler}};
    const RDNA4SurveySlot textures[] = {{2,MTLDataTypeTexture},{3,MTLDataTypeTexture},{4,MTLDataTypeTexture},{7,MTLDataTypeSampler}};
    const RDNA4SurveySlot outer[] = {{3,MTLDataTypePointer},{8,MTLDataTypePointer}};
    const RDNA4SurveySlot inner[] = {{1,MTLDataTypeTexture},{4,MTLDataTypeSampler}};
    const RDNA4SurveySlot samplers[] = {{0,MTLDataTypeSampler},{3,MTLDataTypeSampler},{4,MTLDataTypeSampler}};
    const struct { const char *name; const RDNA4SurveySlot *slots; NSUInteger count; } cases[] = {
        {"mixed",mixed,3},{"textures",textures,4},{"nested",outer,2},{"samplers",samplers,3}
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        @try {
            id<MTLFunction> function = [library newFunctionWithName:@(cases[i].name)];
            require(function != nil, @"survey kernel missing");
            id<MTLArgumentEncoder> encoder = [function newArgumentEncoderWithBufferIndex:2];
            surveyEncoder(device, @(cases[i].name), encoder, cases[i].slots, cases[i].count);
            if (i == 2 && encoder) surveyEncoder(device, @"nested.inner",
                [encoder newArgumentEncoderForBufferAtIndex:3], inner, 2);
        } @catch (NSException *exception) {
            printf("ARG_CASE_EXCEPTION case=%s name=%s reason=%s\n", cases[i].name,
                exception.name.UTF8String, exception.reason.UTF8String);
            require(NO, @"function encoder case raised an exception");
        }
    }
    const struct { const char *name; const RDNA4SurveySlot *slots; NSUInteger count; } descriptors[] = {
        {"descriptor.mixed",mixed,3},{"descriptor.textures",textures,4},{"descriptor.samplers",samplers,3}
    };
    for (unsigned i = 0; i < sizeof descriptors / sizeof descriptors[0]; ++i) {
        @try {
            NSMutableArray<MTLArgumentDescriptor *> *arguments = [NSMutableArray array];
            // Use arrayLength for contiguous texture/sampler runs, preserving sparse IDs.
            for (NSUInteger j = 0; j < descriptors[i].count;) {
                NSUInteger end = j + 1;
                while (end < descriptors[i].count && descriptors[i].slots[end].type == descriptors[i].slots[j].type &&
                    descriptors[i].slots[end].index == descriptors[i].slots[end - 1].index + 1) ++end;
                MTLArgumentDescriptor *d = [MTLArgumentDescriptor argumentDescriptor];
                d.index = descriptors[i].slots[j].index; d.dataType = descriptors[i].slots[j].type;
                d.arrayLength = end - j; d.access = MTLBindingAccessReadOnly;
                if (d.dataType == MTLDataTypeTexture) d.textureType = MTLTextureType2D;
                [arguments addObject:d]; j = end;
            }
            surveyEncoder(device, @(descriptors[i].name), [device newArgumentEncoderWithArguments:arguments],
                descriptors[i].slots, descriptors[i].count);
        } @catch (NSException *exception) {
            printf("ARG_CASE_EXCEPTION case=%s name=%s reason=%s\n", descriptors[i].name,
                exception.name.UTF8String, exception.reason.UTF8String);
            require(NO, @"descriptor encoder case raised an exception");
        }
    }
    puts("ARG_LAYOUT_PASS cases=8 expected_table=Apple_a5vm no_gpu_dispatch=1");
    return 0;
}
