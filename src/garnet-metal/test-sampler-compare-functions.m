#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum {
    kFunctionCount = 8,
    kReferenceCount = 3,
    kCornerCount = 4,
    kColorComponents = 4,
    kFloatsPerFunction = kColorComponents +
        kReferenceCount * kCornerCount,
    kOutputFloatCount = kFunctionCount * kFloatsPerFunction,
};

static const uint8_t kUploadGuard = 0x5a;
static const uint8_t kOutputGuard = 0xcd;
static const NSUInteger kUploadOffset = 64;
static const NSUInteger kOutputOffset = 256;
static const NSUInteger kOutputTail = 256;
static const float kCompareColorPattern[kColorComponents] = {
    0.375f, 0.625f, 0.875f, 1.0f,
};
static const float kReferenceValues[kReferenceCount] = {
    0.25f, 0.50f, 0.75f,
};
/* Row-major 2x2 Depth32Float texture. */
static const float kDepthValues[kCornerCount] = {
    0.25f, 0.50f, 0.75f, 1.00f,
};
static const MTLCompareFunction kFunctions[kFunctionCount] = {
    MTLCompareFunctionNever,
    MTLCompareFunctionLess,
    MTLCompareFunctionEqual,
    MTLCompareFunctionLessEqual,
    MTLCompareFunctionGreater,
    MTLCompareFunctionNotEqual,
    MTLCompareFunctionGreaterEqual,
    MTLCompareFunctionAlways,
};
static const char *const kFunctionNames[kFunctionCount] = {
    "Never", "Less", "Equal", "LessEqual",
    "Greater", "NotEqual", "GreaterEqual", "Always",
};
/*
 * Twelve-bit reference-by-depth comparison signatures. Each function occupies
 * one nibble-triplet; the GPU check rebuilds these bits from its output.
 */
static const uint16_t kExpectedMasks[kFunctionCount] = {
    0x000u, 0x8ceu, 0x421u, 0xcefu,
    0x310u, 0xbdeu, 0x731u, 0xfffu,
};

static void require(BOOL ok, NSString *message) {
    if (!ok) {
        fprintf(stderr, "ARGUMENT_SAMPLER_COMPARE_FAIL %s\n",
            message.UTF8String);
        exit(1);
    }
}

static id<MTLBuffer> newSharedBuffer(id<MTLDevice> device, NSUInteger length,
                                     uint8_t fill, NSString *description) {
    id<MTLBuffer> buffer = [device newBufferWithLength:length
        options:MTLResourceStorageModeShared];
    require(buffer != nil && buffer.contents != NULL,
        [description stringByAppendingString:@" unavailable"]);
    memset(buffer.contents, fill, length);
    return buffer;
}

static void requireGuard(id<MTLBuffer> buffer, NSRange payload, uint8_t guard,
                         NSString *description) {
    require(buffer.contents != NULL && payload.location <= buffer.length &&
        payload.length <= buffer.length - payload.location,
        [description stringByAppendingString:@" range invalid"]);
    const uint8_t *bytes = buffer.contents;
    NSUInteger end = payload.location + payload.length;
    for (NSUInteger i = 0; i < payload.location; ++i)
        if (bytes[i] != guard)
            require(NO, [NSString stringWithFormat:
                @"%@ prefix guard changed at %lu", description,
                (unsigned long)i]);
    for (NSUInteger i = end; i < buffer.length; ++i)
        if (bytes[i] != guard)
            require(NO, [NSString stringWithFormat:
                @"%@ suffix guard changed at %lu", description,
                (unsigned long)i]);
}

static void commitAndWait(id<MTLCommandBuffer> command,
                          NSString *operation) {
    require(command != nil,
        [operation stringByAppendingString:@" command unavailable"]);
    [command commit];
    [command waitUntilCompleted];
    require(command.status == MTLCommandBufferStatusCompleted &&
        command.error == nil,
        command.error.description ?:
            [operation stringByAppendingString:@" failed"]);
}

static void writeAIR(id<MTLFunction> function, NSString *directory) {
    SEL selector = NSSelectorFromString(@"bitcodeData");
    NSData *air = [function respondsToSelector:selector] ?
        ((id (*)(id, SEL))objc_msgSend)(function, selector) : nil;
    NSString *path = [directory stringByAppendingPathComponent:
        [function.name stringByAppendingString:@".air"]];
    require(air.length > 0 && [air writeToFile:path atomically:YES],
        [NSString stringWithFormat:@"AIR write failed for %@",
            function.name]);
    printf("ARGUMENT_SAMPLER_COMPARE_AIR function=%s file=%s bytes=%lu\n",
        function.name.UTF8String, path.UTF8String,
        (unsigned long)air.length);
}

static id<MTLComputePipelineState> newComputePipeline(
    id<MTLDevice> device, id<MTLFunction> function, NSString *description,
    NSError **error) {
    require(function != nil,
        [description stringByAppendingString:@" function unavailable"]);
    id<MTLComputePipelineState> pipeline =
        [device newComputePipelineStateWithFunction:function error:error];
    NSError *failure = error ? *error : nil;
    require(pipeline != nil,
        failure.description ?: [description stringByAppendingString:
            @" pipeline unavailable"]);
    return pipeline;
}

static uint32_t comparePass(MTLCompareFunction function,
                            float reference, float depth) {
    switch (function) {
    case MTLCompareFunctionNever: return 0u;
    case MTLCompareFunctionLess: return reference < depth;
    case MTLCompareFunctionEqual: return reference == depth;
    case MTLCompareFunctionLessEqual: return reference <= depth;
    case MTLCompareFunctionGreater: return reference > depth;
    case MTLCompareFunctionNotEqual: return reference != depth;
    case MTLCompareFunctionGreaterEqual: return reference >= depth;
    case MTLCompareFunctionAlways: return 1u;
    }
    require(NO, @"unknown Metal comparison function");
    return 0u;
}

static uint16_t compareMask(MTLCompareFunction function) {
    uint16_t mask = 0;
    for (NSUInteger reference = 0;
         reference < kReferenceCount; ++reference) {
        for (NSUInteger corner = 0; corner < kCornerCount; ++corner) {
            NSUInteger bit = reference * kCornerCount + corner;
            mask |= (uint16_t)(comparePass(function,
                kReferenceValues[reference], kDepthValues[corner]) << bit);
        }
    }
    return mask;
}

static void verifyCPUOracle(void) {
    for (NSUInteger function = 0; function < kFunctionCount; ++function) {
        uint16_t actual = compareMask(kFunctions[function]);
        require(actual == kExpectedMasks[function],
            [NSString stringWithFormat:
                @"%s oracle mask 0x%03x expected 0x%03x",
                kFunctionNames[function], (unsigned)actual,
                (unsigned)kExpectedMasks[function]]);
        for (NSUInteger prior = 0; prior < function; ++prior)
            require(actual != kExpectedMasks[prior],
                [NSString stringWithFormat:
                    @"%s and %s have the same comparison signature",
                    kFunctionNames[prior], kFunctionNames[function]]);
    }
    printf("ARGUMENT_SAMPLER_COMPARE_ORACLE_OK functions=8 references=%.2f,%.2f,%.2f "
           "depth=%.2f,%.2f,%.2f,%.2f signatures=000,8ce,421,cef,310,bde,731,fff "
           "distinct=8\n",
        kReferenceValues[0], kReferenceValues[1], kReferenceValues[2],
        kDepthValues[0], kDepthValues[1], kDepthValues[2], kDepthValues[3]);
}

static id<MTLSamplerState> newSampler(id<MTLDevice> device,
                                      MTLCompareFunction function,
                                      NSString *description) {
    MTLSamplerDescriptor *descriptor = [MTLSamplerDescriptor new];
    descriptor.normalizedCoordinates = YES;
    descriptor.minFilter = MTLSamplerMinMagFilterNearest;
    descriptor.magFilter = MTLSamplerMinMagFilterNearest;
    descriptor.mipFilter = MTLSamplerMipFilterNotMipmapped;
    descriptor.sAddressMode = MTLSamplerAddressModeClampToEdge;
    descriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
    descriptor.rAddressMode = MTLSamplerAddressModeClampToEdge;
    descriptor.compareFunction = function;
    descriptor.maxAnisotropy = 1;
    id<MTLSamplerState> sampler =
        [device newSamplerStateWithDescriptor:descriptor];
    require(sampler != nil,
        [description stringByAppendingString:@" unavailable"]);
    return sampler;
}

static id<MTLTexture> newColorTexture(id<MTLDevice> device) {
    MTLTextureDescriptor *descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
            MTLPixelFormatRGBA32Float width:1 height:1 mipmapped:NO];
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
    require(texture != nil, @"sampler compare color texture unavailable");
    [texture replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0
        withBytes:kCompareColorPattern bytesPerRow:sizeof kCompareColorPattern];
    return texture;
}

static id<MTLTexture> newDepthTexture(id<MTLDevice> device,
                                      id<MTLCommandQueue> queue,
                                      id<MTLBuffer> *uploadOut) {
    MTLTextureDescriptor *descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
            MTLPixelFormatDepth32Float width:2 height:2 mipmapped:NO];
    descriptor.storageMode = MTLStorageModePrivate;
    descriptor.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
    require(texture != nil, @"sampler compare depth texture unavailable");

    NSUInteger depthBytes = kCornerCount * sizeof(float);
    id<MTLBuffer> upload = newSharedBuffer(device,
        kUploadOffset + depthBytes + 64, kUploadGuard,
        @"sampler compare depth upload");
    memcpy((uint8_t *)upload.contents + kUploadOffset,
        kDepthValues, depthBytes);
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
    require(command != nil && blit != nil,
        @"sampler compare depth upload encoder unavailable");
    [blit copyFromBuffer:upload sourceOffset:kUploadOffset
        sourceBytesPerRow:2 * sizeof(float)
        sourceBytesPerImage:depthBytes
        sourceSize:MTLSizeMake(2, 2, 1)
        toTexture:texture destinationSlice:0 destinationLevel:0
        destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding];
    commitAndWait(command, @"sampler compare depth upload");
    requireGuard(upload, NSMakeRange(kUploadOffset, depthBytes),
        kUploadGuard, @"sampler compare depth upload after GPU");
    *uploadOut = upload;
    return texture;
}

static id<MTLBuffer> newOutputBuffer(id<MTLDevice> device,
                                     NSString *description) {
    return newSharedBuffer(device,
        kOutputOffset + kOutputFloatCount * sizeof(float) + kOutputTail,
        kOutputGuard, description);
}

static void checkOutput(id<MTLBuffer> output, const char *path,
                        BOOL reference) {
    NSRange payload = NSMakeRange(kOutputOffset,
        kOutputFloatCount * sizeof(float));
    requireGuard(output, payload, kOutputGuard,
        [NSString stringWithFormat:@"%s sampler compare output", path]);
    const float *values =
        (const float *)((const uint8_t *)output.contents + kOutputOffset);

    for (NSUInteger function = 0;
         function < kFunctionCount; ++function) {
        NSUInteger base = function * kFloatsPerFunction;
        for (NSUInteger component = 0;
             component < kColorComponents; ++component) {
            float actual = values[base + component];
            require(actual == kCompareColorPattern[component],
                [NSString stringWithFormat:
                    @"%s %s ordinary component=%lu actual=%.9g expected=%.9g",
                    path, kFunctionNames[function],
                    (unsigned long)component, actual,
                    kCompareColorPattern[component]]);
        }

        uint16_t actualMask = 0;
        for (NSUInteger bit = 0;
             bit < kReferenceCount * kCornerCount; ++bit) {
            uint32_t expected =
                (kExpectedMasks[function] >> bit) & 1u;
            float actual = values[base + kColorComponents + bit];
            require(actual == (float)expected,
                [NSString stringWithFormat:
                    @"%s %s reference=%lu depth=%lu actual=%.9g expected=%u",
                    path, kFunctionNames[function],
                    (unsigned long)(bit / kCornerCount),
                    (unsigned long)(bit % kCornerCount),
                    actual, expected]);
            actualMask |= (uint16_t)((actual == 1.0f ? 1u : 0u) << bit);
        }
        require(actualMask == kExpectedMasks[function],
            [NSString stringWithFormat:
                @"%s %s signature=0x%03x expected=0x%03x", path,
                kFunctionNames[function], (unsigned)actualMask,
                (unsigned)kExpectedMasks[function]]);
        printf("ARGUMENT_SAMPLER_COMPARE_CASE_OK backend=%s path=%s "
               "function=%s signature=0x%03x ordinary=0.375,0.625,0.875,1 "
               "mixed=ordinary+sample_compare\n",
            reference ? "reference" : "bundle", path,
            kFunctionNames[function], (unsigned)actualMask);
    }
}

static void runDirectCase(id<MTLDevice> device, id<MTLCommandQueue> queue,
                          id<MTLComputePipelineState> pipeline,
                          id<MTLTexture> color, id<MTLTexture> depth,
                          NSArray<id<MTLSamplerState>> *samplers,
                          BOOL reference) {
    require(samplers.count == kFunctionCount,
        @"direct comparison sampler count");
    id<MTLBuffer> output = newOutputBuffer(device,
        @"direct sampler compare output");
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLComputeCommandEncoder> compute =
        [command computeCommandEncoder];
    require(command != nil && compute != nil,
        @"direct sampler compare encoder unavailable");
    [compute setComputePipelineState:pipeline];
    [compute setTexture:color atIndex:0];
    [compute setTexture:depth atIndex:1];
    for (NSUInteger function = 0;
         function < kFunctionCount; ++function)
        [compute setSamplerState:samplers[function] atIndex:function];
    [compute setBuffer:output offset:kOutputOffset atIndex:0];
    [compute useResource:color usage:MTLResourceUsageRead];
    [compute useResource:depth usage:MTLResourceUsageRead];
    [compute dispatchThreads:MTLSizeMake(1, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [compute endEncoding];
    commitAndWait(command, @"direct sampler comparison functions");
    checkOutput(output, "direct", reference);
}

static void runConstexprCase(id<MTLDevice> device,
                             id<MTLCommandQueue> queue,
                             id<MTLComputePipelineState> pipeline,
                             id<MTLTexture> color,
                             id<MTLTexture> depth, BOOL reference) {
    id<MTLBuffer> output = newOutputBuffer(device,
        @"constexpr sampler compare output");
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLComputeCommandEncoder> compute =
        [command computeCommandEncoder];
    require(command != nil && compute != nil,
        @"constexpr sampler compare encoder unavailable");
    [compute setComputePipelineState:pipeline];
    [compute setTexture:color atIndex:0];
    [compute setTexture:depth atIndex:1];
    [compute setBuffer:output offset:kOutputOffset atIndex:0];
    [compute useResource:color usage:MTLResourceUsageRead];
    [compute useResource:depth usage:MTLResourceUsageRead];
    [compute dispatchThreads:MTLSizeMake(1, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [compute endEncoding];
    commitAndWait(command, @"constexpr sampler comparison functions");
    checkOutput(output, "constexpr", reference);
}

static NSString *validSource(void) {
    return @"#include <metal_stdlib>\n"
        "using namespace metal;\n"
        "constant float kReferences[3] = {0.25f, 0.50f, 0.75f};\n"
        "constant float2 kCorners[4] = {float2(0.25f, 0.25f), "
        "float2(0.75f, 0.25f), float2(0.25f, 0.75f), "
        "float2(0.75f, 0.75f)};\n"
        "constexpr sampler static_never(coord::normalized, "
        "address::clamp_to_edge, filter::nearest, compare_func::never);\n"
        "constexpr sampler static_less(coord::normalized, "
        "address::clamp_to_edge, filter::nearest, compare_func::less);\n"
        "constexpr sampler static_equal(coord::normalized, "
        "address::clamp_to_edge, filter::nearest, compare_func::equal);\n"
        "constexpr sampler static_less_equal(coord::normalized, "
        "address::clamp_to_edge, filter::nearest, "
        "compare_func::less_equal);\n"
        "constexpr sampler static_greater(coord::normalized, "
        "address::clamp_to_edge, filter::nearest, "
        "compare_func::greater);\n"
        "constexpr sampler static_not_equal(coord::normalized, "
        "address::clamp_to_edge, filter::nearest, "
        "compare_func::not_equal);\n"
        "constexpr sampler static_greater_equal(coord::normalized, "
        "address::clamp_to_edge, filter::nearest, "
        "compare_func::greater_equal);\n"
        "constexpr sampler static_always(coord::normalized, "
        "address::clamp_to_edge, filter::nearest, compare_func::always);\n"
        "#define EMIT_MIXED(STATE, BASE) do { \\\n"
        "  float4 ordinary = color.sample(STATE, float2(0.5f)); \\\n"
        "  out[(BASE) + 0u] = ordinary.x; \\\n"
        "  out[(BASE) + 1u] = ordinary.y; \\\n"
        "  out[(BASE) + 2u] = ordinary.z; \\\n"
        "  out[(BASE) + 3u] = ordinary.w; \\\n"
        "  for (uint r = 0u; r < 3u; ++r) { \\\n"
        "    for (uint c = 0u; c < 4u; ++c) { \\\n"
        "      out[(BASE) + 4u + r * 4u + c] = depth.sample_compare( \\\n"
        "          STATE, kCorners[c], kReferences[r], level(0.0f)); \\\n"
        "    } \\\n"
        "  } \\\n"
        "} while (0)\n"
        "kernel void sampler_compare_direct(\n"
        "    texture2d<float> color [[texture(0)]],\n"
        "    depth2d<float> depth [[texture(1)]],\n"
        "    sampler s0 [[sampler(0)]], sampler s1 [[sampler(1)]],\n"
        "    sampler s2 [[sampler(2)]], sampler s3 [[sampler(3)]],\n"
        "    sampler s4 [[sampler(4)]], sampler s5 [[sampler(5)]],\n"
        "    sampler s6 [[sampler(6)]], sampler s7 [[sampler(7)]],\n"
        "    device float *out [[buffer(0)]]) {\n"
        "  EMIT_MIXED(s0, 0u);\n"
        "  EMIT_MIXED(s1, 16u);\n"
        "  EMIT_MIXED(s2, 32u);\n"
        "  EMIT_MIXED(s3, 48u);\n"
        "  EMIT_MIXED(s4, 64u);\n"
        "  EMIT_MIXED(s5, 80u);\n"
        "  EMIT_MIXED(s6, 96u);\n"
        "  EMIT_MIXED(s7, 112u);\n"
        "}\n"
        "kernel void sampler_compare_constexpr(\n"
        "    texture2d<float> color [[texture(0)]],\n"
        "    depth2d<float> depth [[texture(1)]],\n"
        "    device float *out [[buffer(0)]]) {\n"
        "  EMIT_MIXED(static_never, 0u);\n"
        "  EMIT_MIXED(static_less, 16u);\n"
        "  EMIT_MIXED(static_equal, 32u);\n"
        "  EMIT_MIXED(static_less_equal, 48u);\n"
        "  EMIT_MIXED(static_greater, 64u);\n"
        "  EMIT_MIXED(static_not_equal, 80u);\n"
        "  EMIT_MIXED(static_greater_equal, 96u);\n"
        "  EMIT_MIXED(static_always, 112u);\n"
        "}\n"
        "#undef EMIT_MIXED\n";
}

static void usage(void) {
    puts("Usage: test-sampler-compare-functions --reference OUTPUT_DIR "
         "[--capture-only]\n"
         "       test-sampler-compare-functions --bundle BUNDLE OUTPUT_DIR "
         "[--capture-only]\n"
         "       test-sampler-compare-functions BUNDLE OUTPUT_DIR "
         "[--capture-only]");
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
    for (; index < argc; ++index) {
        if (!strcmp(argv[index], "--capture-only") && !captureOnly)
            captureOnly = YES;
        else {
            usage();
            return 2;
        }
    }

    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(120);

    @autoreleasepool {
        verifyCPUOracle();
        NSError *error = nil;
        NSString *directory = [NSString stringWithUTF8String:outputPath];
        require([[NSFileManager defaultManager]
            createDirectoryAtPath:directory
            withIntermediateDirectories:YES attributes:nil error:&error],
            error.description ?: @"output directory unavailable");

        id<MTLDevice> device = nil;
        if (reference) {
            device = MTLCreateSystemDefaultDevice();
        } else {
            NSBundle *bundle = [NSBundle bundleWithPath:
                [NSString stringWithUTF8String:bundlePath]];
            require([bundle loadAndReturnError:&error],
                error.description ?: @"bundle load failed");
            device = [[[bundle classNamed:@"GarnetDevice"] alloc] init];
        }
        require(device != nil, @"Metal device unavailable");

        NSString *source = validSource();
        require([source writeToFile:[directory stringByAppendingPathComponent:
            @"sampler-compare-functions.metal"] atomically:YES
            encoding:NSUTF8StringEncoding error:&error],
            @"sampler comparison source write failed");
        MTLCompileOptions *options = [MTLCompileOptions new];
        options.languageVersion = MTLLanguageVersion2_1;
        id<MTLLibrary> library = [device newLibraryWithSource:source
            options:options error:&error];
        require(library != nil,
            error.description ?: @"sampler comparison source compilation failed");

        id<MTLFunction> directFunction =
            [library newFunctionWithName:@"sampler_compare_direct"];
        id<MTLFunction> constexprFunction =
            [library newFunctionWithName:@"sampler_compare_constexpr"];
        require(directFunction != nil && constexprFunction != nil,
            @"sampler comparison function unavailable");
        writeAIR(directFunction, directory);
        writeAIR(constexprFunction, directory);

        if (captureOnly) {
            printf("ARGUMENT_SAMPLER_COMPARE_CAPTURE backend=%s air=2 "
                   "gpu_execution=0 msl=2.1\n",
                reference ? "reference" : "bundle");
            return 0;
        }

        id<MTLComputePipelineState> directPipeline =
            newComputePipeline(device, directFunction,
                @"direct sampler comparison", &error);
        error = nil;
        id<MTLComputePipelineState> constexprPipeline =
            newComputePipeline(device, constexprFunction,
                @"constexpr sampler comparison", &error);
        id<MTLCommandQueue> queue = [device newCommandQueue];
        require(queue != nil,
            @"sampler comparison command queue unavailable");

        NSMutableArray<id<MTLSamplerState>> *samplers =
            [NSMutableArray arrayWithCapacity:kFunctionCount];
        for (NSUInteger function = 0;
             function < kFunctionCount; ++function) {
            [samplers addObject:newSampler(device, kFunctions[function],
                [NSString stringWithFormat:@"%s comparison sampler",
                    kFunctionNames[function]])];
        }

        id<MTLTexture> color = newColorTexture(device);
        id<MTLBuffer> depthUpload = nil;
        id<MTLTexture> depth = newDepthTexture(device, queue, &depthUpload);
        runDirectCase(device, queue, directPipeline, color, depth,
            samplers, reference);
        runConstexprCase(device, queue, constexprPipeline, color, depth,
            reference);
        require(depthUpload.length > 0,
            @"sampler comparison depth upload was released early");

        printf("ARGUMENT_SAMPLER_COMPARE_PASS backend=%s msl=2.1 "
               "functions=8 references=3 depth_texels=4 "
               "direct=8-runtime-samplers constexpr=8-static-samplers "
               "mixed=ordinary+sample_compare offset=256 guards=exact "
               "signatures_unique=1\n",
            reference ? "reference" : "bundle");
    }
    return 0;
}
