// Shares the image fixture's guarded buffers, typed uploads and CPU texel oracle.
static void runCubeReadGrid(id<MTLDevice> device, id<MTLCommandQueue> queue,
                           const Shape *shape, unsigned kind, NSString *directory,
                           BOOL compileOnly) {
    check(shape->cube, @"read-grid requires a cube or cube array");
    NSString *scalar = kind == 0 ? @"float" : kind == 1 ? @"uint" : @"int";
    NSString *layer = shape->arrayed ? @", p.z" : @"";
    NSString *source = [NSString stringWithFormat:
        @"#include <metal_stdlib>\nusing namespace metal;\n"
         "struct Args { array<%s%s<%@>, 2> src [[id(0)]]; sampler filter [[id(2)]]; };\n"
         "kernel void cube_reads(constant Args &a [[buffer(3)]], device float *out [[buffer(0)]], "
         "constant uint4 &p [[buffer(1)]], uint tid [[thread_position_in_grid]]) { "
         "uint w = a.src[p.x].get_width(p.w); "
         "out[tid] = float(a.src[p.x].read(uint2(tid %% w, tid / w), p.y%@, p.w)%@); "
         "out[16 + tid] = float(a.src[p.x].sample(a.filter, float3(0.25f,1.0f,-0.25f)%@, level(float(p.w)))%@); }\n",
        shape->depth ? "depth" : "texture", shape->name, scalar, layer,
        shape->depth ? @"" : @".x", layer, shape->depth ? @"" : @".x"];
    NSString *label = [caseLabel(shape, kind) stringByAppendingString:@"-read-grid"];
    NSError *error = nil;
    check([source writeToFile:[directory stringByAppendingPathComponent:[label stringByAppendingString:@".metal"]]
        atomically:YES encoding:NSUTF8StringEncoding error:&error], @"cube read source write failed");
    MTLCompileOptions *options = [MTLCompileOptions new];
    options.languageVersion = MTLLanguageVersion2_1;
    id<MTLLibrary> library = [device newLibraryWithSource:source options:options error:&error];
    check(library != nil, error.description ?: @"cube read compile failed");
    id<MTLFunction> function = [library newFunctionWithName:@"cube_reads"];
    check(function != nil, @"cube read function unavailable");
    SEL selector = NSSelectorFromString(@"bitcodeData");
    check(!compileOnly || [function respondsToSelector:selector], @"cube read AIR capture unavailable");
    if ([function respondsToSelector:selector]) {
        NSData *air = ((id (*)(id, SEL))objc_msgSend)(function, selector);
        check(air.length != 0 && [air writeToFile:[directory stringByAppendingPathComponent:
            [label stringByAppendingString:@".air"]] atomically:YES], @"cube read AIR capture failed");
    }
    if (compileOnly) return;
    id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function error:&error];
    check(pipeline != nil, error.description ?: @"cube read pipeline unavailable");
    id<MTLArgumentEncoder> arguments = [function newArgumentEncoderWithBufferIndex:3];
    check(arguments != nil && arguments.encodedLength >= 24 && arguments.alignment != 0 &&
        kGuard % arguments.alignment == 0, @"cube read argument layout invalid");
    id<MTLBuffer> encoded = buffer(device, arguments.encodedLength);
    [arguments setArgumentBuffer:encoded offset:kGuard];
    id<MTLTexture> inputs[2];
    for (NSUInteger resource = 0; resource < 2; ++resource) {
        inputs[resource] = texture(device, shape, kind, NO);
        upload(device, queue, inputs[resource], shape, kind, resource, NO);
        [arguments setTexture:inputs[resource] atIndex:resource];
    }
    MTLSamplerDescriptor *descriptor = [MTLSamplerDescriptor new];
    descriptor.minFilter = MTLSamplerMinMagFilterNearest;
    descriptor.magFilter = MTLSamplerMinMagFilterNearest;
    descriptor.mipFilter = MTLSamplerMipFilterNearest;
    descriptor.supportArgumentBuffers = YES;
    id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:descriptor];
    check(sampler != nil, @"cube read control sampler unavailable");
    [arguments setSamplerState:sampler atIndex:2];
    NSData *argumentSnapshot = [NSData dataWithBytes:encoded.contents length:encoded.length];
    unsigned dispatches = 0, observations = 0;
    for (uint32_t resource = 0; resource < 2; ++resource)
        for (uint32_t mip = 0; mip < 2; ++mip)
            for (uint32_t slice = 0; slice < (shape->arrayed ? 2u : 1u); ++slice)
                for (uint32_t face = 0; face < 6; ++face) {
                    const uint32_t parameters[] = {resource, face, slice, mip};
                    const NSUInteger width = 4u >> mip, count = width * width;
                    id<MTLBuffer> selection = buffer(device, sizeof(parameters));
                    memcpy((uint8_t *)selection.contents + kGuard, parameters, sizeof(parameters));
                    id<MTLBuffer> output = buffer(device, 32 * sizeof(float));
                    id<MTLCommandBuffer> command = [queue commandBuffer];
                    id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
                    check(encoder != nil, @"cube read encoder unavailable");
                    [encoder setComputePipelineState:pipeline];
                    [encoder setBuffer:encoded offset:kGuard atIndex:3];
                    [encoder setBuffer:output offset:kGuard atIndex:0];
                    [encoder setBuffer:selection offset:kGuard atIndex:1];
                    for (NSUInteger r = 0; r < 2; ++r) [encoder useResource:inputs[r] usage:MTLResourceUsageRead];
                    [encoder dispatchThreads:MTLSizeMake(count, 1, 1) threadsPerThreadgroup:MTLSizeMake(count, 1, 1)];
                    [encoder endEncoding];
                    finish(command, label);
                    const float *actual = (const float *)((const uint8_t *)output.contents + kGuard);
                    for (NSUInteger i = 0; i < 32; ++i) {
                        NSUInteger tid = i % 16;
                        if (tid >= count) {
                            const uint8_t *bytes = (const uint8_t *)&actual[i];
                            for (NSUInteger b = 0; b < sizeof(float); ++b)
                                check(bytes[b] == kCanary, @"cube read untouched output changed");
                            continue;
                        }
                        NSUInteger selectedFace = i < 16 ? face : 2;
                        NSUInteger x = i < 16 ? tid % width : (mip ? 1u : 2u);
                        NSUInteger y = i < 16 ? tid / width : (mip ? 0u : 1u);
                        float expected = texel(shape, kind, resource, mip, slice * 6 + selectedFace, x, y, 0, 0);
                        check(actual[i] == expected, [NSString stringWithFormat:
                            @"%@ resource=%u mip=%u slice=%u face=%u output=%lu expected=%g actual=%g",
                            label, resource, mip, slice, face, (unsigned long)i, expected, actual[i]]);
                        ++observations;
                    }
                    guards(output, 32 * sizeof(float), @"cube read output");
                    guards(selection, sizeof(parameters), @"cube read selection");
                    check(memcmp((uint8_t *)selection.contents + kGuard, parameters, sizeof(parameters)) == 0,
                        @"cube read parameters changed");
                    check(memcmp(encoded.contents, argumentSnapshot.bytes, encoded.length) == 0,
                        @"cube read argument bytes changed");
                    ++dispatches;
                }
    printf("ARGUMENT_CUBE_READS_PASS shape=%s kind=%u faces=6 slices=%u mips=2 resources=2 "
        "dispatches=%u observations=%u read_and_sample=1 guards=output,arguments,selection gpu_execution=1\n",
        shape->name, kind, shape->arrayed ? 2 : 1, dispatches, observations);
}
