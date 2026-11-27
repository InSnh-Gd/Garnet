#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum {
    kTextureSize = 4,
    kCoordinateCount = 10,
};
static const NSUInteger kOutputOffset = 256;
static const NSUInteger kOutputTail = 256;
static const uint8_t kOutputGuard = 0xcd;
static const uint8_t kUploadGuard = 0x5a;
static const NSUInteger kUploadOffset = 64;
static const float kCompareReference = 0.5f;
static const float kTolerance = 1.0e-6f;

static float gColor[kTextureSize][kTextureSize][4];
static float gDepth[kTextureSize][kTextureSize];

/* 0-2: texel centers, 3-5: fractional, 6-9: clamp edges/outside. */
static const float kPixelCoordinates[kCoordinateCount][2] = {
    {0.5f, 0.5f},
    {1.5f, 2.5f},
    {3.5f, 3.5f},
    {1.0f, 1.0f},
    {1.25f, 2.0f},
    {2.75f, 1.25f},
    {4.0f, 4.0f},
    {-0.5f, -0.5f},
    {-1.0f, -2.0f},
    {5.5f, 6.5f},
};

static void require(BOOL ok, NSString *message) {
    if (!ok) {
        fprintf(stderr, "ARGUMENT_SAMPLER_COORDINATES_FAIL %s\n",
            message.UTF8String);
        exit(1);
    }
}

static void requireClose(float actual, float expected, NSString *message) {
    require(fabsf(actual - expected) <= kTolerance,
        [NSString stringWithFormat:@"%@ actual=%.9g expected=%.9g",
            message, actual, expected]);
}

static void requireGuard(id<MTLBuffer> buffer, NSUInteger offset,
                         NSUInteger length, uint8_t guard,
                         NSString *description) {
    require(buffer.contents != NULL && offset <= buffer.length &&
        length <= buffer.length - offset,
        [description stringByAppendingString:@" range invalid"]);
    const uint8_t *bytes = buffer.contents;
    for (NSUInteger i = 0; i < offset; ++i)
        if (bytes[i] != guard)
            require(NO, [NSString stringWithFormat:
                @"%@ prefix guard changed at %lu", description,
                (unsigned long)i]);
    for (NSUInteger i = offset + length; i < buffer.length; ++i)
        if (bytes[i] != guard)
            require(NO, [NSString stringWithFormat:
                @"%@ suffix guard changed at %lu", description,
                (unsigned long)i]);
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

static id<MTLBuffer> newOutputBuffer(id<MTLDevice> device,
                                     NSUInteger floatCount,
                                     NSString *description) {
    return newSharedBuffer(device,
        kOutputOffset + floatCount * sizeof(float) + kOutputTail,
        kOutputGuard, description);
}

static void fillPattern(void) {
    for (NSUInteger y = 0; y < kTextureSize; ++y) {
        for (NSUInteger x = 0; x < kTextureSize; ++x) {
            gColor[y][x][0] =
                0.125f * (float)((2 * x + 3 * y) % 7);
            gColor[y][x][1] =
                0.125f * (float)((x + 2 * y + 1) % 6);
            gColor[y][x][2] =
                0.25f * (float)((3 * x + y) % 4);
            gColor[y][x][3] = 1.0f;
            /* Keep depth values in the normalized depth range [0, 1]. */
            gDepth[y][x] = 0.125f * (float)((x + 2 * y) % 8);
        }
    }
}

static id<MTLTexture> newColorTexture(id<MTLDevice> device) {
    MTLTextureDescriptor *descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
            MTLPixelFormatRGBA32Float width:kTextureSize height:kTextureSize
            mipmapped:NO];
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
    require(texture != nil, @"sampler coordinates color texture unavailable");
    [texture replaceRegion:MTLRegionMake2D(0, 0, kTextureSize, kTextureSize)
        mipmapLevel:0 withBytes:gColor
        bytesPerRow:kTextureSize * 4 * sizeof(float)];
    return texture;
}

static void commitAndWait(id<MTLCommandBuffer> command, NSString *operation) {
    require(command != nil,
        [operation stringByAppendingString:@" command unavailable"]);
    [command commit];
    [command waitUntilCompleted];
    require(command.status == MTLCommandBufferStatusCompleted &&
        command.error == nil,
        command.error.description ?: [operation stringByAppendingString:@" failed"]);
}

static id<MTLTexture> newDepthTexture(id<MTLDevice> device,
                                      id<MTLCommandQueue> queue,
                                      id<MTLBuffer> *uploadOut) {
    MTLTextureDescriptor *descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
            MTLPixelFormatDepth32Float width:kTextureSize height:kTextureSize
            mipmapped:NO];
    descriptor.storageMode = MTLStorageModePrivate;
    descriptor.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
    require(texture != nil, @"sampler coordinates depth texture unavailable");

    NSUInteger depthBytes = kTextureSize * kTextureSize * sizeof(float);
    id<MTLBuffer> upload = newSharedBuffer(device,
        kUploadOffset + depthBytes + 64, kUploadGuard,
        @"sampler coordinates depth upload");
    memcpy((uint8_t *)upload.contents + kUploadOffset, gDepth, depthBytes);
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
    require(command != nil && blit != nil,
        @"sampler coordinates depth upload encoder unavailable");
    [blit copyFromBuffer:upload sourceOffset:kUploadOffset
        sourceBytesPerRow:kTextureSize * sizeof(float)
        sourceBytesPerImage:depthBytes
        sourceSize:MTLSizeMake(kTextureSize, kTextureSize, 1)
        toTexture:texture destinationSlice:0 destinationLevel:0
        destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding];
    commitAndWait(command, @"sampler coordinates depth upload");
    requireGuard(upload, kUploadOffset, depthBytes, kUploadGuard,
        @"sampler coordinates depth upload after GPU");
    *uploadOut = upload;
    return texture;
}

static id<MTLSamplerState> newSampler(id<MTLDevice> device, BOOL normalized,
                                      MTLCompareFunction compare,
                                      NSString *description) {
    MTLSamplerDescriptor *descriptor = [MTLSamplerDescriptor new];
    descriptor.normalizedCoordinates = normalized;
    descriptor.minFilter = MTLSamplerMinMagFilterLinear;
    descriptor.magFilter = MTLSamplerMinMagFilterLinear;
    descriptor.mipFilter = MTLSamplerMipFilterNotMipmapped;
    descriptor.sAddressMode = MTLSamplerAddressModeClampToEdge;
    descriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
    descriptor.rAddressMode = MTLSamplerAddressModeClampToEdge;
    descriptor.compareFunction = compare;
    descriptor.maxAnisotropy = 1;
    id<MTLSamplerState> sampler =
        [device newSamplerStateWithDescriptor:descriptor];
    require(sampler != nil,
        [description stringByAppendingString:@" unavailable"]);
    return sampler;
}

static id<MTLComputePipelineState> newPipeline(id<MTLDevice> device,
                                               id<MTLFunction> function,
                                               NSString *description,
                                               NSError **error) {
    require(function != nil,
        [description stringByAppendingString:@" function unavailable"]);
    id<MTLComputePipelineState> pipeline =
        [device newComputePipelineStateWithFunction:function error:error];
    NSError *failure = error ? *error : nil;
    require(pipeline != nil,
        failure.description ?: [description stringByAppendingString:@" unavailable"]);
    return pipeline;
}

static void writeAIR(id<MTLFunction> function, NSString *directory) {
    SEL selector = NSSelectorFromString(@"bitcodeData");
    NSData *air = [function respondsToSelector:selector] ?
        ((id (*)(id, SEL))objc_msgSend)(function, selector) : nil;
    NSString *path = [directory stringByAppendingPathComponent:
        [function.name stringByAppendingString:@".air"]];
    require(air.length > 0 && [air writeToFile:path atomically:YES],
        [NSString stringWithFormat:@"AIR write failed for %@", function.name]);
    printf("ARGUMENT_SAMPLER_COORDINATES_AIR function=%s file=%s bytes=%lu\n",
        function.name.UTF8String, path.UTF8String,
        (unsigned long)air.length);
}

static NSString *validSource(void) {
    return @"#include <metal_stdlib>\n"
        "using namespace metal;\n"
        "constant uint kCoordinateCount = 10u;\n"
        "constant float kCompareReference = 0.5f;\n"
        "float2 coordinate_pixel(uint index) {\n"
        "  switch (index) {\n"
        "  case 0u: return float2(0.5f, 0.5f);\n"
        "  case 1u: return float2(1.5f, 2.5f);\n"
        "  case 2u: return float2(3.5f, 3.5f);\n"
        "  case 3u: return float2(1.0f, 1.0f);\n"
        "  case 4u: return float2(1.25f, 2.0f);\n"
        "  case 5u: return float2(2.75f, 1.25f);\n"
        "  case 6u: return float2(4.0f, 4.0f);\n"
        "  case 7u: return float2(-0.5f, -0.5f);\n"
        "  case 8u: return float2(-1.0f, -2.0f);\n"
        "  default: return float2(5.5f, 6.5f);\n"
        "  }\n"
        "}\n"
        "float2 coordinate_normalized(uint index) {\n"
        "  return coordinate_pixel(index) * (1.0f / 4.0f);\n"
        "}\n"
        "kernel void sampler_coordinates_ordinary(\n"
        "    texture2d<float> image [[texture(0)]],\n"
        "    sampler pixelSampler [[sampler(0)]],\n"
        "    sampler normalizedSampler [[sampler(1)]],\n"
        "    device float *out [[buffer(0)]]) {\n"
        "  for (uint i = 0u; i < kCoordinateCount; ++i) {\n"
        "    float4 pixelValue = image.sample(pixelSampler, coordinate_pixel(i));\n"
        "    float4 normalizedValue = image.sample(normalizedSampler,\n"
        "        coordinate_normalized(i));\n"
        "    uint base = i * 8u;\n"
        "    out[base + 0u] = pixelValue.x;\n"
        "    out[base + 1u] = pixelValue.y;\n"
        "    out[base + 2u] = pixelValue.z;\n"
        "    out[base + 3u] = pixelValue.w;\n"
        "    out[base + 4u] = normalizedValue.x;\n"
        "    out[base + 5u] = normalizedValue.y;\n"
        "    out[base + 6u] = normalizedValue.z;\n"
        "    out[base + 7u] = normalizedValue.w;\n"
        "  }\n"
        "}\n"
        "kernel void sampler_coordinates_compare(\n"
        "    depth2d<float> depth [[texture(0)]],\n"
        "    sampler pixelNever [[sampler(0)]],\n"
        "    sampler normalizedNever [[sampler(1)]],\n"
        "    sampler pixelAlways [[sampler(2)]],\n"
        "    sampler normalizedAlways [[sampler(3)]],\n"
        "    sampler pixelLess [[sampler(4)]],\n"
        "    sampler normalizedLess [[sampler(5)]],\n"
        "    device float *out [[buffer(0)]]) {\n"
        "  for (uint i = 0u; i < kCoordinateCount; ++i) {\n"
        "    float2 pixelCoord = coordinate_pixel(i);\n"
        "    float2 normalizedCoord = coordinate_normalized(i);\n"
        "    uint base = i * 6u;\n"
        "    out[base + 0u] = depth.sample_compare(pixelNever, pixelCoord,\n"
        "        kCompareReference, level(0.0f));\n"
        "    out[base + 1u] = depth.sample_compare(normalizedNever,\n"
        "        normalizedCoord, kCompareReference, level(0.0f));\n"
        "    out[base + 2u] = depth.sample_compare(pixelAlways, pixelCoord,\n"
        "        kCompareReference, level(0.0f));\n"
        "    out[base + 3u] = depth.sample_compare(normalizedAlways,\n"
        "        normalizedCoord, kCompareReference, level(0.0f));\n"
        "    out[base + 4u] = depth.sample_compare(pixelLess, pixelCoord,\n"
        "        kCompareReference, level(0.0f));\n"
        "    out[base + 5u] = depth.sample_compare(normalizedLess,\n"
        "        normalizedCoord, kCompareReference, level(0.0f));\n"
        "  }\n"
        "}\n"
        "kernel void sampler_coordinates_gather(\n"
        "    texture2d<float> image [[texture(0)]],\n"
        "    sampler pixelSampler [[sampler(0)]],\n"
        "    sampler normalizedSampler [[sampler(1)]],\n"
        "    device float *out [[buffer(0)]]) {\n"
        "  for (uint i = 0u; i < kCoordinateCount; ++i) {\n"
        "    float4 pixelValue = image.gather(pixelSampler, coordinate_pixel(i));\n"
        "    float4 normalizedValue = image.gather(normalizedSampler,\n"
        "        coordinate_normalized(i));\n"
        "    uint base = i * 8u;\n"
        "    out[base + 0u] = pixelValue.x;\n"
        "    out[base + 1u] = pixelValue.y;\n"
        "    out[base + 2u] = pixelValue.z;\n"
        "    out[base + 3u] = pixelValue.w;\n"
        "    out[base + 4u] = normalizedValue.x;\n"
        "    out[base + 5u] = normalizedValue.y;\n"
        "    out[base + 6u] = normalizedValue.z;\n"
        "    out[base + 7u] = normalizedValue.w;\n"
        "  }\n"
        "}\n"
        "kernel void sampler_coordinates_gather_compare(\n"
        "    depth2d<float> depth [[texture(0)]],\n"
        "    sampler pixelNever [[sampler(0)]],\n"
        "    sampler normalizedNever [[sampler(1)]],\n"
        "    sampler pixelAlways [[sampler(2)]],\n"
        "    sampler normalizedAlways [[sampler(3)]],\n"
        "    sampler pixelLess [[sampler(4)]],\n"
        "    sampler normalizedLess [[sampler(5)]],\n"
        "    device float *out [[buffer(0)]]) {\n"
        "  for (uint i = 0u; i < kCoordinateCount; ++i) {\n"
        "    float2 pixelCoord = coordinate_pixel(i);\n"
        "    float2 normalizedCoord = coordinate_normalized(i);\n"
        "    float4 pixelNeverValue = depth.gather_compare(pixelNever,\n"
        "        pixelCoord, kCompareReference);\n"
        "    float4 normalizedNeverValue = depth.gather_compare(normalizedNever,\n"
        "        normalizedCoord, kCompareReference);\n"
        "    float4 pixelAlwaysValue = depth.gather_compare(pixelAlways,\n"
        "        pixelCoord, kCompareReference);\n"
        "    float4 normalizedAlwaysValue = depth.gather_compare(normalizedAlways,\n"
        "        normalizedCoord, kCompareReference);\n"
        "    float4 pixelLessValue = depth.gather_compare(pixelLess,\n"
        "        pixelCoord, kCompareReference);\n"
        "    float4 normalizedLessValue = depth.gather_compare(normalizedLess,\n"
        "        normalizedCoord, kCompareReference);\n"
        "    uint base = i * 24u;\n"
        "    out[base + 0u] = pixelNeverValue.x;\n"
        "    out[base + 1u] = pixelNeverValue.y;\n"
        "    out[base + 2u] = pixelNeverValue.z;\n"
        "    out[base + 3u] = pixelNeverValue.w;\n"
        "    out[base + 4u] = normalizedNeverValue.x;\n"
        "    out[base + 5u] = normalizedNeverValue.y;\n"
        "    out[base + 6u] = normalizedNeverValue.z;\n"
        "    out[base + 7u] = normalizedNeverValue.w;\n"
        "    out[base + 8u] = pixelAlwaysValue.x;\n"
        "    out[base + 9u] = pixelAlwaysValue.y;\n"
        "    out[base + 10u] = pixelAlwaysValue.z;\n"
        "    out[base + 11u] = pixelAlwaysValue.w;\n"
        "    out[base + 12u] = normalizedAlwaysValue.x;\n"
        "    out[base + 13u] = normalizedAlwaysValue.y;\n"
        "    out[base + 14u] = normalizedAlwaysValue.z;\n"
        "    out[base + 15u] = normalizedAlwaysValue.w;\n"
        "    out[base + 16u] = pixelLessValue.x;\n"
        "    out[base + 17u] = pixelLessValue.y;\n"
        "    out[base + 18u] = pixelLessValue.z;\n"
        "    out[base + 19u] = pixelLessValue.w;\n"
        "    out[base + 20u] = normalizedLessValue.x;\n"
        "    out[base + 21u] = normalizedLessValue.y;\n"
        "    out[base + 22u] = normalizedLessValue.z;\n"
        "    out[base + 23u] = normalizedLessValue.w;\n"
        "  }\n"
        "}\n";
}

static NSString *invalidSource(void) {
    return @"#include <metal_stdlib>\n"
        "using namespace metal;\n"
        "constexpr sampler invalid_pixel_repeat(coord::pixel, address::repeat,\n"
        "    filter::nearest);\n"
        "kernel void invalid_sampler(texture2d<float> image [[texture(0)]],\n"
        "    device float *out [[buffer(0)]]) {\n"
        "  out[0] = image.sample(invalid_pixel_repeat, float2(1.0f)).x;\n"
        "}\n";
}

static int clampIndex(int value, int extent) {
    if (value < 0) return 0;
    if (value >= extent) return extent - 1;
    return value;
}

static float bilinear(float v00, float v10, float v01, float v11,
                      float tx, float ty) {
    float top = v00 * (1.0f - tx) + v10 * tx;
    float bottom = v01 * (1.0f - tx) + v11 * tx;
    return top * (1.0f - ty) + bottom * ty;
}

static void sampleColor(float out[4], const float coordinate[2]) {
    float x = coordinate[0] - 0.5f;
    float y = coordinate[1] - 0.5f;
    int x0 = (int)floorf(x);
    int y0 = (int)floorf(y);
    int x1 = x0 + 1;
    int y1 = y0 + 1;
    float tx = x - (float)x0;
    float ty = y - (float)y0;
    int xa = clampIndex(x0, (int)kTextureSize);
    int xb = clampIndex(x1, (int)kTextureSize);
    int ya = clampIndex(y0, (int)kTextureSize);
    int yb = clampIndex(y1, (int)kTextureSize);
    for (NSUInteger channel = 0; channel < 4; ++channel)
        out[channel] = bilinear(
            gColor[ya][xa][channel], gColor[ya][xb][channel],
            gColor[yb][xa][channel], gColor[yb][xb][channel], tx, ty);
}

static float comparePass(MTLCompareFunction function, float reference,
                         float depth) {
    switch (function) {
    case MTLCompareFunctionNever: return 0.0f;
    /* Metal's Less compares the supplied reference on the left. */
    case MTLCompareFunctionLess: return reference < depth ? 1.0f : 0.0f;
    case MTLCompareFunctionEqual: return reference == depth ? 1.0f : 0.0f;
    case MTLCompareFunctionLessEqual: return reference <= depth ? 1.0f : 0.0f;
    case MTLCompareFunctionGreater: return reference > depth ? 1.0f : 0.0f;
    case MTLCompareFunctionNotEqual: return reference != depth ? 1.0f : 0.0f;
    case MTLCompareFunctionGreaterEqual: return reference >= depth ? 1.0f : 0.0f;
    case MTLCompareFunctionAlways: return 1.0f;
    }
    return -1.0f;
}

static float sampleCompare(const float coordinate[2],
                           MTLCompareFunction function) {
    float x = coordinate[0] - 0.5f;
    float y = coordinate[1] - 0.5f;
    int x0 = (int)floorf(x);
    int y0 = (int)floorf(y);
    int x1 = x0 + 1;
    int y1 = y0 + 1;
    float tx = x - (float)x0;
    float ty = y - (float)y0;
    int xa = clampIndex(x0, (int)kTextureSize);
    int xb = clampIndex(x1, (int)kTextureSize);
    int ya = clampIndex(y0, (int)kTextureSize);
    int yb = clampIndex(y1, (int)kTextureSize);
    return bilinear(
        comparePass(function, kCompareReference, gDepth[ya][xa]),
        comparePass(function, kCompareReference, gDepth[ya][xb]),
        comparePass(function, kCompareReference, gDepth[yb][xa]),
        comparePass(function, kCompareReference, gDepth[yb][xb]), tx, ty);
}

static void gatherOffsets(const float coordinate[2], int x[4], int y[4]) {
    int baseX = (int)floorf(coordinate[0] - 0.5f);
    int baseY = (int)floorf(coordinate[1] - 0.5f);
    /* MSL 2.1 gather order: (-,+), (+,+), (+,-), (-,-). */
    static const int offsetX[4] = {0, 1, 1, 0};
    static const int offsetY[4] = {1, 1, 0, 0};
    for (NSUInteger i = 0; i < 4; ++i) {
        x[i] = clampIndex(baseX + offsetX[i], (int)kTextureSize);
        y[i] = clampIndex(baseY + offsetY[i], (int)kTextureSize);
    }
}

static void gatherColor(float out[4], const float coordinate[2]) {
    int x[4], y[4];
    gatherOffsets(coordinate, x, y);
    for (NSUInteger i = 0; i < 4; ++i)
        out[i] = gColor[y[i]][x[i]][0];
}

static void gatherCompare(float out[4], const float coordinate[2],
                          MTLCompareFunction function) {
    int x[4], y[4];
    gatherOffsets(coordinate, x, y);
    for (NSUInteger i = 0; i < 4; ++i)
        out[i] = comparePass(function, kCompareReference, gDepth[y[i]][x[i]]);
}

static const float *payload(id<MTLBuffer> buffer) {
    return (const float *)((const uint8_t *)buffer.contents + kOutputOffset);
}

static void checkOrdinary(id<MTLBuffer> output) {
    NSUInteger values = kCoordinateCount * 2 * 4;
    requireGuard(output, kOutputOffset, values * sizeof(float),
        kOutputGuard, @"sampler coordinates ordinary output");
    const float *actual = payload(output);
    for (NSUInteger i = 0; i < kCoordinateCount; ++i) {
        float expected[4];
        sampleColor(expected, kPixelCoordinates[i]);
        for (NSUInteger channel = 0; channel < 4; ++channel) {
            float pixel = actual[i * 8 + channel];
            float normalized = actual[i * 8 + 4 + channel];
            NSString *context = [NSString stringWithFormat:
                @"ordinary coord=%lu channel=%lu", (unsigned long)i,
                (unsigned long)channel];
            requireClose(pixel, expected[channel],
                [context stringByAppendingString:@" pixel"]);
            requireClose(normalized, expected[channel],
                [context stringByAppendingString:@" normalized"]);
            requireClose(normalized, pixel,
                [context stringByAppendingString:@" twin mismatch"]);
        }
    }
    printf("ARGUMENT_SAMPLER_COORDINATES_CASE_PASS case=ordinary "
           "coordinates=%lu pairs=%lu components=4 cpu_oracle=1\n",
        (unsigned long)kCoordinateCount, (unsigned long)kCoordinateCount);
}

static void checkCompare(id<MTLBuffer> output) {
    static const MTLCompareFunction functions[3] = {
        MTLCompareFunctionNever,
        MTLCompareFunctionAlways,
        MTLCompareFunctionLess,
    };
    static const char *names[3] = {"Never", "Always", "Less"};
    NSUInteger values = kCoordinateCount * 3 * 2;
    requireGuard(output, kOutputOffset, values * sizeof(float),
        kOutputGuard, @"sampler coordinates compare output");
    const float *actual = payload(output);
    BOOL lessFiltered = NO;
    for (NSUInteger i = 0; i < kCoordinateCount; ++i) {
        for (NSUInteger function = 0; function < 3; ++function) {
            float expected = sampleCompare(kPixelCoordinates[i],
                functions[function]);
            if (functions[function] == MTLCompareFunctionLess &&
                expected > 0.0f && expected < 1.0f)
                lessFiltered = YES;
            float pixel = actual[i * 6 + function * 2];
            float normalized = actual[i * 6 + function * 2 + 1];
            NSString *context = [NSString stringWithFormat:
                @"sample_compare coord=%lu function=%s", (unsigned long)i,
                names[function]];
            requireClose(pixel, expected,
                [context stringByAppendingString:@" pixel"]);
            requireClose(normalized, expected,
                [context stringByAppendingString:@" normalized"]);
            requireClose(normalized, pixel,
                [context stringByAppendingString:@" twin mismatch"]);
        }
    }
    require(lessFiltered,
        @"Less did not exercise per-texel compare followed by filtering");
    printf("ARGUMENT_SAMPLER_COORDINATES_CASE_PASS case=sample_compare "
           "coordinates=%lu functions=Never,Always,Less pairs=6 cpu_oracle=1\n",
        (unsigned long)kCoordinateCount);
}

static void checkGather(id<MTLBuffer> output) {
    NSUInteger values = kCoordinateCount * 2 * 4;
    requireGuard(output, kOutputOffset, values * sizeof(float),
        kOutputGuard, @"sampler coordinates gather output");
    const float *actual = payload(output);
    for (NSUInteger i = 0; i < kCoordinateCount; ++i) {
        float expected[4];
        gatherColor(expected, kPixelCoordinates[i]);
        for (NSUInteger component = 0; component < 4; ++component) {
            float pixel = actual[i * 8 + component];
            float normalized = actual[i * 8 + 4 + component];
            NSString *context = [NSString stringWithFormat:
                @"gather coord=%lu component=%lu", (unsigned long)i,
                (unsigned long)component];
            requireClose(pixel, expected[component],
                [context stringByAppendingString:@" pixel"]);
            requireClose(normalized, expected[component],
                [context stringByAppendingString:@" normalized"]);
            requireClose(normalized, pixel,
                [context stringByAppendingString:@" twin mismatch"]);
        }
    }
    printf("ARGUMENT_SAMPLER_COORDINATES_CASE_PASS case=gather "
           "coordinates=%lu pairs=%lu components=4 cpu_oracle=1\n",
        (unsigned long)kCoordinateCount, (unsigned long)kCoordinateCount);
}

static void checkGatherCompare(id<MTLBuffer> output) {
    static const MTLCompareFunction functions[3] = {
        MTLCompareFunctionNever,
        MTLCompareFunctionAlways,
        MTLCompareFunctionLess,
    };
    static const char *names[3] = {"Never", "Always", "Less"};
    NSUInteger values = kCoordinateCount * 3 * 2 * 4;
    requireGuard(output, kOutputOffset, values * sizeof(float),
        kOutputGuard, @"sampler coordinates gather_compare output");
    const float *actual = payload(output);
    BOOL lessMixed = NO;
    for (NSUInteger i = 0; i < kCoordinateCount; ++i) {
        for (NSUInteger function = 0; function < 3; ++function) {
            float expected[4];
            gatherCompare(expected, kPixelCoordinates[i],
                functions[function]);
            if (functions[function] == MTLCompareFunctionLess) {
                BOOL anyPass = NO;
                BOOL anyFail = NO;
                for (NSUInteger component = 0; component < 4; ++component) {
                    anyPass = anyPass || expected[component] == 1.0f;
                    anyFail = anyFail || expected[component] == 0.0f;
                }
                lessMixed = lessMixed || (anyPass && anyFail);
            }
            for (NSUInteger component = 0; component < 4; ++component) {
                NSUInteger base = i * 24 + function * 8;
                float pixel = actual[base + component];
                float normalized = actual[base + 4 + component];
                NSString *context = [NSString stringWithFormat:
                    @"gather_compare coord=%lu function=%s component=%lu",
                    (unsigned long)i, names[function],
                    (unsigned long)component];
                requireClose(pixel, expected[component],
                    [context stringByAppendingString:@" pixel"]);
                requireClose(normalized, expected[component],
                    [context stringByAppendingString:@" normalized"]);
                requireClose(normalized, pixel,
                    [context stringByAppendingString:@" twin mismatch"]);
            }
        }
    }
    require(lessMixed,
        @"Less gather_compare did not produce a per-texel mixed result");
    printf("ARGUMENT_SAMPLER_COORDINATES_CASE_PASS case=gather_compare "
           "coordinates=%lu functions=Never,Always,Less pairs=6 components=4 "
           "cpu_oracle=1\n", (unsigned long)kCoordinateCount);
}

static void usage(void) {
    puts("Usage: test-sampler-coordinates --reference OUTPUT_DIR [--capture-only]\n"
         "       test-sampler-coordinates --bundle BUNDLE OUTPUT_DIR [--capture-only]\n"
         "       test-sampler-coordinates BUNDLE OUTPUT_DIR [--capture-only]");
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
    fillPattern();

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
            @"sampler-coordinates.metal"] atomically:YES
            encoding:NSUTF8StringEncoding error:&error],
            @"valid sampler coordinates source write failed");
        MTLCompileOptions *options = [MTLCompileOptions new];
        options.languageVersion = MTLLanguageVersion2_1;
        id<MTLLibrary> library = [device newLibraryWithSource:source
            options:options error:&error];
        require(library != nil,
            error.description ?: @"valid sampler coordinates source compile failed");

        id<MTLFunction> ordinaryFunction =
            [library newFunctionWithName:@"sampler_coordinates_ordinary"];
        id<MTLFunction> compareFunction =
            [library newFunctionWithName:@"sampler_coordinates_compare"];
        id<MTLFunction> gatherFunction =
            [library newFunctionWithName:@"sampler_coordinates_gather"];
        id<MTLFunction> gatherCompareFunction =
            [library newFunctionWithName:@"sampler_coordinates_gather_compare"];
        require(ordinaryFunction != nil && compareFunction != nil &&
            gatherFunction != nil && gatherCompareFunction != nil,
            @"sampler coordinates functions unavailable");
        for (id<MTLFunction> function in @[
                 ordinaryFunction, compareFunction,
                 gatherFunction, gatherCompareFunction])
            writeAIR(function, directory);

        NSString *invalid = invalidSource();
        require([invalid writeToFile:[directory stringByAppendingPathComponent:
            @"sampler-coordinates-invalid-pixel-repeat.metal"] atomically:YES
            encoding:NSUTF8StringEncoding error:&error],
            @"invalid sampler coordinates source write failed");
        NSError *rejectError = nil;
        id<MTLLibrary> invalidLibrary = [device newLibraryWithSource:invalid
            options:options error:&rejectError];
        require(invalidLibrary == nil && rejectError != nil,
            @"pixel-coordinate repeat sampler was not rejected");
        printf("ARGUMENT_SAMPLER_COORDINATES_REJECT_PASS "
               "case=pixel-repeat backend=%s\n",
            reference ? "reference" : "bundle");

        if (captureOnly) {
            printf("ARGUMENT_SAMPLER_COORDINATES_CAPTURE backend=%s air=4 "
                   "gpu_execution=0 msl=2.1\n",
                reference ? "reference" : "bundle");
            return 0;
        }

        id<MTLComputePipelineState> ordinaryPipeline =
            newPipeline(device, ordinaryFunction, @"ordinary pipeline", &error);
        id<MTLComputePipelineState> comparePipeline =
            newPipeline(device, compareFunction, @"sample_compare pipeline", &error);
        id<MTLComputePipelineState> gatherPipeline =
            newPipeline(device, gatherFunction, @"gather pipeline", &error);
        id<MTLComputePipelineState> gatherComparePipeline =
            newPipeline(device, gatherCompareFunction,
                @"gather_compare pipeline", &error);
        id<MTLCommandQueue> queue = [device newCommandQueue];
        require(queue != nil, @"sampler coordinates command queue unavailable");

        id<MTLTexture> color = newColorTexture(device);
        id<MTLBuffer> depthUpload = nil;
        id<MTLTexture> depth = newDepthTexture(device, queue, &depthUpload);

        id<MTLSamplerState> pixelNormal =
            newSampler(device, NO, MTLCompareFunctionNever, @"pixel normal sampler");
        id<MTLSamplerState> normalizedNormal =
            newSampler(device, YES, MTLCompareFunctionNever,
                @"normalized normal sampler");
        id<MTLSamplerState> pixelNever =
            newSampler(device, NO, MTLCompareFunctionNever,
                @"pixel Never sampler");
        id<MTLSamplerState> normalizedNever =
            newSampler(device, YES, MTLCompareFunctionNever,
                @"normalized Never sampler");
        id<MTLSamplerState> pixelAlways =
            newSampler(device, NO, MTLCompareFunctionAlways,
                @"pixel Always sampler");
        id<MTLSamplerState> normalizedAlways =
            newSampler(device, YES, MTLCompareFunctionAlways,
                @"normalized Always sampler");
        id<MTLSamplerState> pixelLess =
            newSampler(device, NO, MTLCompareFunctionLess,
                @"pixel Less sampler");
        id<MTLSamplerState> normalizedLess =
            newSampler(device, YES, MTLCompareFunctionLess,
                @"normalized Less sampler");

        id<MTLBuffer> ordinaryOutput =
            newOutputBuffer(device, kCoordinateCount * 2 * 4,
                @"sampler coordinates ordinary output");
        id<MTLBuffer> compareOutput =
            newOutputBuffer(device, kCoordinateCount * 3 * 2,
                @"sampler coordinates compare output");
        id<MTLBuffer> gatherOutput =
            newOutputBuffer(device, kCoordinateCount * 2 * 4,
                @"sampler coordinates gather output");
        id<MTLBuffer> gatherCompareOutput =
            newOutputBuffer(device, kCoordinateCount * 3 * 2 * 4,
                @"sampler coordinates gather_compare output");

        id<MTLCommandBuffer> command = [queue commandBuffer];
        id<MTLComputeCommandEncoder> compute = [command computeCommandEncoder];
        require(command != nil && compute != nil,
            @"sampler coordinates compute encoder unavailable");

        [compute setComputePipelineState:ordinaryPipeline];
        [compute setTexture:color atIndex:0];
        [compute setSamplerState:pixelNormal atIndex:0];
        [compute setSamplerState:normalizedNormal atIndex:1];
        [compute setBuffer:ordinaryOutput offset:kOutputOffset atIndex:0];
        [compute dispatchThreadgroups:MTLSizeMake(1, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];

        [compute setComputePipelineState:comparePipeline];
        [compute setTexture:depth atIndex:0];
        [compute setSamplerState:pixelNever atIndex:0];
        [compute setSamplerState:normalizedNever atIndex:1];
        [compute setSamplerState:pixelAlways atIndex:2];
        [compute setSamplerState:normalizedAlways atIndex:3];
        [compute setSamplerState:pixelLess atIndex:4];
        [compute setSamplerState:normalizedLess atIndex:5];
        [compute setBuffer:compareOutput offset:kOutputOffset atIndex:0];
        [compute dispatchThreadgroups:MTLSizeMake(1, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];

        [compute setComputePipelineState:gatherPipeline];
        [compute setTexture:color atIndex:0];
        [compute setSamplerState:pixelNormal atIndex:0];
        [compute setSamplerState:normalizedNormal atIndex:1];
        [compute setBuffer:gatherOutput offset:kOutputOffset atIndex:0];
        [compute dispatchThreadgroups:MTLSizeMake(1, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];

        [compute setComputePipelineState:gatherComparePipeline];
        [compute setTexture:depth atIndex:0];
        [compute setSamplerState:pixelNever atIndex:0];
        [compute setSamplerState:normalizedNever atIndex:1];
        [compute setSamplerState:pixelAlways atIndex:2];
        [compute setSamplerState:normalizedAlways atIndex:3];
        [compute setSamplerState:pixelLess atIndex:4];
        [compute setSamplerState:normalizedLess atIndex:5];
        [compute setBuffer:gatherCompareOutput offset:kOutputOffset atIndex:0];
        [compute dispatchThreadgroups:MTLSizeMake(1, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];

        [compute endEncoding];
        commitAndWait(command, @"sampler coordinates dispatches");
        require(depthUpload.length > 0,
            @"sampler coordinates depth upload was released early");

        checkOrdinary(ordinaryOutput);
        checkCompare(compareOutput);
        checkGather(gatherOutput);
        checkGatherCompare(gatherCompareOutput);
        printf("ARGUMENT_SAMPLER_COORDINATES_PASS backend=%s msl=2.1 "
               "coordinates=%lu ordinary=1 sample_compare=3 gather=1 "
               "gather_compare=3 cpu_oracle=1 guards=exact "
               "reject=pixel-repeat\n",
            reference ? "reference" : "bundle",
            (unsigned long)kCoordinateCount);
    }
    return 0;
}
