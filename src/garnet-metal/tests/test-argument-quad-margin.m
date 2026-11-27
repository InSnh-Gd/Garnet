#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RDNA4_ARGUMENT_TEST_PREFIX "ARGUMENT_QUAD_MARGIN"
#include "test-argument-render-support.h"

enum { kModeCount = 3, kSamplingCount = 3 };

typedef enum {
    QuadMarginModeImageOnly = 0,
    QuadMarginModeSamplerOnly = 1,
    QuadMarginModeBoth = 2,
} QuadMarginMode;

typedef enum {
    QuadMarginSamplingImplicit = 0,
    QuadMarginSamplingBias1 = 1,
    QuadMarginSamplingGradient = 2,
    QuadMarginSamplingLod1 = 3,
} QuadMarginSampling;

static const char *modeName(QuadMarginMode mode) {
    if (mode == QuadMarginModeImageOnly) return "image-only";
    if (mode == QuadMarginModeSamplerOnly) return "sampler-only";
    return "both";
}

static const char *samplingName(QuadMarginSampling sampling) {
    if (sampling == QuadMarginSamplingBias1) return "bias1";
    if (sampling == QuadMarginSamplingGradient) return "gradient";
    if (sampling == QuadMarginSamplingLod1) return "lod1";
    return "implicit";
}

static NSString *validSource(void) {
    return @"#include <metal_stdlib>\n"
        "using namespace metal;\n"
        "struct Args {\n"
        "  array<texture2d<float>, 2> images [[id(0)]];\n"
        "  array<sampler, 2> filters [[id(2)]];\n"
        "};\n"
        "uint quad_margin_image_index(uint mode, uint flip, float2 p) {\n"
        "  if (mode == 0u) return uint(p.x) & 1u;\n"
        "  if (mode == 1u) return flip & 1u;\n"
        "  return (uint(p.x) & 1u) ^ (flip & 1u);\n"
        "}\n"
        "uint quad_margin_sampler_index(uint mode, uint flip, float2 p) {\n"
        "  if (mode == 0u) return flip & 1u;\n"
        "  return uint(p.y) & 1u;\n"
        "}\n"
        "vertex float4 quad_margin_vertex(uint vid [[vertex_id]]) {\n"
        "  float2 p = float2(float((vid << 1u) & 2u), float(vid & 2u));\n"
        "  return float4(p * 2.0f - 1.0f, 0.0f, 1.0f);\n"
        "}\n"
        "fragment float4 quad_margin_implicit(\n"
        "    float4 position [[position]],\n"
        "    constant Args &args [[buffer(3)]],\n"
        "    constant uint *control [[buffer(1)]]) {\n"
        "  uint imageIndex = quad_margin_image_index(control[0], control[1],\n"
        "      position.xy);\n"
        "  uint samplerIndex = quad_margin_sampler_index(control[0], control[1],\n"
        "      position.xy);\n"
        "  return args.images[imageIndex].sample(args.filters[samplerIndex],\n"
        "      position.xy * 0.2f);\n"
        "}\n"
        "fragment float4 quad_margin_bias1(\n"
        "    float4 position [[position]],\n"
        "    constant Args &args [[buffer(3)]],\n"
        "    constant uint *control [[buffer(1)]]) {\n"
        "  uint imageIndex = quad_margin_image_index(control[0], control[1],\n"
        "      position.xy);\n"
        "  uint samplerIndex = quad_margin_sampler_index(control[0], control[1],\n"
        "      position.xy);\n"
        "  return args.images[imageIndex].sample(args.filters[samplerIndex],\n"
        "      position.xy * 0.2f, bias(1.0f));\n"
        "}\n"
        "fragment float4 quad_margin_gradient(\n"
        "    float4 position [[position]],\n"
        "    constant Args &args [[buffer(3)]],\n"
        "    constant uint *control [[buffer(1)]]) {\n"
        "  uint imageIndex = quad_margin_image_index(control[0], control[1],\n"
        "      position.xy);\n"
        "  uint samplerIndex = quad_margin_sampler_index(control[0], control[1],\n"
        "      position.xy);\n"
        "  return args.images[imageIndex].sample(args.filters[samplerIndex],\n"
        "      position.xy * 0.2f,\n"
        "      gradient2d(float2(0.2f, 0.0f), float2(0.0f, 0.2f)));\n"
        "}\n";
}

static NSString *controlSource(void) {
    // Compile separately so the original source and captured AIR stay comparable.
    return [validSource() stringByAppendingString:
        @"struct CoordinateArgs {\n"
        "  texture2d<float> image [[id(0)]];\n"
        "  sampler filter [[id(1)]];\n"
        "};\n"
        "fragment float4 quad_margin_coordinate(\n"
        "    float4 position [[position]],\n"
        "    constant CoordinateArgs &args [[buffer(3)]]) {\n"
        "  return args.image.sample(args.filter, position.xy * 0.2f);\n"
        "}\n"
        "fragment float4 quad_margin_lod1(\n"
        "    float4 position [[position]],\n"
        "    constant Args &args [[buffer(3)]],\n"
        "    constant uint *control [[buffer(1)]]) {\n"
        "  uint imageIndex = quad_margin_image_index(control[0], control[1],\n"
        "      position.xy);\n"
        "  uint samplerIndex = quad_margin_sampler_index(control[0], control[1],\n"
        "      position.xy);\n"
        "  return args.images[imageIndex].sample(args.filters[samplerIndex],\n"
        "      position.xy * 0.2f, level(1.0f));\n"
        "}\n"];
}

static uint32_t imageIndex(QuadMarginMode mode, uint32_t flip,
                           uint32_t x, uint32_t y) {
    (void)y;
    if (mode == QuadMarginModeImageOnly) return x & 1u;
    if (mode == QuadMarginModeSamplerOnly) return flip & 1u;
    return (x & 1u) ^ (flip & 1u);
}

static uint32_t samplerIndex(QuadMarginMode mode, uint32_t flip,
                             uint32_t x, uint32_t y) {
    (void)x;
    if (mode == QuadMarginModeImageOnly) return flip & 1u;
    return y & 1u;
}

static uint32_t expectedMip(uint32_t sampler, QuadMarginSampling sampling) {
    /*
     * d(coord)/dx = 0.2, so rho is in [1.6, 2.2627] and implicit LOD is
     * [0.678, 1.178] without bias, [1.678, 2.178] with bias 1. Nearest mip
     * selection therefore has at least 0.178 LOD margin from every boundary:
     * B selects 1 or 2. A clamps every case to mip 0.
     */
    if (sampler == 0) return 0;
    return sampling == QuadMarginSamplingBias1 ? 2 : 1;
}

static void runCase(id<MTLDevice> device, id<MTLCommandQueue> queue,
                    id<MTLRenderPipelineState> pipeline,
                    id<MTLBuffer> arguments, NSUInteger argumentOffset,
                    NSUInteger argumentLength, NSData *argumentSnapshot,
                    id<MTLBuffer> control, NSUInteger controlOffset,
                    NSUInteger controlLength,
                    id<MTLTexture> image0, id<MTLTexture> image1,
                    QuadMarginMode mode, QuadMarginSampling sampling,
                    uint32_t flip, BOOL coordinateOnly) {
    uint32_t controlValues[2] = {(uint32_t)mode, flip};
    memcpy((uint8_t *)control.contents + controlOffset, controlValues,
        sizeof controlValues);
    requireGuard(control, controlOffset, controlLength, kControlGuard,
        @"quad margin control");
    NSData *controlSnapshot = [NSData dataWithBytes:control.contents
        length:control.length];

    id<MTLTexture> target = newTarget(device);
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor =
        MTLClearColorMake(kTargetGuard, kTargetGuard, kTargetGuard, kTargetGuard);

    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLRenderCommandEncoder> draw =
        [command renderCommandEncoderWithDescriptor:pass];
    require(command != nil && draw != nil,
        @"quad margin render encoder unavailable");
    [draw setRenderPipelineState:pipeline];
    [draw setViewport:(MTLViewport){
        (double)kViewportOrigin, (double)kViewportOrigin,
        (double)kViewportSize, (double)kViewportSize, 0.0, 1.0 }];
    [draw setScissorRect:(MTLScissorRect){
        kViewportOrigin, kViewportOrigin, kViewportSize, kViewportSize }];
    [draw setFragmentBuffer:arguments offset:argumentOffset atIndex:3];
    [draw setFragmentBuffer:control offset:controlOffset atIndex:1];
    [draw useResource:image0 usage:MTLResourceUsageRead
        stages:MTLRenderStageFragment];
    [draw useResource:image1 usage:MTLResourceUsageRead
        stages:MTLRenderStageFragment];
    [draw drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [draw endEncoding];
    [command commit];
    [command waitUntilCompleted];
    require(command.status == MTLCommandBufferStatusCompleted &&
        command.error == nil,
        command.error.description ?: @"quad margin execution failed");

    requireGuard(arguments, argumentOffset, argumentLength, kArgumentGuard,
        @"quad margin arguments");
    require(memcmp(argumentSnapshot.bytes, arguments.contents,
        arguments.length) == 0, @"GPU changed quad margin arguments");
    requireGuard(control, controlOffset, controlLength, kControlGuard,
        @"quad margin control after GPU");
    require(memcmp(controlSnapshot.bytes, control.contents,
        control.length) == 0, @"GPU changed quad margin control");

    float pixels[kTargetSize * kTargetSize * 4];
    [target getBytes:pixels bytesPerRow:kTargetSize * 4 * sizeof(float)
        fromRegion:MTLRegionMake2D(0, 0, kTargetSize, kTargetSize)
        mipmapLevel:0];
    for (uint32_t y = 0; y < kTargetSize; ++y) {
        for (uint32_t x = 0; x < kTargetSize; ++x) {
            BOOL inside = x >= kViewportOrigin &&
                x < kViewportOrigin + kViewportSize &&
                y >= kViewportOrigin &&
                y < kViewportOrigin + kViewportSize;
            float expected = kTargetGuard;
            if (inside) {
                uint32_t image = coordinateOnly ? 1u :
                    imageIndex(mode, flip, x, y);
                uint32_t sampler = coordinateOnly ? 1u :
                    samplerIndex(mode, flip, x, y);
                expected = (float)((image ? 20u : 10u) +
                    expectedMip(sampler, sampling));
            }
            for (NSUInteger channel = 0; channel < 4; ++channel) {
                float actual = pixels[4 * (y * kTargetSize + x) + channel];
                require(actual == expected,
                    [NSString stringWithFormat:
                        @"mode=%s sampling=%s flip=%u pixel=(%u,%u) "
                        "channel=%lu actual=%g expected=%g",
                        coordinateOnly ? "coordinate" : modeName(mode),
                        samplingName(sampling), flip,
                        x, y, (unsigned long)channel, actual, expected]);
            }
        }
    }
    printf("ARGUMENT_QUAD_MARGIN_CASE mode=%s sampling=%s flip=%u "
           "pixels=%lu channels=4 guards=exact\n",
        coordinateOnly ? "coordinate" : modeName(mode),
        samplingName(sampling), flip,
        (unsigned long)(kViewportSize * kViewportSize));
}

static int parseMode(const char *value) {
    if (!strcmp(value, "image") || !strcmp(value, "image-only"))
        return QuadMarginModeImageOnly;
    if (!strcmp(value, "sampler") || !strcmp(value, "sampler-only"))
        return QuadMarginModeSamplerOnly;
    if (!strcmp(value, "both")) return QuadMarginModeBoth;
    return -1;
}

static int parseSampling(const char *value) {
    if (!strcmp(value, "implicit")) return QuadMarginSamplingImplicit;
    if (!strcmp(value, "bias1")) return QuadMarginSamplingBias1;
    if (!strcmp(value, "gradient")) return QuadMarginSamplingGradient;
    return -1;
}

static void usage(void) {
    puts("Usage: test-argument-quad-margin --reference OUTPUT_DIR "
         "[--capture-only] [--mode image|sampler|both]... "
         "[--sampling implicit|bias1|gradient]... [--control coordinate|lod1]\n"
         "       test-argument-quad-margin --bundle BUNDLE OUTPUT_DIR "
         "[--capture-only] [--mode image|sampler|both]... "
         "[--sampling implicit|bias1|gradient]... [--control coordinate|lod1]\n"
         "       test-argument-quad-margin BUNDLE OUTPUT_DIR "
         "[--capture-only] [--mode image|sampler|both]... "
         "[--sampling implicit|bias1|gradient]... [--control coordinate|lod1]\n"
         "Controls run separately; --control rejects --sampling. "
         "Coordinate also rejects --mode and binds only image1 and sampler B.");
}

int main(int argc, const char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        usage();
        return 0;
    }
    if (argc < 3) {
        usage();
        return 2;
    }

    BOOL reference = NO;
    BOOL captureOnly = NO;
    const char *bundlePath = NULL;
    const char *outputPath = NULL;
    int index = 1;
    if (!strcmp(argv[index], "--reference")) {
        reference = YES;
        outputPath = argv[index + 1];
        index += 2;
    } else if (!strcmp(argv[index], "--bundle")) {
        if (argc < 4) {
            usage();
            return 2;
        }
        bundlePath = argv[index + 1];
        outputPath = argv[index + 2];
        index += 3;
    } else {
        bundlePath = argv[index];
        outputPath = argv[index + 1];
        index += 2;
    }

    BOOL selectedModes[kModeCount] = {YES, YES, YES};
    BOOL explicitMode = NO;
    BOOL selectedSamplings[kSamplingCount] = {YES, YES, YES};
    BOOL explicitSampling = NO;
    const char *selectedControl = NULL;
    for (; index < argc; ++index) {
        if (!strcmp(argv[index], "--capture-only") && !captureOnly) {
            captureOnly = YES;
        } else if (!strcmp(argv[index], "--mode") && index + 1 < argc) {
            int mode = parseMode(argv[++index]);
            if (mode < 0) {
                usage();
                return 2;
            }
            if (!explicitMode) {
                memset(selectedModes, 0, sizeof selectedModes);
                explicitMode = YES;
            }
            selectedModes[mode] = YES;
        } else if (!strcmp(argv[index], "--sampling") && index + 1 < argc) {
            int sampling = parseSampling(argv[++index]);
            if (sampling < 0) {
                usage();
                return 2;
            }
            if (!explicitSampling) {
                memset(selectedSamplings, 0, sizeof selectedSamplings);
                explicitSampling = YES;
            }
            selectedSamplings[sampling] = YES;
        } else if (!strcmp(argv[index], "--control") && index + 1 < argc &&
                   selectedControl == NULL) {
            selectedControl = argv[++index];
            if (strcmp(selectedControl, "coordinate") &&
                strcmp(selectedControl, "lod1")) {
                usage();
                return 2;
            }
        } else {
            usage();
            return 2;
        }
    }
    BOOL coordinateOnly = selectedControl &&
        !strcmp(selectedControl, "coordinate");
    if (selectedControl && (explicitSampling ||
        (coordinateOnly && explicitMode))) {
        usage();
        return 2;
    }
    NSUInteger selectedModeCount = 0;
    for (NSUInteger mode = 0; mode < kModeCount; ++mode)
        if (selectedModes[mode]) ++selectedModeCount;
    NSUInteger selectedSamplingCount = 0;
    for (NSUInteger sampling = 0; sampling < kSamplingCount; ++sampling)
        if (selectedSamplings[sampling]) ++selectedSamplingCount;
    if (selectedControl) selectedSamplingCount = 1;
    if (coordinateOnly) selectedModeCount = 1;

    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(120);
    @autoreleasepool {
        NSError *error = nil;
        NSString *directory = @(outputPath);
        require([[NSFileManager defaultManager] createDirectoryAtPath:directory
            withIntermediateDirectories:YES attributes:nil error:&error],
            error.description ?: @"output directory unavailable");

        id<MTLDevice> device = nil;
        if (reference) {
            device = MTLCreateSystemDefaultDevice();
        } else {
            NSBundle *bundle = [NSBundle bundleWithPath:@(bundlePath)];
            require([bundle loadAndReturnError:&error],
                error.description ?: @"bundle load failed");
            device = [[[bundle classNamed:@"GarnetDevice"] alloc] init];
        }
        require(device != nil, @"Metal device unavailable");

        NSString *source = validSource();
        require([source writeToFile:[directory stringByAppendingPathComponent:
            @"argument-quad-margin.metal"] atomically:YES
            encoding:NSUTF8StringEncoding error:&error],
            @"quad margin source write failed");
        MTLCompileOptions *options = [MTLCompileOptions new];
        options.languageVersion = MTLLanguageVersion2_1;
        id<MTLLibrary> library = [device newLibraryWithSource:source
            options:options error:&error];
        require(library != nil,
            error.description ?: @"quad margin source compile failed");

        id<MTLFunction> vertex = [library newFunctionWithName:
            @"quad_margin_vertex"];
        id<MTLFunction> implicit = [library newFunctionWithName:
            @"quad_margin_implicit"];
        id<MTLFunction> bias1 = [library newFunctionWithName:
            @"quad_margin_bias1"];
        id<MTLFunction> gradient = [library newFunctionWithName:
            @"quad_margin_gradient"];
        require(vertex != nil && implicit != nil && bias1 != nil &&
            gradient != nil, @"quad margin functions unavailable");
        for (id<MTLFunction> function in @[vertex, implicit, bias1, gradient])
            writeAIR(function, directory);

        id<MTLFunction> controlFragment = nil;
        if (selectedControl) {
            NSString *controls = controlSource();
            require([controls writeToFile:
                [directory stringByAppendingPathComponent:
                    @"argument-quad-margin-controls.metal"] atomically:YES
                encoding:NSUTF8StringEncoding error:&error],
                @"quad margin control source write failed");
            error = nil;
            id<MTLLibrary> controlLibrary = [device newLibraryWithSource:controls
                options:options error:&error];
            require(controlLibrary != nil,
                error.description ?: @"quad margin control compile failed");
            controlFragment = [controlLibrary newFunctionWithName:
                coordinateOnly ? @"quad_margin_coordinate" : @"quad_margin_lod1"];
            require(controlFragment != nil, @"quad margin control unavailable");
            writeAIR(controlFragment, directory);
        }

        if (captureOnly) {
            printf("ARGUMENT_QUAD_MARGIN_CAPTURE backend=%s functions=%u "
                   "gpu_execution=0 msl=2.1 modes=%lu samplings=%lu control=%s\n",
                reference ? "reference" : "bundle",
                selectedControl ? 5u : 4u,
                (unsigned long)selectedModeCount,
                (unsigned long)selectedSamplingCount,
                selectedControl ?: "none");
            return 0;
        }

        id<MTLArgumentEncoder> encoder =
            [(controlFragment ?: implicit) newArgumentEncoderWithBufferIndex:3];
        require(encoder != nil && encoder.encodedLength > 0 &&
            encoder.alignment > 0 && encoder.alignment <= 4096,
            @"quad margin argument layout unavailable");
        NSUInteger argumentOffset = alignUp(kArgumentOffset, encoder.alignment);
        id<MTLBuffer> arguments = newSharedBuffer(device,
            argumentOffset + encoder.encodedLength + kArgumentTail,
            kArgumentGuard, @"quad margin argument buffer");
        id<MTLTexture> image0 = newMipTexture(device, 10, @"quad margin image0");
        id<MTLTexture> image1 = newMipTexture(device, 20, @"quad margin image1");
        id<MTLSamplerState> samplerA =
            newNearestSampler(device, 0.0f, @"quad margin clamp0 sampler");
        id<MTLSamplerState> samplerB =
            newNearestSampler(device, 3.0f, @"quad margin max3 sampler");

        [encoder setArgumentBuffer:arguments offset:argumentOffset];
        id<MTLTexture> images[2] = {image0, image1};
        id<MTLSamplerState> samplers[2] = {samplerA, samplerB};
        if (coordinateOnly) {
            [encoder setTextures:images + 1 withRange:NSMakeRange(0, 1)];
            [encoder setSamplerStates:samplers + 1 withRange:NSMakeRange(1, 1)];
        } else {
            [encoder setTextures:images withRange:NSMakeRange(0, 2)];
            [encoder setSamplerStates:samplers withRange:NSMakeRange(2, 2)];
        }
        requireGuard(arguments, argumentOffset, encoder.encodedLength,
            kArgumentGuard, @"quad margin arguments after encode");
        NSData *argumentSnapshot = [NSData dataWithBytes:arguments.contents
            length:arguments.length];
        printf("ARGUMENT_QUAD_MARGIN_LAYOUT length=%lu alignment=%lu "
               "argument_offset=%lu control_offset=%lu images=%u samplers=%u "
               "msl=2.1\n",
            (unsigned long)encoder.encodedLength,
            (unsigned long)encoder.alignment,
            (unsigned long)argumentOffset, (unsigned long)kControlOffset,
            coordinateOnly ? 1u : 2u, coordinateOnly ? 1u : 2u);

        id<MTLBuffer> control = newSharedBuffer(device,
            kControlOffset + 2 * sizeof(uint32_t) + kControlTail,
            kControlGuard, @"quad margin control buffer");
        id<MTLRenderPipelineState> implicitPipeline =
            selectedControl ? nil : newPipeline(device, vertex, implicit,
                @"quad margin implicit pipeline unavailable", &error);
        error = nil;
        id<MTLRenderPipelineState> bias1Pipeline =
            selectedControl ? nil : newPipeline(device, vertex, bias1,
                @"quad margin bias1 pipeline unavailable", &error);
        error = nil;
        id<MTLRenderPipelineState> gradientPipeline =
            selectedControl ? nil : newPipeline(device, vertex, gradient,
                @"quad margin gradient pipeline unavailable", &error);
        error = nil;
        id<MTLRenderPipelineState> controlPipeline = selectedControl ?
            newPipeline(device, vertex, controlFragment,
                @"quad margin control pipeline unavailable", &error) : nil;
        id<MTLCommandQueue> queue = [device newCommandQueue];
        require(queue != nil, @"quad margin command queue unavailable");

        NSUInteger cases = 0;
        NSUInteger flipCount = coordinateOnly ? 1 : 2;
        for (NSUInteger mode = 0; mode < kModeCount; ++mode) {
            if (!selectedModes[mode]) continue;
            if (coordinateOnly && mode != 0) continue;
            NSUInteger samplingCount = selectedControl ? 1 : kSamplingCount;
            for (NSUInteger sample = 0; sample < samplingCount; ++sample) {
                if (!selectedControl && !selectedSamplings[sample]) continue;
                QuadMarginSampling sampling = selectedControl ?
                    (coordinateOnly ? QuadMarginSamplingImplicit :
                        QuadMarginSamplingLod1) : (QuadMarginSampling)sample;
                id<MTLRenderPipelineState> pipeline =
                    selectedControl ? controlPipeline :
                    sampling == QuadMarginSamplingImplicit ? implicitPipeline :
                    sampling == QuadMarginSamplingBias1 ? bias1Pipeline :
                    gradientPipeline;
                for (uint32_t flip = 0; flip < flipCount; ++flip) {
                    runCase(device, queue, pipeline, arguments, argumentOffset,
                        encoder.encodedLength, argumentSnapshot, control,
                        kControlOffset, 2 * sizeof(uint32_t), image0, image1,
                        (QuadMarginMode)mode,
                        sampling, flip, coordinateOnly);
                    ++cases;
                }
            }
        }

        printf("ARGUMENT_QUAD_MARGIN_PASS backend=%s msl=2.1 modes=%lu "
               "samplings=%lu flips=%lu cases=%lu "
               "interior_pixels=%lu channels=4 cpu_oracle=1 "
               "implicit_mip=1 bias1_mip=2 gradient_mip=1 clamp0_mip=0 "
               "guards=target,argument,control control=%s lod1_mip=1\n",
            reference ? "reference" : "bundle",
            (unsigned long)selectedModeCount,
            (unsigned long)selectedSamplingCount, (unsigned long)flipCount,
            (unsigned long)cases,
            (unsigned long)(kViewportSize * kViewportSize),
            selectedControl ?: "none");
    }
    return 0;
}
