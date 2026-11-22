#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RDNA4_ARGUMENT_TEST_PREFIX "ARGUMENT_SAMPLING_MATRIX"
#include "test-argument-render-support.h"

typedef struct { float scale, bias; uint32_t image, sampler; } Control;

static NSString *sourceForMode(const char *mode, float biasLimit, BOOL arrayShape) {
    NSString *effectiveBias = isfinite(biasLimit) ? [NSString stringWithFormat:
        @"clamp(control.bias, -%.9ef, %.9ef)", biasLimit, biasLimit] : @"control.bias";
    NSString *option = !strcmp(mode, "bias") ? @"bias(control.bias)" :
        !strcmp(mode, "level") ? @"level(control.bias)" :
        [NSString stringWithFormat:@"gradient2d(float2(control.scale * exp2(%@), 0), float2(0, control.scale * exp2(%@)))",
            effectiveBias, effectiveBias];
    NSString *sample = arrayShape ? [NSString stringWithFormat:
        @"args.images[image].sample(args.filters[filter], position.xy * control.scale, 1u + ((uint(position.x) ^ uint(position.y) ^ control.image) & 1u), %@, int2(1, -1))", option] :
        [NSString stringWithFormat:@"args.images[image].sample(args.filters[filter], position.xy * control.scale, %@)", option];
    return [NSString stringWithFormat:
        @"#include <metal_stdlib>\nusing namespace metal;\n"
         "struct Args { array<%@<float>, 2> images [[id(0)]]; array<sampler, 2> filters [[id(2)]]; };\n"
         "struct Control { float scale; float bias; uint image; uint sampler; };\n"
         "vertex float4 matrix_vertex(uint vid [[vertex_id]]) { float2 p = float2(float((vid << 1u) & 2u), float(vid & 2u)); return float4(p * 2.0f - 1.0f, 0, 1); }\n"
         "fragment float4 matrix_fragment(float4 position [[position]], constant Args &args [[buffer(3)]], constant Control &control [[buffer(1)]]) { "
         "uint image = (uint(position.x) ^ control.image) & 1u; uint filter = (uint(position.y) ^ control.sampler) & 1u; "
         "return %@; }\n", arrayShape ? @"texture2d_array" : @"texture2d", sample];
}

static id<MTLTexture> patternedImage(id<MTLDevice> device, NSUInteger width,
    NSUInteger height, uint32_t base, BOOL arrayShape) {
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
        MTLPixelFormatRGBA32Float width:width height:height mipmapped:YES];
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.usage = MTLTextureUsageShaderRead;
    if (arrayShape) {
        descriptor.textureType = MTLTextureType2DArray;
        descriptor.arrayLength = 3;
    }
    id<MTLTexture> image = [device newTextureWithDescriptor:descriptor];
    require(image && image.width == width && image.height == height, @"matrix image dimensions");
    require(width <= 32 && height <= 32, @"matrix staging extent");
    float texels[32 * 32 * 4];
    for (NSUInteger layer = 0; layer < image.arrayLength; ++layer)
      for (NSUInteger level = 0; level < image.mipmapLevelCount; ++level) {
        NSUInteger w = MAX((NSUInteger)1, width >> level), h = MAX((NSUInteger)1, height >> level);
        for (NSUInteger y = 0; y < h; ++y) for (NSUInteger x = 0; x < w; ++x)
            for (unsigned c = 0; c < 4; ++c) texels[4 * (y * w + x) + c] =
                (float)(base + level + 100 * layer) + (arrayShape ? (float)(x + 32 * y) / 2048.0f : 0);
        [image replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:level slice:layer withBytes:texels
            bytesPerRow:w * 4 * sizeof(float) bytesPerImage:w * h * 4 * sizeof(float)];
    }
    return image;
}

static id<MTLSamplerState> narrowSampler(id<MTLDevice> device) {
    MTLSamplerDescriptor *descriptor = [MTLSamplerDescriptor new];
    descriptor.normalizedCoordinates = YES;
    descriptor.minFilter = MTLSamplerMinMagFilterNearest;
    descriptor.magFilter = MTLSamplerMinMagFilterNearest;
    descriptor.mipFilter = MTLSamplerMipFilterNearest;
    descriptor.sAddressMode = descriptor.tAddressMode = descriptor.rAddressMode = MTLSamplerAddressModeClampToEdge;
    descriptor.lodMinClamp = 1;
    descriptor.lodMaxClamp = 2;
    descriptor.supportArgumentBuffers = YES;
    id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:descriptor];
    require(sampler != nil, @"matrix narrow sampler");
    return sampler;
}

static float expected(const char *mode, Control control, const NSUInteger widths[2],
    const NSUInteger heights[2], uint32_t x, uint32_t y, float biasLimit, BOOL arrayShape) {
    uint32_t image = (x ^ control.image) & 1u, sampler = (y ^ control.sampler) & 1u;
    double lod = !strcmp(mode, "level") ? control.bias :
        log2((double)MAX(widths[image], heights[image]) * control.scale) +
            fmin(fmax(control.bias, -biasLimit), biasLimit);
    lod = fmin(fmax(lod, sampler ? 1.0 : 0.0), sampler ? 2.0 : 4.0);
    NSUInteger last = (NSUInteger)floor(log2((double)MAX(widths[image], heights[image])));
    NSUInteger mip = MIN(last, (NSUInteger)floor(lod + 0.5));
    float value = (float)((image ? 20u : 10u) + mip);
    if (arrayShape) {
        uint32_t layer = 1u + ((x ^ y ^ control.image) & 1u);
        NSUInteger w = MAX((NSUInteger)1, widths[image] >> mip), h = MAX((NSUInteger)1, heights[image] >> mip);
        long px = (long)floorf(((float)x + 0.5f) * control.scale * (float)w) + 1;
        long py = (long)floorf(((float)y + 0.5f) * control.scale * (float)h) - 1;
        px = MIN(MAX(px, 0), (long)w - 1);
        py = MIN(MAX(py, 0), (long)h - 1);
        value += 100 * layer + (float)(px + 32 * py) / 2048.0f;
    }
    return value;
}

static void execute(id<MTLDevice> device, id<MTLCommandQueue> queue,
    id<MTLRenderPipelineState> pipeline, id<MTLBuffer> arguments, NSUInteger argumentOffset,
    NSUInteger argumentLength, NSData *argumentSnapshot, id<MTLBuffer> controls,
    id<MTLTexture> __strong *images, const NSUInteger widths[2], const NSUInteger heights[2],
    const char *shape, const char *mode, Control control, float biasLimit) {
    memcpy((uint8_t *)controls.contents + kControlOffset, &control, sizeof control);
    NSData *snapshot = [NSData dataWithBytes:controls.contents length:controls.length];
    id<MTLTexture> target = newTarget(device);
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(kTargetGuard, kTargetGuard, kTargetGuard, kTargetGuard);
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLRenderCommandEncoder> draw = [command renderCommandEncoderWithDescriptor:pass];
    require(command && draw, @"matrix draw encoder");
    [draw setRenderPipelineState:pipeline];
    [draw setViewport:(MTLViewport){kViewportOrigin, kViewportOrigin, kViewportSize, kViewportSize, 0, 1}];
    [draw setScissorRect:(MTLScissorRect){kViewportOrigin, kViewportOrigin, kViewportSize, kViewportSize}];
    [draw setFragmentBuffer:arguments offset:argumentOffset atIndex:3];
    [draw setFragmentBuffer:controls offset:kControlOffset atIndex:1];
    for (unsigned i = 0; i < 2; ++i) [draw useResource:images[i] usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
    [draw drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [draw endEncoding];
    [command commit];
    [command waitUntilCompleted];
    require(command.status == MTLCommandBufferStatusCompleted && !command.error,
        command.error.description ?: @"matrix completion");
    requireGuard(arguments, argumentOffset, argumentLength, kArgumentGuard, @"matrix argument guards");
    requireGuard(controls, kControlOffset, sizeof control, kControlGuard, @"matrix control guards");
    require(!memcmp(arguments.contents, argumentSnapshot.bytes, arguments.length), @"matrix argument storage unchanged");
    require(!memcmp(controls.contents, snapshot.bytes, controls.length), @"matrix controls unchanged");
    float pixels[kTargetSize * kTargetSize * 4];
    [target getBytes:pixels bytesPerRow:kTargetSize * 4 * sizeof(float)
        fromRegion:MTLRegionMake2D(0, 0, kTargetSize, kTargetSize) mipmapLevel:0];
    for (uint32_t y = 0; y < kTargetSize; ++y) for (uint32_t x = 0; x < kTargetSize; ++x) {
        BOOL inside = x >= kViewportOrigin && x < kViewportOrigin + kViewportSize &&
            y >= kViewportOrigin && y < kViewportOrigin + kViewportSize;
        float wanted = inside ? expected(mode, control, widths, heights, x, y, biasLimit,
            !strncmp(shape, "array-", 6)) : kTargetGuard;
        for (uint32_t channel = 0; channel < 4; ++channel)
            require(pixels[4 * (y * kTargetSize + x) + channel] == wanted,
                [NSString stringWithFormat:@"shape=%s mode=%s scale=%g bias=%g image=%u sampler=%u pixel=%u,%u channel=%u actual=%g expected=%g",
                    shape, mode, control.scale, control.bias, control.image, control.sampler, x, y, channel,
                    pixels[4 * (y * kTargetSize + x) + channel], wanted]);
    }
    printf("ARGUMENT_SAMPLING_MATRIX_DRAW_PASS shape=%s mode=%s scale=%g bias=%g image=%u sampler=%u pixels=64 channels=4 guards=exact gpu_execution=1\n",
        shape, mode, control.scale, control.bias, control.image, control.sampler);
}

int main(int argc, const char **argv) {
    if ((argc != 6 && argc != 7) || (strcmp(argv[3], "pot") && strcmp(argv[3], "npot") &&
        strcmp(argv[3], "array-pot") && strcmp(argv[3], "array-npot")) ||
        (strcmp(argv[4], "bias") && strcmp(argv[4], "grad") && strcmp(argv[4], "level"))) return 2;
    char *end = NULL;
    double bias = strtod(argv[5], &end);
    if (!end || end == argv[5] || *end || !isfinite(bias)) return 2;
    float biasLimit = INFINITY;
    if (argc == 7) {
        double limit = strtod(argv[6], &end);
        if (!end || end == argv[6] || *end || !isfinite(limit) || limit < 0 || limit > 120 ||
            !strcmp(argv[4], "level")) return 2;
        biasLimit = fabsf((float)limit);
        if (fabsf((float)bias) > biasLimit + 4.0f) return 2;
    } else if (bias < -2 || bias > 2) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(110);
    @autoreleasepool {
        NSError *error = nil;
        BOOL reference = !strcmp(argv[1], "--reference");
        id<MTLDevice> device;
        if (reference) device = MTLCreateSystemDefaultDevice();
        else {
            NSBundle *bundle = [NSBundle bundleWithPath:@(argv[1])];
            require([bundle loadAndReturnError:&error], error.description ?: @"matrix bundle load");
            device = [[[bundle classNamed:@"GarnetDevice"] alloc] init];
        }
        require(device != nil, @"matrix device");
        NSString *directory = @(argv[2]);
        require([[NSFileManager defaultManager] createDirectoryAtPath:directory withIntermediateDirectories:YES
            attributes:nil error:&error], @"matrix output directory");
        BOOL arrayShape = !strncmp(argv[3], "array-", 6);
        NSString *source = sourceForMode(argv[4], biasLimit, arrayShape);
        require([source writeToFile:[directory stringByAppendingPathComponent:@"source.metal"] atomically:YES
            encoding:NSUTF8StringEncoding error:&error], @"matrix source capture");
        MTLCompileOptions *options = [MTLCompileOptions new];
        options.languageVersion = MTLLanguageVersion2_1;
        id<MTLLibrary> library = [device newLibraryWithSource:source options:options error:&error];
        require(library != nil, error.description ?: @"matrix source compile");
        id<MTLFunction> vertex = [library newFunctionWithName:@"matrix_vertex"];
        id<MTLFunction> fragment = [library newFunctionWithName:@"matrix_fragment"];
        require(vertex && fragment, @"matrix functions");
        writeAIR(vertex, directory);
        writeAIR(fragment, directory);
        id<MTLRenderPipelineState> pipeline = newPipeline(device, vertex, fragment, @"matrix pipeline", &error);
        id<MTLCommandQueue> queue = [device newCommandQueue];
        require(queue != nil, @"matrix queue");
        id<MTLArgumentEncoder> encoder = [fragment newArgumentEncoderWithBufferIndex:3];
        require(encoder && encoder.encodedLength && encoder.alignment, @"matrix argument encoder");
        NSUInteger offset = alignUp(kArgumentOffset, encoder.alignment);
        id<MTLBuffer> arguments = newSharedBuffer(device, offset + encoder.encodedLength + kArgumentTail,
            kArgumentGuard, @"matrix arguments");
        BOOL npot = !strcmp(argv[3], "npot") || !strcmp(argv[3], "array-npot");
        NSUInteger widths[] = {npot ? 13 : 8, npot ? 23 : 16};
        NSUInteger heights[] = {npot ? 7 : 8, npot ? 11 : 16};
        id<MTLTexture> images[] = {
            (npot || arrayShape) ? patternedImage(device, widths[0], heights[0], 10, arrayShape) :
                newMipTexture(device, 10, @"matrix pot control"),
            patternedImage(device, widths[1], heights[1], 20, arrayShape)};
        id<MTLSamplerState> samplers[] = {newNearestSampler(device, 4, @"matrix wide clamp"), narrowSampler(device)};
        [encoder setArgumentBuffer:arguments offset:offset];
        [encoder setTextures:images withRange:NSMakeRange(0, 2)];
        [encoder setSamplerStates:samplers withRange:NSMakeRange(2, 2)];
        NSData *snapshot = [NSData dataWithBytes:arguments.contents length:arguments.length];
        id<MTLBuffer> controls = newSharedBuffer(device, kControlOffset + sizeof(Control) + kControlTail,
            kControlGuard, @"matrix controls");
        const float scales[] = {0.15f, 0.3f};
        unsigned draws = 0;
        for (unsigned scale = 0; scale < 2; ++scale) for (uint32_t image = 0; image < 2; ++image)
            for (uint32_t sampler = 0; sampler < 2; ++sampler) {
                @autoreleasepool {
                    int shift = isfinite(biasLimit) ? (int)ceilf(biasLimit) * (bias < 0 ? 1 : -1) : 0;
                    Control control = {ldexpf(scales[scale], shift), (float)bias, image, sampler};
                    execute(device, queue, pipeline, arguments, offset, encoder.encodedLength, snapshot, controls,
                        images, widths, heights, argv[3], argv[4], control, biasLimit);
                    ++draws;
                }
            }
        if (arrayShape) printf("ARGUMENT_SAMPLING_ARRAY_PATTERN layers=1:2 offset=1:-1 spatial=1\n");
        printf("ARGUMENT_SAMPLING_MATRIX_PASS backend=%s shape=%s mode=%s bias=%.9g bias_limit=%.9g draws=%u msl=2.1 cpu_oracle=1 sampler_clamps=0:4,1:2 nonzero_offsets=1 guards=target,argument,control gpu_execution=1\n",
            reference ? "reference" : "bundle", argv[3], argv[4], bias, biasLimit, draws);
    }
    return 0;
}
