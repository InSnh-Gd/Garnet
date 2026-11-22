#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RDNA4_ARGUMENT_TEST_PREFIX "ARGUMENT_FRAGMENT_FLOW"
#include "test-argument-render-support.h"

typedef enum {
    FlowBranch, FlowLoop, FlowHelper, FlowReturn, FlowDiscard, FlowMerge,
    FlowQuadBranch, FlowQuadLoop,
    FlowQuadBranchLanes, FlowQuadLoopLanes,
    FlowRuntimeBranch, FlowRuntimeLoop, FlowRuntimeArrayBranch, FlowRuntimeArrayLoop,
    FlowRuntimeReturn,
    FlowRuntimeDiscard, FlowRuntimeContinue, FlowRuntimeContinueGrad,
    FlowRuntimeAffine, FlowRuntimeAffineGrad,
    FlowRuntimeMathLoop, FlowRuntimeMathLoopGrad,
    FlowRuntimeBuffer, FlowRuntimeBufferGrad,
    FlowRuntimeBufferHelper, FlowRuntimeBufferHelperGrad,
    FlowRuntimeGuarded, FlowRuntimeGuardedGrad, FlowRuntimeGuardedNull,
    FlowRuntimeDynamic, FlowRuntimeDynamicGrad, FlowRuntimeDynamicNull,
    FlowRuntimeEffects, FlowRuntimeEffectsGrad, FlowRuntimeEffectsNull,
} Flow;

static const char *flowNames[] = {
    "uniform-branch", "uniform-loop", "helper-calls", "uniform-return",
    "uniform-discard", "varying-merge", "quad-uniform-branch", "quad-uniform-loop",
    "quad-uniform-branch-lane-resources", "quad-uniform-loop-lane-resources",
    "runtime-coordinate-branch", "runtime-coordinate-loop",
    "runtime-array-coordinate-branch", "runtime-array-coordinate-loop",
    "runtime-coordinate-return",
    "runtime-coordinate-discard", "runtime-coordinate-continue", "runtime-coordinate-continue-grad",
    "runtime-coordinate-affine", "runtime-coordinate-affine-grad",
    "runtime-coordinate-math-loop", "runtime-coordinate-math-loop-grad",
    "runtime-coordinate-buffer", "runtime-coordinate-buffer-grad",
    "runtime-coordinate-buffer-helper", "runtime-coordinate-buffer-helper-grad",
    "runtime-coordinate-guarded", "runtime-coordinate-guarded-grad", "runtime-coordinate-guarded-null",
    "runtime-coordinate-dynamic", "runtime-coordinate-dynamic-grad", "runtime-coordinate-dynamic-null",
    "runtime-coordinate-effects", "runtime-coordinate-effects-grad", "runtime-coordinate-effects-null",
};

#include "test-argument-exit-flow.inc"
#include "test-argument-runtime-flow.inc"

static NSString *flowSource(Flow flow) {
    if (flow >= FlowRuntimeBranch) return runtimeFlowSource(flow);
    NSString *sample = @"args.images[control[1] & 1u].sample(args.filters[1], position.xy * 0.2f)";
    NSString *body;
    switch (flow) {
    case FlowBranch:
        body = [NSString stringWithFormat:
            @"if (control[0]) return args.images[control[1] & 1u].sample(args.filters[1], position.xy * 0.4f); return %@;", sample];
        break;
    case FlowLoop:
        body = [NSString stringWithFormat:
            @"float4 sum = 0; for (uint i = 0; i < control[0] + 1u; ++i) sum += %@; return sum;", sample];
        break;
    case FlowHelper:
        body = @"if (control[0]) return flow_read(args.images[control[1] & 1u], args.filters[1], position.xy * 0.4f); "
            "return flow_read(args.images[control[1] & 1u], args.filters[1], position.xy * 0.2f);";
        break;
    case FlowReturn:
        body = [NSString stringWithFormat:@"if (control[0]) return float4(7); return %@;", sample];
        break;
    case FlowDiscard:
        body = [NSString stringWithFormat:@"if (control[0]) discard_fragment(); return %@;", sample];
        break;
    case FlowMerge:
        body = [NSString stringWithFormat:
            @"float4 extra; if ((uint(position.x) & 1u) != 0u) "
             "extra = args.images[control[1] & 1u].sample(args.filters[1], position.xy * 0.2f, level(0.0f)); "
             "else extra = args.images[control[1] & 1u].sample(args.filters[1], position.xy * 0.2f, level(1.0f)); "
             "return (%@) + extra;", sample];
        break;
    case FlowQuadBranch:
    case FlowQuadBranchLanes:
        body = @"constexpr sampler fixed(coord::normalized, address::clamp_to_edge, filter::nearest, mip_filter::nearest); "
            "uint resource = (control[1] ^ uint(position.x)) & 1u; "
            "if ((uint(position.x) & 2u) != 0u) return args.images[resource].sample(fixed, position.xy * 0.4f); "
            "return args.images[resource].sample(fixed, position.xy * 0.2f);";
        break;
    case FlowQuadLoop:
    case FlowQuadLoopLanes:
        body = @"constexpr sampler fixed(coord::normalized, address::clamp_to_edge, filter::nearest, mip_filter::nearest); "
            "uint resource = (control[1] ^ uint(position.x)) & 1u; float4 sum = 0; "
            "for (uint i = 0; i < control[0] + 1u; ++i) { "
            "if ((uint(position.x) & 2u) != 0u) sum += args.images[resource].sample(fixed, position.xy * (i ? 0.4f : 0.2f)); "
            "else sum += args.images[resource].sample(fixed, position.xy * 0.2f); } return sum;";
        break;
    default:
        require(NO, @"unknown structured flow");
        return nil;
    }
    if (flow == FlowQuadBranch || flow == FlowQuadLoop)
        body = [body stringByReplacingOccurrencesOfString:@"(control[1] ^ uint(position.x)) & 1u"
            withString:@"control[1] & 1u"];
    return [NSString stringWithFormat:
        @"#include <metal_stdlib>\nusing namespace metal;\n"
         "struct Args { array<texture2d<float>, 2> images [[id(0)]]; array<sampler, 2> filters [[id(2)]]; };\n"
         "vertex float4 flow_vertex(uint vid [[vertex_id]]) { float2 p = float2(float((vid << 1u) & 2u), float(vid & 2u)); return float4(p * 2.0f - 1.0f, 0, 1); }\n"
         "__attribute__((noinline)) float4 flow_read(texture2d<float> image, sampler filter, float2 uv) { return image.sample(filter, uv); }\n"
         "fragment float4 flow_fragment(float4 position [[position]], constant Args &args [[buffer(3)]], constant uint *control [[buffer(1)]]) { %@ }\n", body];
}

static float expected(Flow flow, uint32_t state, uint32_t resource, uint32_t x) {
    if (flow >= FlowRuntimeBranch) return runtimeExpected(flow, state, resource, x);
    if (flow == FlowQuadBranch || flow == FlowQuadLoop ||
        flow == FlowQuadBranchLanes || flow == FlowQuadLoopLanes) {
        BOOL lanes = flow == FlowQuadBranchLanes || flow == FlowQuadLoopLanes;
        uint32_t base = ((resource ^ (lanes ? x : 0u)) & 1u) ? 20u : 10u;
        if (flow == FlowQuadBranch || flow == FlowQuadBranchLanes)
            return (float)(base + ((x & 2u) ? 2u : 1u));
        return (float)(base + 1u + (state ? base + ((x & 2u) ? 2u : 1u) : 0u));
    }
    // 8*0.2 and 8*0.4 stay away from every nearest-mip boundary, including Vulkan's rho range.
    float value = (float)((resource ? 20u : 10u) +
        ((flow == FlowBranch || flow == FlowHelper) && state ? 2u : 1u));
    if (flow == FlowLoop) value *= (float)(state + 1u);
    if (flow == FlowReturn && state) value = 7;
    if (flow == FlowDiscard && state) value = kTargetGuard;
    if (flow == FlowMerge) value += (float)((resource ? 20u : 10u) + ((x & 1u) ? 0u : 1u));
    return value;
}

static void execute(Flow flow, uint32_t state, uint32_t resource,
    id<MTLDevice> device, id<MTLCommandQueue> queue, id<MTLRenderPipelineState> pipeline,
    id<MTLBuffer> arguments, NSUInteger offset, NSUInteger length, NSData *argumentSnapshot,
    id<MTLBuffer> control, id<MTLTexture> image0, id<MTLTexture> image1) {
    uint32_t values[] = {state, resource, state, state ^ 1u};
    if (flow >= FlowRuntimeBuffer) {
        const float scales[] = {0.2f, 0.4f};
        memcpy(values + 2, scales, sizeof(scales));
    }
    memcpy((uint8_t *)control.contents + kControlOffset, values, sizeof(values));
    NSData *controlSnapshot = [NSData dataWithBytes:control.contents length:control.length];
    id<MTLBuffer> effects = nil;
    if (flow >= FlowRuntimeEffects) {
        effects = newSharedBuffer(device, kControlOffset + 2 * kTargetSize * kTargetSize * sizeof(uint32_t) + kArgumentTail,
            0xe7, @"flow effects");
        uint32_t *counts = (uint32_t *)((uint8_t *)effects.contents + kControlOffset);
        for (uint32_t i = 0; i < kTargetSize * kTargetSize; ++i) {
            counts[2 * i] = 0x1234u + 17u * i;
            counts[2 * i + 1] = 0x3f4ccccdu;
        }
    }
    id<MTLTexture> target = newTarget(device);
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(kTargetGuard, kTargetGuard, kTargetGuard, kTargetGuard);
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLRenderCommandEncoder> draw = [command renderCommandEncoderWithDescriptor:pass];
    require(command && draw, @"structured flow encoder");
    [draw setRenderPipelineState:pipeline];
    [draw setViewport:(MTLViewport){kViewportOrigin, kViewportOrigin, kViewportSize, kViewportSize, 0, 1}];
    [draw setScissorRect:(MTLScissorRect){kViewportOrigin, kViewportOrigin, kViewportSize, kViewportSize}];
    [draw setFragmentBuffer:arguments offset:offset atIndex:3];
    [draw setFragmentBuffer:control offset:kControlOffset atIndex:1];
    if (flow == FlowRuntimeGuardedNull || flow >= FlowRuntimeDynamic) {
        BOOL absent = (flow == FlowRuntimeGuardedNull || flow == FlowRuntimeDynamicNull || flow == FlowRuntimeEffectsNull) && !state;
        [draw setFragmentBuffer:absent ? nil : control offset:absent ? 0 : kControlOffset atIndex:2];
    }
    if (effects) [draw setFragmentBuffer:effects offset:kControlOffset atIndex:4];
    [draw useResource:image0 usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
    [draw useResource:image1 usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
    [draw drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [draw endEncoding];
    [command commit];
    [command waitUntilCompleted];
    require(command.status == MTLCommandBufferStatusCompleted && !command.error,
        command.error.description ?: @"structured flow completion");
    requireGuard(arguments, offset, length, kArgumentGuard, @"flow argument guards");
    requireGuard(control, kControlOffset, sizeof(values), kControlGuard, @"flow control guards");
    require(!memcmp(arguments.contents, argumentSnapshot.bytes, arguments.length), @"argument storage unchanged");
    require(!memcmp(control.contents, controlSnapshot.bytes, control.length), @"control storage unchanged");
    if (effects) {
        requireGuard(effects, kControlOffset, 2 * kTargetSize * kTargetSize * sizeof(uint32_t), 0xe7, @"flow effect guards");
        const uint32_t *counts = (const uint32_t *)((const uint8_t *)effects.contents + kControlOffset);
        for (uint32_t y = 0; y < kTargetSize; ++y) for (uint32_t x = 0; x < kTargetSize; ++x) {
            BOOL writes = x >= kViewportOrigin && x < kViewportOrigin + kViewportSize &&
                y >= kViewportOrigin && y < kViewportOrigin + kViewportSize &&
                !(flow == FlowRuntimeEffectsNull && !state);
            uint32_t wanted = 0x1234u + 17u * (y * kTargetSize + x) + (writes ? state + 1u : 0u);
            require(counts[2 * (y * kTargetSize + x)] == wanted, [NSString stringWithFormat:
                @"flow=%s effect pixel=%u,%u actual=%u expected=%u", flowNames[flow], x, y, counts[2 * (y * kTargetSize + x)], wanted]);
            uint32_t wantedBits = writes ? values[(((x & 2u) != 0u) == (state != 0u)) ? 3 : 2] : 0x3f4ccccdu;
            require(counts[2 * (y * kTargetSize + x) + 1] == wantedBits, @"flow effect scale word");
        }
    }
    float pixels[kTargetSize * kTargetSize * 4];
    [target getBytes:pixels bytesPerRow:kTargetSize * 4 * sizeof(float)
        fromRegion:MTLRegionMake2D(0, 0, kTargetSize, kTargetSize) mipmapLevel:0];
    for (uint32_t y = 0; y < kTargetSize; ++y) {
        for (uint32_t x = 0; x < kTargetSize; ++x) {
            BOOL inside = x >= kViewportOrigin && x < kViewportOrigin + kViewportSize &&
                y >= kViewportOrigin && y < kViewportOrigin + kViewportSize;
            float wanted = inside ? expected(flow, state, resource, x) : kTargetGuard;
            for (uint32_t channel = 0; channel < 4; ++channel) {
                float actual = pixels[4 * (y * kTargetSize + x) + channel];
                require(actual == wanted, [NSString stringWithFormat:
                    @"flow=%s state=%u resource=%u pixel=%u,%u channel=%u actual=%g expected=%g",
                    flowNames[flow], state, resource, x, y, channel, actual, wanted]);
            }
        }
    }
    printf("ARGUMENT_FRAGMENT_FLOW_CASE_PASS flow=%s state=%u resource=%u pixels=64 channels=4 guards=exact gpu_execution=1\n",
        flowNames[flow], state, resource);
}

int main(int argc, const char **argv) {
    if (argc != 3 && argc != 4) return 2;
    int selected = -1;
    if (argc == 4) {
        for (unsigned i = 0; i < sizeof(flowNames) / sizeof(flowNames[0]); ++i)
            if (!strcmp(argv[3], flowNames[i])) selected = (int)i;
        if (selected < 0) return 2;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(110);
    @autoreleasepool {
        BOOL reference = !strcmp(argv[1], "--reference");
        NSError *error = nil;
        id<MTLDevice> device;
        if (reference) device = MTLCreateSystemDefaultDevice();
        else {
            NSBundle *bundle = [NSBundle bundleWithPath:@(argv[1])];
            require([bundle loadAndReturnError:&error], error.description ?: @"bundle load");
            device = [[[bundle classNamed:@"GarnetDevice"] alloc] init];
        }
        require(device != nil, @"structured flow device");
        NSString *directory = @(argv[2]);
        require([[NSFileManager defaultManager] createDirectoryAtPath:directory withIntermediateDirectories:YES
            attributes:nil error:&error], @"structured flow output directory");
        id<MTLCommandQueue> queue = [device newCommandQueue];
        require(queue != nil, @"structured flow queue");
        unsigned draws = 0, flows = 0;
        for (unsigned i = 0; i < sizeof(flowNames) / sizeof(flowNames[0]); ++i) {
            if (selected >= 0 && i != (unsigned)selected) continue;
            @autoreleasepool {
                Flow flow = (Flow)i;
                NSString *folder = [directory stringByAppendingPathComponent:@(flowNames[i])];
                require([[NSFileManager defaultManager] createDirectoryAtPath:folder withIntermediateDirectories:YES
                    attributes:nil error:&error], @"flow case directory");
                NSString *source = flowSource(flow);
                require([source writeToFile:[folder stringByAppendingPathComponent:@"source.metal"]
                    atomically:YES encoding:NSUTF8StringEncoding error:&error], @"flow authored source capture");
                MTLCompileOptions *options = [MTLCompileOptions new];
                options.languageVersion = MTLLanguageVersion2_1;
                id<MTLLibrary> library = [device newLibraryWithSource:source options:options error:&error];
                require(library != nil, error.description ?: @"flow source compilation");
                id<MTLFunction> vertex = [library newFunctionWithName:@"flow_vertex"];
                id<MTLFunction> fragment = [library newFunctionWithName:@"flow_fragment"];
                require(vertex && fragment, @"flow functions");
                writeAIR(vertex, folder);
                writeAIR(fragment, folder);
                id<MTLRenderPipelineState> pipeline = newPipeline(device, vertex, fragment, @"flow pipeline", &error);
                id<MTLArgumentEncoder> encoder = [fragment newArgumentEncoderWithBufferIndex:3];
                require(encoder && encoder.encodedLength && encoder.alignment, @"flow argument layout");
                NSUInteger offset = alignUp(kArgumentOffset, encoder.alignment);
                id<MTLBuffer> arguments = newSharedBuffer(device, offset + encoder.encodedLength + kArgumentTail,
                    kArgumentGuard, @"flow arguments");
                id<MTLTexture> images[] = {newMipTexture(device, 10, @"flow image0"), newMipTexture(device, 20, @"flow image1")};
                id<MTLSamplerState> samplers[] = {newNearestSampler(device, 0, @"flow clamp0"), newNearestSampler(device, 3, @"flow max3")};
                if (flow >= FlowRuntimeBranch) {
                    BOOL arrayed = runtimeFlowIsArray(flow);
                    images[0] = runtimeImage(device, 8, 10, arrayed);
                    images[1] = runtimeImage(device, 16, 20, arrayed);
                    samplers[0] = runtimePixelSampler(device);
                    samplers[1] = newNearestSampler(device, 4, @"runtime normalized max4");
                }
                [encoder setArgumentBuffer:arguments offset:offset];
                [encoder setTextures:images withRange:NSMakeRange(0, 2)];
                [encoder setSamplerStates:samplers withRange:NSMakeRange(2, 2)];
                NSData *snapshot = [NSData dataWithBytes:arguments.contents length:arguments.length];
                id<MTLBuffer> control = newSharedBuffer(device, kControlOffset + 4 * sizeof(uint32_t) + kControlTail,
                    kControlGuard, @"flow control");
                for (uint32_t state = 0; state < 2; ++state)
                    for (uint32_t resource = 0; resource < 2; ++resource) {
                        execute(flow, state, resource, device, queue, pipeline, arguments, offset,
                            encoder.encodedLength, snapshot, control, images[0], images[1]);
                        ++draws;
                    }
                ++flows;
            }
        }
        printf("ARGUMENT_FRAGMENT_FLOW_PASS backend=%s flows=%u draws=%u msl=2.1 cpu_oracle=1 nonzero_resource=1 guards=target,argument,control gpu_execution=1\n",
            reference ? "reference" : "bundle", flows, draws);
    }
    return 0;
}
