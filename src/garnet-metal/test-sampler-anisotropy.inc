// Anisotropic kernels are implementation-dependent. Compare all three representations on the same
// device and require resolution recovery over an isotropic 8x1 footprint, without inventing taps.
static id<MTLTexture> anisotropyTexture(id<MTLDevice> device, unsigned resource) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
        MTLPixelFormatRGBA32Float width:64 height:64 mipmapped:YES];
    d.storageMode = MTLStorageModeShared;
    d.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> result = [device newTextureWithDescriptor:d];
    require(result != nil, "anisotropy texture allocation");
    for (NSUInteger mip = 0; mip < 7; ++mip) {
        NSUInteger size = 64u >> mip, span = 1u << mip;
        OracleColor *data = calloc(size * size, sizeof(*data));
        require(data != NULL, "anisotropy upload allocation");
        for (NSUInteger y = 0; y < size; ++y) {
            float bright = 0;
            for (NSUInteger i = 0; i < span; ++i) if ((y * span + i) % 8 < 2) ++bright;
            float value = bright / (float)span * (float)(resource + 1);
            for (NSUInteger x = 0; x < size; ++x)
                data[y * size + x] = (OracleColor){value, value, value, 1};
        }
        [result replaceRegion:MTLRegionMake2D(0, 0, size, size) mipmapLevel:mip
            withBytes:data bytesPerRow:size * sizeof(*data)];
        free(data);
    }
    return result;
}

static void runAnisotropy(id<MTLDevice> device, id<MTLCommandQueue> queue,
                         NSString *directory, BOOL reference) {
    static const unsigned anisotropies[] = {1, 2, 4, 8, 16};
    NSMutableArray *observations = [NSMutableArray array];
    id<MTLTexture> images[] = {anisotropyTexture(device, 0), anisotropyTexture(device, 1)};
    float extremes[2][2] = {{0}};
    unsigned dispatches = 0;
    for (NSUInteger a = 0; a < sizeof(anisotropies) / sizeof(anisotropies[0]); ++a) {
        unsigned anisotropy = anisotropies[a];
        NSString *label = [NSString stringWithFormat:@"anisotropy-%u", anisotropy];
        NSString *msl = [NSString stringWithFormat:
            @"#include <metal_stdlib>\nusing namespace metal;\n"
             "struct Args { array<texture2d<float>,2> images [[id(0)]]; array<sampler,2> filters [[id(2)]]; };\n"
             "kernel void anisotropy(constant Args &a [[buffer(3)]], device float4 *out [[buffer(0)]], "
             "constant uint &i [[buffer(1)]], texture2d<float> t [[texture(0)]], sampler d [[sampler(0)]]) { "
             "constexpr sampler s(filter::linear, mip_filter::linear, address::clamp_to_edge, max_anisotropy(%u)); "
             "float2 uv(0.5f, 0.5078125f); gradient2d g(float2(0.125f,0.0f),float2(0.0f,0.015625f)); "
             "out[0] = a.images[i].sample(a.filters[i],uv,g); out[1] = t.sample(d,uv,g); out[2] = t.sample(s,uv,g); }\n",
            anisotropy];
        id<MTLComputePipelineState> pso = pipeline(device, msl, @"anisotropy", directory, label);
        NSError *error = nil;
        MTLCompileOptions *options = [MTLCompileOptions new]; options.languageVersion = MTLLanguageVersion2_1;
        id<MTLLibrary> library = [device newLibraryWithSource:msl options:options error:&error];
        id<MTLArgumentEncoder> arguments = [[library newFunctionWithName:@"anisotropy"] newArgumentEncoderWithBufferIndex:3];
        require(arguments && arguments.alignment && kOffset % arguments.alignment == 0,
            "anisotropy argument layout");
        MTLSamplerDescriptor *d = [MTLSamplerDescriptor new];
        d.minFilter = d.magFilter = MTLSamplerMinMagFilterLinear;
        d.mipFilter = MTLSamplerMipFilterLinear;
        d.maxAnisotropy = anisotropy;
        d.supportArgumentBuffers = YES;
        id<MTLSamplerState> samplers[] = {[device newSamplerStateWithDescriptor:d], [device newSamplerStateWithDescriptor:d]};
        require(samplers[0] && samplers[1], "anisotropic state supported");
        id<MTLBuffer> encoded = guarded(device, arguments.encodedLength);
        [arguments setArgumentBuffer:encoded offset:kOffset];
        for (NSUInteger r = 0; r < 2; ++r) {
            [arguments setTexture:images[r] atIndex:r];
            [arguments setSamplerState:samplers[r] atIndex:2 + r];
        }
        NSData *before = [NSData dataWithBytes:encoded.contents length:encoded.length];
        for (uint32_t r = 0; r < 2; ++r) {
            id<MTLBuffer> output = guarded(device, 3 * sizeof(OracleColor));
            id<MTLBuffer> selection = guarded(device, sizeof(r));
            memcpy((uint8_t *)selection.contents + kOffset, &r, sizeof(r));
            id<MTLCommandBuffer> command = [queue commandBuffer];
            id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
            [encoder setComputePipelineState:pso];
            [encoder setBuffer:output offset:kOffset atIndex:0];
            [encoder setBuffer:selection offset:kOffset atIndex:1];
            [encoder setBuffer:encoded offset:kOffset atIndex:3];
            [encoder setTexture:images[r] atIndex:0];
            [encoder setSamplerState:samplers[r] atIndex:0];
            for (NSUInteger n = 0; n < 2; ++n) [encoder useResource:images[n] usage:MTLResourceUsageRead];
            [encoder dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
            [encoder endEncoding];
            completed(command);
            guards(output, 3 * sizeof(OracleColor));
            guards(selection, sizeof(r));
            require(!memcmp((uint8_t *)selection.contents + kOffset, &r, sizeof(r)) &&
                !memcmp(encoded.contents, before.bytes, encoded.length), "anisotropy input bytes unchanged");
            const OracleColor *actual = (const OracleColor *)((uint8_t *)output.contents + kOffset);
            require(oracle_color_close(actual[0], actual[1]) && oracle_color_close(actual[1], actual[2]),
                "anisotropy direct/argument/constexpr outputs agree");
            float value = actual[0].r;
            require(isfinite(value) && value >= 0 && value <= (float)(r + 1) &&
                actual[0].g == value && actual[0].b == value && actual[0].a == 1,
                "anisotropy channel/domain invariant");
            if (anisotropy == 1) {
                require(fabsf(value - 0.25f * (float)(r + 1)) <= 1.0f / 4096,
                    "isotropic 8x1 footprint selects constant mip3");
                extremes[r][0] = value;
            }
            if (anisotropy == 8) extremes[r][1] = value;
            [observations addObject:@{ @"anisotropy": @(anisotropy), @"resource": @(r),
                @"argument": @(value), @"direct": @(actual[1].r), @"constexpr": @(actual[2].r) }];
            printf("SAMPLER_ANISOTROPY_VALUE backend=%s anisotropy=%u resource=%u value=%.9g\n",
                reference ? "reference" : "bundle", anisotropy, r, value);
            fflush(stdout);
            ++dispatches;
        }
    }
    for (unsigned r = 0; r < 2; ++r)
        require(extremes[r][1] - extremes[r][0] >= 0.125f * (float)(r + 1),
            "anisotropic 8x1 footprint recovers stripe resolution over isotropic control");
    NSError *error = nil;
    NSData *json = [NSJSONSerialization dataWithJSONObject:observations options:NSJSONWritingPrettyPrinted error:&error];
    require(json && [json writeToFile:[directory stringByAppendingPathComponent:@"anisotropy-observations.json"] atomically:YES],
        "persist exact anisotropy observations");
    printf("SAMPLER_ANISOTROPY_PASS backend=%s states=1,2,4,8,16 resources=2 dispatches=%u "
        "direct_argument_constexpr=1 isotropic_control=1 stripe_recovery=1 "
        "footprint=implementation-dependent guards=output,argument,selection gpu_execution=1\n",
        reference ? "reference" : "bundle", dispatches);
}
