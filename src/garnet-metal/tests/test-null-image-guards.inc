// Reuse typed-image population and exact read/write oracles; never call a null member.
static void runNullImageGuards(id<MTLDevice> device, id<MTLCommandQueue> queue,
    const Shape *shape, unsigned kind, NSString *directory, BOOL compileOnly,
    BOOL sampling, MTLCompareFunction comparison) {
    check(!sampling || (shape->depth && !shape->multisample), @"null sampling needs a non-MS depth image");
    NSUInteger outputWords = sampling ? 7u : 5u;
    BOOL writable = !shape->depth && !shape->multisample;
    NSString *scalar = shape->depth || !kind ? @"float" : kind == 1 ? @"uint" : @"int";
    NSString *type = [NSString stringWithFormat:@"%s%s<%@%@>", shape->depth ? "depth" : "texture",
        shape->name, scalar, shape->multisample ? @", access::read" : @""];
    NSString *coord = oneDimensional(shape) ? @"2u" : shape->type == MTLTextureType3D ?
        @"uint3(2,1,2)" : @"uint2(2,1)";
    NSString *suffix = [NSString stringWithFormat:@"%@%@, %uu", shape->cube ? @", 2u" : @"",
        shape->arrayed ? @", 1u" : @"", shape->multisample ? 2 : 0];
    NSString *dst = writable ? [NSString stringWithFormat:
        @"array<texture%s<%@, access::write>,2> dst [[id(2)]];", shape->name, scalar] : @"";
    NSString *store = writable ? [NSString stringWithFormat:
        @"auto dst = a.dst[s]; bool missingDst = is_null_texture(dst); out[4] = uint(missingDst);"
         " if (!missingDst) dst.write(%@4(%@), %@%@%@, 0u);", scalar,
        kind == 2 ? @"-77" : @"77", coord, shape->cube ? @", 2u" : @"",
        shape->arrayed ? @", 1u" : @""] : @"out[4] = 0xffffffffu;";
    NSString *uv = shape->cube ? @"float3(0.25f,1.0f,-0.25f)" : @"float2(0.625f,0.375f)";
    NSString *layer = shape->arrayed ? @", 1u" : @"";
    NSString *samples = sampling ? [NSString stringWithFormat:
        @"if (missing) { out[5]=0xabc005u; out[6]=0xabc006u; } else {"
         "out[5]=as_type<uint>(image.sample(a.filter, %@%@, level(0.0f)));"
         "out[6]=as_type<uint>(image.sample_compare(a.filter, %@%@, 0.25f, level(0.0f))); }",
        uv, layer, uv, layer] : @"";
    NSString *source = [NSString stringWithFormat:
        @"#include <metal_stdlib>\nusing namespace metal;\n"
         "struct Args { array<%@,2> src [[id(0)]]; %@ %@ };\n"
         "kernel void null_image_guards(constant Args &a [[buffer(3)]], device uint *out [[buffer(0)]],"
         " constant uint &choose [[buffer(1)]]) { uint s = choose & 1u; auto image = a.src[s];"
         " bool missing = is_null_texture(image); out[0] = uint(missing);"
         " if (missing) { out[1]=0xabc001u; out[2]=0xabc002u; out[3]=0xabc003u; }"
         " else { out[1]=as_type<uint>(float(image.read(%@%@)%@)); out[2]=image.get_width();"
         " out[3]=%@; } %@ %@ }\n", type, dst, sampling ? @"sampler filter [[id(4)]];" : @"",
        coord, suffix, shape->depth ? @"" : @".x",
        shape->multisample ? @"image.get_num_samples()" : oneDimensional(shape) ? @"1u" : @"image.get_height()", store, samples];
    NSString *label = [(sampling ? @"null-sampling-" : @"null-guard-") stringByAppendingString:caseLabel(shape, kind)];
    NSError *error = nil;
    check([source writeToFile:[directory stringByAppendingPathComponent:[label stringByAppendingString:@".metal"]]
        atomically:YES encoding:NSUTF8StringEncoding error:&error], @"null guard source write");
    MTLCompileOptions *options = [MTLCompileOptions new]; options.languageVersion = MTLLanguageVersion2_1;
    id<MTLLibrary> library = [device newLibraryWithSource:source options:options error:&error];
    check(library != nil, error.description ?: @"null guard library");
    id<MTLFunction> function = [library newFunctionWithName:@"null_image_guards"];
    check(function != nil, @"null guard function");
    SEL bitcode = NSSelectorFromString(@"bitcodeData");
    check([function respondsToSelector:bitcode], @"null guard AIR selector");
    NSData *air = ((id (*)(id, SEL))objc_msgSend)(function, bitcode);
    check(air.length && [air writeToFile:[directory stringByAppendingPathComponent:
        [label stringByAppendingString:@".air"]] atomically:YES], @"null guard AIR capture");
    if (compileOnly) return;
    id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function error:&error];
    check(pipeline != nil, error.description ?: @"null guard pipeline");
    id<MTLArgumentEncoder> arguments = [function newArgumentEncoderWithBufferIndex:3];
    check(arguments && arguments.encodedLength >= (sampling ? 24u : writable ? 32u : 16u) &&
        arguments.alignment && arguments.alignment <= kGuard && kGuard % arguments.alignment == 0,
        @"null guard argument layout");
    id<MTLTexture> inputs[2], outputs[2] = {nil, nil};
    id<MTLSamplerState> filter = nil;
    if (sampling) {
        MTLSamplerDescriptor *descriptor = [MTLSamplerDescriptor new];
        descriptor.minFilter = descriptor.magFilter = MTLSamplerMinMagFilterNearest;
        descriptor.mipFilter = MTLSamplerMipFilterNearest;
        descriptor.normalizedCoordinates = YES;
        descriptor.sAddressMode = descriptor.tAddressMode = descriptor.rAddressMode = MTLSamplerAddressModeClampToEdge;
        descriptor.compareFunction = comparison;
        descriptor.supportArgumentBuffers = YES;
        filter = [device newSamplerStateWithDescriptor:descriptor];
        check(filter != nil, @"null guard dual-use sampler");
    }
    for (NSUInteger r=0; r<2; ++r) {
        inputs[r] = texture(device, shape, kind, NO);
        if (shape->multisample) populateMultisample(device, queue, inputs[r], shape, kind, r);
        else upload(device, queue, inputs[r], shape, kind, r, NO);
        if (writable) outputs[r] = texture(device, shape, kind, YES);
    }
    for (unsigned privateArgs=0; privateArgs<2; ++privateArgs)
        for (uint32_t missing=0; missing<2; ++missing) {
            id<MTLBuffer> encoded = buffer(device, arguments.encodedLength);
            [arguments setArgumentBuffer:encoded offset:kGuard];
            if (sampling) [arguments setSamplerState:filter atIndex:4];
            for (unsigned r=0; r<2; ++r) {
                [arguments setTexture:r==missing ? nil : inputs[r] atIndex:r];
                if (writable) [arguments setTexture:r!=missing ? nil : outputs[r] atIndex:2+r];
            }
            NSData *before = [NSData dataWithBytes:encoded.contents length:encoded.length];
            for (uint32_t selected=0; selected<2; ++selected) {
                if (writable) for (unsigned r=0; r<2; ++r)
                    upload(device, queue, outputs[r], shape, kind, r, YES);
                id<MTLBuffer> payload = privateArgs ? [device newBufferWithLength:encoded.length
                    options:MTLResourceStorageModePrivate] : encoded;
                id<MTLBuffer> copied = privateArgs ? buffer(device, arguments.encodedLength) : encoded;
                id<MTLBuffer> observation = buffer(device, outputWords*sizeof(uint32_t));
                id<MTLBuffer> selection = buffer(device, sizeof(selected));
                check(payload && copied, @"null guard transport buffers");
                memcpy((uint8_t *)selection.contents+kGuard, &selected, sizeof(selected));
                id<MTLCommandBuffer> command = [queue commandBuffer];
                if (privateArgs) {
                    id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
                    check(blit != nil, @"null guard private upload");
                    [blit copyFromBuffer:encoded sourceOffset:0 toBuffer:payload destinationOffset:0 size:encoded.length];
                    [blit endEncoding];
                }
                id<MTLComputeCommandEncoder> compute = [command computeCommandEncoder];
                check(compute != nil, @"null guard compute encoder");
                [compute setComputePipelineState:pipeline];
                [compute setBuffer:payload offset:kGuard atIndex:3];
                [compute setBuffer:observation offset:kGuard atIndex:0];
                [compute setBuffer:selection offset:kGuard atIndex:1];
                for (unsigned r=0; r<2; ++r) {
                    [compute useResource:inputs[r] usage:MTLResourceUsageRead];
                    if (writable) [compute useResource:outputs[r] usage:MTLResourceUsageWrite];
                }
                [compute dispatchThreads:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];
                [compute endEncoding];
                if (privateArgs) {
                    id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
                    check(blit != nil, @"null guard private readback");
                    [blit copyFromBuffer:payload sourceOffset:0 toBuffer:copied destinationOffset:0 size:encoded.length];
                    [blit endEncoding];
                }
                finish(command, label);
                float value = texel(shape, kind, selected, 0,
                    shape->cube ? (shape->arrayed ? 8 : 2) : shape->arrayed ? 1 : 0,
                    2, oneDimensional(shape) ? 0 : 1, shape->type==MTLTextureType3D ? 2 : 0,
                    shape->multisample ? 2 : 0);
                uint32_t bits; memcpy(&bits, &value, sizeof(bits));
                BOOL nullSource = selected==missing;
                float compared = comparison == MTLCompareFunctionAlways ? 1.0f :
                    comparison == MTLCompareFunctionNever ? 0.0f : 0.25f < value;
                uint32_t comparisonBits; memcpy(&comparisonBits, &compared, sizeof(compared));
                uint32_t expected[7] = {nullSource, nullSource ? 0xabc001u : bits,
                    nullSource ? 0xabc002u : 4u,
                    nullSource ? 0xabc003u : shape->multisample ? 4u : oneDimensional(shape) ? 1u : 4u,
                    writable ? !nullSource : UINT32_MAX,
                    nullSource ? 0xabc005u : bits, nullSource ? 0xabc006u : comparisonBits};
                const uint32_t *actual = (const void *)((const uint8_t *)observation.contents+kGuard);
                for (NSUInteger i=0; i<outputWords; ++i) check(actual[i]==expected[i], [NSString stringWithFormat:
                    @"%@ private=%u missing=%u selected=%u output=%u expected=%u actual=%u",
                    label, privateArgs, missing, selected, (unsigned)i, expected[i], actual[i]]);
                check(!memcmp(encoded.contents, before.bytes, encoded.length) &&
                    !memcmp(copied.contents, before.bytes, encoded.length), @"null guard argument bytes changed");
                check(!memcmp((const uint8_t *)selection.contents+kGuard, &selected, sizeof(selected)),
                    @"null guard selection changed");
                guards(observation, outputWords*sizeof(uint32_t), @"null guard output");
                guards(selection, sizeof(selected), @"null guard selection");
                if (writable) for (unsigned r=0; r<2; ++r)
                    verifyWrites(device, queue, outputs[r], shape, kind, r==selected && nullSource);
            }
        }
    printf("ARGUMENT_NULL_GUARD_PASS case=%s dispatches=8 storage=shared,private null_members=0 "
        "source_and_destination_opposite=1 write_guards=%d dual_use=%d compare=%lu output_words=%lu gpu_execution=1\n",
        caseLabel(shape, kind).UTF8String, writable, sampling, (unsigned long)comparison, (unsigned long)outputWords);
}
