// Quad-uniform exits retain defined source derivatives while varying across the draw.
static NSString *exitFlowBody(Flow flow) {
    if (flow == FlowRuntimeDiscard)
        return @"if ((uint(position.x) & 2u) != 0u) discard_fragment(); "
            "return args.images[resource].sample(args.filters[filter], position.xy * 0.2f);";
    if (flow == FlowRuntimeAffine || flow == FlowRuntimeAffineGrad) {
        NSString *gradient = flow == FlowRuntimeAffineGrad ?
            @", gradient2d(float2(0, scale), float2(scale, 0))" : @"";
        NSString *sample = [NSString stringWithFormat:
            @"float2 uv = fma(position.yx + float2(1.125f, -2.375f), float2(scale), float2(0.375f, 0.625f)); "
             "return args.images[resource].sample(args.filters[filter], uv%@);", gradient];
        return [NSString stringWithFormat:
            @"if ((uint(position.x) & 2u) != 0u) { float scale = 0.4f; %@ } "
             "float scale = 0.2f; %@", sample, sample];
    }
    if (flow == FlowRuntimeMathLoop || flow == FlowRuntimeMathLoopGrad) {
        NSString *gradient = flow == FlowRuntimeMathLoopGrad ?
            @", gradient2d(float2(scale, 0), float2(0, scale))" : @"";
        return [NSString stringWithFormat:
            @"float4 sum = 0; uint limit = uint(clamp(float(control[0]) + 2.25f, 1.0f, 3.75f)); "
             "for (uint i = 0; i < limit; ++i) { float scale = i ? 0.4f : 0.2f; "
             "sum += args.images[resource].sample(args.filters[filter], position.xy * scale%@); } "
             "return sum;", gradient];
    }
    NSString *gradient = flow == FlowRuntimeContinueGrad ?
        @", gradient2d(float2(scale, 0), float2(0, scale))" : @"";
    return [NSString stringWithFormat:
        @"float4 sum = 0; for (uint i = 0; i < control[0] + 2u; ++i) { "
         "if (i == 0u && (uint(position.x) & 2u) != 0u) continue; "
         "float scale = i ? 0.4f : 0.2f; "
         "sum += args.images[resource].sample(args.filters[filter], position.xy * scale%@); "
         "} return sum;", gradient];
}
