#include <metal_stdlib>
using namespace metal;

struct SamplerArgs {
    array<sampler, 2> filters [[id(0)]];
};

// Compile each control separately with -std=macos-metal2.1. The default translation unit is legal.
#if defined(CONTROL_THREAD)
kernel void sampler_copy_control(device SamplerArgs &dst [[buffer(0)]],
                                 sampler input [[sampler(0)]]) {
    dst.filters[0] = input;
}
#elif defined(CONTROL_CONSTEXPR)
kernel void sampler_copy_control(device SamplerArgs &dst [[buffer(0)]]) {
    constexpr sampler local(coord::normalized, filter::nearest);
    dst.filters[0] = local;
}
#else
kernel void sampler_copy_device(device SamplerArgs &dst [[buffer(0)]],
                                const device SamplerArgs &src [[buffer(1)]],
                                uint index [[thread_position_in_grid]]) {
    dst.filters[index & 1] = src.filters[1 - (index & 1)];
}

kernel void sampler_copy_constant(device SamplerArgs &dst [[buffer(0)]],
                                  constant SamplerArgs &src [[buffer(1)]],
                                  uint index [[thread_position_in_grid]]) {
    dst.filters[index & 1] = src.filters[1 - (index & 1)];
}
#endif
