// Runtime coordinate-domain, descriptor-size and array-layer controls for fragment flow tests.
static BOOL runtimeFlowIsArray(Flow flow) {
    return flow == FlowRuntimeArrayBranch || flow == FlowRuntimeArrayLoop;
}

static NSString *runtimeFlowSource(Flow flow) {
    BOOL arrayed = runtimeFlowIsArray(flow);
    BOOL helper = flow == FlowRuntimeBufferHelper || flow == FlowRuntimeBufferHelperGrad;
    BOOL guarded = flow >= FlowRuntimeGuarded;
    BOOL dynamic = flow >= FlowRuntimeDynamic;
    BOOL effects = flow >= FlowRuntimeEffects;
    BOOL nullable = flow == FlowRuntimeGuardedNull || flow == FlowRuntimeDynamicNull || flow == FlowRuntimeEffectsNull;
    BOOL buffered = flow >= FlowRuntimeBuffer && !guarded;
    BOOL loop = flow == FlowRuntimeLoop || flow == FlowRuntimeArrayLoop;
    NSString *layer = arrayed ? @", control[2u + (uint(position.x) & 1u)]" : @"";
    NSString *first = [NSString stringWithFormat:
        @"args.images[resource].sample(args.filters[filter], position.xy * %@%@)",
        loop ? @"(i ? 0.4f : 0.2f)" : @"0.4f", layer];
    NSString *second = [NSString stringWithFormat:
        @"args.images[resource].sample(args.filters[filter], position.xy * 0.2f%@)", layer];
    NSString *body = loop ? [NSString stringWithFormat:
        @"float4 sum = 0; for (uint i = 0; i < control[0] + 1u; ++i) { "
         "if ((uint(position.x) & 2u) != 0u) sum += %@; else sum += %@; } return sum;", first, second]
        : [NSString stringWithFormat:
        @"if ((uint(position.x) & 2u) != 0u) return %@; return %@;", first, second];
    if (flow == FlowRuntimeReturn)
        body = @"if ((uint(position.x) & 2u) != 0u) return float4(7); "
            "return args.images[control[1] & 1u].sample(args.filters[1], position.xy * 0.2f);";
    if (flow >= FlowRuntimeDiscard) body = exitFlowBody(flow);
    if (buffered || guarded) {
        NSString *gradient = flow == FlowRuntimeBufferGrad || flow == FlowRuntimeBufferHelperGrad || flow == FlowRuntimeGuardedGrad || flow == FlowRuntimeDynamicGrad || flow == FlowRuntimeEffectsGrad ?
            @", gradient2d(float2(scale, 0), float2(0, scale))" : @"";
        NSString *sample = [NSString stringWithFormat:
            @"return args.images[resource].sample(args.filters[filter], position.xy * scale%@);", gradient];
        NSString *effect = effects ? @"effects[2u * effectIndex] += control[0] + 1u; " : @"";
        NSString *highEffect = effects ? [effect stringByAppendingString:
            @"effects[2u * effectIndex + 1u] = as_type<uint>(scales[ulong(control[0]) + 2ul]); "] : @"";
        NSString *lowEffect = effects ? [effect stringByAppendingString:
            @"effects[2u * effectIndex + 1u] = as_type<uint>(scales[3ul - ulong(control[0])]); "] : @"";
        body = [NSString stringWithFormat:
            @"if ((uint(position.x) & 2u) != 0u) { %@float scale = %@; %@ } "
             "%@float scale = %@; %@", highEffect,
            effects ? @"flow_effect_scale(effects, 2ul * ulong(effectIndex) + 1ul)" :
            dynamic ? @"scales[ulong(control[0]) + 2ul]" :
            guarded ? (nullable ? @"scales[3]" : @"as_type<float>(control[3])") :
                (helper ? @"flow_high(reinterpret_cast<constant float *>(&control))" : @"control.high"), sample, lowEffect,
            effects ? @"flow_effect_scale(effects, 2ul * ulong(effectIndex) + 1ul)" :
            dynamic ? @"scales[3ul - ulong(control[0])]" :
            guarded ? (nullable ? @"scales[2]" : @"as_type<float>(control[2])") :
                (helper ? @"flow_low(reinterpret_cast<constant float *>(&control))" : @"control.low"), sample];
        if (nullable)
            body = [@"if (control[0] == 0u) return float4(7); " stringByAppendingString:body];
        if (effects)
            body = [@"uint effectIndex = uint(position.y) * 8u + uint(position.x); " stringByAppendingString:body];
    }
    return [NSString stringWithFormat:
        @"#include <metal_stdlib>\nusing namespace metal;\n"
         "struct Args { array<%@<float>, 2> images [[id(0)]]; array<sampler, 2> filters [[id(2)]]; };\n"
         "%@\n"
         "%@\n"
         "vertex float4 flow_vertex(uint vid [[vertex_id]]) { float2 p = float2(float((vid << 1u) & 2u), float(vid & 2u)); return float4(p * 2.0f - 1.0f, 0, 1); }\n"
         "fragment float4 flow_fragment(float4 position [[position]], constant Args &args [[buffer(3)]], %@ [[buffer(1)]]%@%@) { %@ %@ }\n",
        arrayed ? @"texture2d_array" : @"texture2d",
        buffered ? @"struct Control { uint state; uint resource; float low; float high; };" : @"",
        helper ? @"__attribute__((noinline)) float flow_high(constant float *values) { return values[3]; }\n"
            "__attribute__((noinline)) float flow_low(constant float *values) { return values[2]; }" :
            effects ? @"__attribute__((noinline)) float flow_effect_scale(device uint *values, ulong index) { return as_type<float>(values[index]); }" : @"",
        buffered ? @"constant Control &control" : @"constant uint *control",
        nullable || dynamic ? @", constant float *scales [[buffer(2)]]" : @"",
        effects ? @", device uint *effects [[buffer(4)]]" : @"",
        buffered ? @"uint resource = (control.resource ^ uint(position.x)) & 1u; uint filter = (control.state ^ uint(position.x)) & 1u;" :
            @"uint resource = (control[1] ^ uint(position.x)) & 1u; uint filter = (control[0] ^ uint(position.x)) & 1u;",
        body];
}

static float runtimeExpected(Flow flow, uint32_t state, uint32_t resource, uint32_t x) {
    if ((flow == FlowRuntimeGuardedNull || flow == FlowRuntimeDynamicNull || flow == FlowRuntimeEffectsNull) && !state) return 7.0f;
    if (flow == FlowRuntimeReturn)
        return (x & 2u) ? 7.0f : (float)((resource ? 20u : 10u) + resource + 1u);
    uint32_t image = (resource ^ x) & 1u;
    BOOL normalized = ((state ^ x) & 1u) != 0u;
    uint32_t layer = runtimeFlowIsArray(flow) ? (state ^ x) & 1u : 0u;
    uint32_t base = (image ? 20u : 10u) + 100u * layer;
    uint32_t low = normalized ? image + 1u : 0u;
    uint32_t high = normalized ? image + 2u : 0u;
    if (flow >= FlowRuntimeDynamic)
        return (float)(base + (((x & 2u) != 0u) == (state != 0u) ? high : low));
    if (flow == FlowRuntimeDiscard) return (x & 2u) ? kTargetGuard : (float)(base + low);
    if (flow == FlowRuntimeContinue || flow == FlowRuntimeContinueGrad)
        return (float)(((x & 2u) ? 0u : base + low) + (state + 1u) * (base + high));
    if (flow == FlowRuntimeMathLoop || flow == FlowRuntimeMathLoopGrad)
        return (float)(base + low + (state + 1u) * (base + high));
    if (flow == FlowRuntimeLoop || flow == FlowRuntimeArrayLoop)
        return (float)(base + low + (state ? base + ((x & 2u) ? high : low) : 0u));
    return (float)(base + ((x & 2u) ? high : low));
}

static id<MTLTexture> runtimeImage(id<MTLDevice> device, NSUInteger size,
                                  uint32_t base, BOOL arrayed) {
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
        MTLPixelFormatRGBA32Float width:size height:size mipmapped:YES];
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.usage = MTLTextureUsageShaderRead;
    if (arrayed) {
        descriptor.textureType = MTLTextureType2DArray;
        descriptor.arrayLength = 2;
    }
    id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
    require(texture != nil, @"runtime coordinate image");
    float texels[16 * 16 * 4];
    for (NSUInteger layer = 0; layer < descriptor.arrayLength; ++layer) {
        for (NSUInteger level = 0; level < texture.mipmapLevelCount; ++level) {
            NSUInteger extent = size >> level;
            for (NSUInteger i = 0; i < extent * extent * 4; ++i)
                texels[i] = (float)(base + 100u * layer + level);
            [texture replaceRegion:MTLRegionMake2D(0, 0, extent, extent) mipmapLevel:level
                slice:layer withBytes:texels bytesPerRow:extent * 4 * sizeof(float)
                bytesPerImage:extent * extent * 4 * sizeof(float)];
        }
    }
    return texture;
}

static id<MTLSamplerState> runtimePixelSampler(id<MTLDevice> device) {
    MTLSamplerDescriptor *descriptor = [MTLSamplerDescriptor new];
    descriptor.normalizedCoordinates = NO;
    descriptor.minFilter = MTLSamplerMinMagFilterNearest;
    descriptor.magFilter = MTLSamplerMinMagFilterNearest;
    descriptor.mipFilter = MTLSamplerMipFilterNotMipmapped;
    descriptor.sAddressMode = MTLSamplerAddressModeClampToEdge;
    descriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
    descriptor.rAddressMode = MTLSamplerAddressModeClampToEdge;
    descriptor.lodMinClamp = 0;
    descriptor.lodMaxClamp = 0;
    descriptor.supportArgumentBuffers = YES;
    id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:descriptor];
    require(sampler != nil, @"runtime pixel sampler");
    return sampler;
}
