#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const uint8_t kArgumentGuard = 0xa5;
static const NSUInteger kArgumentOffset = 256;
static const NSUInteger kArgumentTail = 256;
static const NSUInteger kTargetSize = 6;
static const NSUInteger kViewportOrigin = 1;
static const NSUInteger kViewportSize = 4;
static const float kTargetGuard = 99.0f;

typedef enum {
    QuadModeDivergent = 0,
    QuadModeUniform = 1,
    QuadModeGradient = 2,
} QuadMode;

static const char *quadModeName(QuadMode mode) {
    if (mode == QuadModeUniform) return "uniform";
    if (mode == QuadModeGradient) return "gradient";
    return "divergent";
}

static void require(BOOL ok, NSString *message) {
    if (!ok) {
        fprintf(stderr, "ARGUMENT_QUAD_LOD_FAIL %s\n", message.UTF8String);
        exit(1);
    }
}

static NSUInteger alignUp(NSUInteger value, NSUInteger alignment) {
    require(alignment != 0, @"argument encoder alignment is zero");
    return (value + alignment - 1) / alignment * alignment;
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

static void requireGuard(id<MTLBuffer> buffer, NSUInteger offset, NSUInteger length,
                         uint8_t guard, NSString *description) {
    require(buffer.contents != NULL && offset <= buffer.length &&
        length <= buffer.length - offset,
        [description stringByAppendingString:@" range invalid"]);
    const uint8_t *bytes = buffer.contents;
    for (NSUInteger i = 0; i < offset; ++i)
        if (bytes[i] != guard)
            require(NO, [NSString stringWithFormat:@"%@ prefix changed at %lu",
                description, (unsigned long)i]);
    for (NSUInteger i = offset + length; i < buffer.length; ++i)
        if (bytes[i] != guard)
            require(NO, [NSString stringWithFormat:@"%@ suffix changed at %lu",
                description, (unsigned long)i]);
}

static id<MTLTexture> newMipTexture(id<MTLDevice> device, uint32_t base,
                                    NSString *description) {
    MTLTextureDescriptor *descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
            width:8 height:8 mipmapped:YES];
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
    require(texture != nil && texture.mipmapLevelCount == 4,
        [description stringByAppendingString:@" unavailable"]);
    float texels[8 * 8 * 4];
    for (NSUInteger level = 0; level < 4; ++level) {
        NSUInteger size = 8 >> level;
        float value = (float)(base + (uint32_t)level);
        for (NSUInteger i = 0; i < size * size * 4; ++i) texels[i] = value;
        [texture replaceRegion:MTLRegionMake2D(0, 0, size, size)
            mipmapLevel:level withBytes:texels
            bytesPerRow:size * 4 * sizeof(float)];
    }
    return texture;
}

static id<MTLSamplerState> newNearestSampler(id<MTLDevice> device,
                                              BOOL clampLod, NSString *description) {
    MTLSamplerDescriptor *descriptor = [MTLSamplerDescriptor new];
    descriptor.minFilter = MTLSamplerMinMagFilterNearest;
    descriptor.magFilter = MTLSamplerMinMagFilterNearest;
    descriptor.mipFilter = MTLSamplerMipFilterNearest;
    descriptor.normalizedCoordinates = YES;
    descriptor.sAddressMode = MTLSamplerAddressModeClampToEdge;
    descriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
    descriptor.rAddressMode = MTLSamplerAddressModeClampToEdge;
    descriptor.supportArgumentBuffers = YES;
    if (clampLod) {
        descriptor.lodMinClamp = 0.0f;
        descriptor.lodMaxClamp = 1.0f;
    }
    id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:descriptor];
    require(sampler != nil, [description stringByAppendingString:@" unavailable"]);
    return sampler;
}

static void writeAIR(id<MTLFunction> function, NSString *directory) {
    SEL selector = NSSelectorFromString(@"bitcodeData");
    NSData *air = [function respondsToSelector:selector] ?
        ((id (*)(id, SEL))objc_msgSend)(function, selector) : nil;
    NSString *path = [directory stringByAppendingPathComponent:
        [function.name stringByAppendingString:@".air"]];
    require(air.length > 0 && [air writeToFile:path atomically:YES],
        [NSString stringWithFormat:@"AIR write failed for %@", function.name]);
    printf("ARGUMENT_QUAD_LOD_AIR function=%s file=%s bytes=%lu\n",
        function.name.UTF8String, path.UTF8String, (unsigned long)air.length);
}

static id<MTLRenderPipelineState> newQuadPipeline(id<MTLDevice> device,
                                                   id<MTLFunction> vertex,
                                                   id<MTLFunction> fragment,
                                                   NSString *description,
                                                   NSError **error) {
    MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = vertex;
    descriptor.fragmentFunction = fragment;
    descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
    id<MTLRenderPipelineState> pipeline =
        [device newRenderPipelineStateWithDescriptor:descriptor error:error];
    NSError *failure = error ? *error : nil;
    require(pipeline != nil, failure.description ?: description);
    return pipeline;
}

static uint32_t expectedLevel(uint32_t samplerIndex, BOOL bias) {
    if (samplerIndex == 0) return 1;
    return bias ? 3 : 2;
}

static void runCase(id<MTLDevice> device, id<MTLCommandQueue> queue,
                    id<MTLRenderPipelineState> pipeline,
                    id<MTLBuffer> arguments, NSUInteger argumentOffset,
                    NSUInteger argumentLength, id<MTLBuffer> flipBuffer,
                    id<MTLTexture> image0, id<MTLTexture> image1,
                    NSData *argumentSnapshot, uint32_t flip,
                    BOOL bias, QuadMode mode, const char *caseName) {
    MTLTextureDescriptor *targetDescriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
            width:kTargetSize height:kTargetSize mipmapped:NO];
    targetDescriptor.storageMode = MTLStorageModeShared;
    targetDescriptor.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> target = [device newTextureWithDescriptor:targetDescriptor];
    require(target != nil, @"quad LOD target unavailable");

    require(flipBuffer.contents != NULL, @"flip buffer unavailable");
    *(uint32_t *)flipBuffer.contents = flip;
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor =
        MTLClearColorMake(kTargetGuard, kTargetGuard, kTargetGuard, kTargetGuard);

    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLRenderCommandEncoder> draw =
        [command renderCommandEncoderWithDescriptor:pass];
    require(command != nil && draw != nil, @"quad LOD render encoder unavailable");
    [draw setRenderPipelineState:pipeline];
    [draw setViewport:(MTLViewport){
        (double)kViewportOrigin, (double)kViewportOrigin,
        (double)kViewportSize, (double)kViewportSize, 0.0, 1.0 }];
    [draw setScissorRect:(MTLScissorRect){
        kViewportOrigin, kViewportOrigin, kViewportSize, kViewportSize }];
    [draw setFragmentBuffer:arguments offset:argumentOffset atIndex:3];
    [draw setFragmentBuffer:flipBuffer offset:0 atIndex:1];
    [draw useResource:image0 usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
    [draw useResource:image1 usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
    [draw drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [draw endEncoding];
    [command commit];
    [command waitUntilCompleted];
    require(command.status == MTLCommandBufferStatusCompleted && command.error == nil,
        command.error.description ?: [NSString stringWithFormat:@"%s execution failed", caseName]);

    requireGuard(arguments, argumentOffset, argumentLength, kArgumentGuard,
        [NSString stringWithFormat:@"%s arguments", caseName]);
    require(memcmp(argumentSnapshot.bytes, arguments.contents, arguments.length) == 0,
        [NSString stringWithFormat:@"%s changed argument storage", caseName]);

    float pixels[6 * 6 * 4];
    [target getBytes:pixels bytesPerRow:kTargetSize * 4 * sizeof(float)
        fromRegion:MTLRegionMake2D(0, 0, kTargetSize, kTargetSize) mipmapLevel:0];
    for (NSUInteger y = 0; y < kTargetSize; ++y) {
        for (NSUInteger x = 0; x < kTargetSize; ++x) {
            BOOL interior = x >= kViewportOrigin &&
                x < kViewportOrigin + kViewportSize &&
                y >= kViewportOrigin &&
                y < kViewportOrigin + kViewportSize;
            float expected = kTargetGuard;
            if (interior) {
                uint32_t imageIndex = mode == QuadModeUniform ?
                    flip : (((uint32_t)x & 1u) ^ flip);
                uint32_t samplerIndex = mode == QuadModeUniform ?
                    flip : ((uint32_t)y & 1u);
                uint32_t base = imageIndex ? 20u : 10u;
                expected = (float)(base + expectedLevel(samplerIndex, bias));
            }
            for (NSUInteger channel = 0; channel < 4; ++channel) {
                float actual = pixels[4 * (y * kTargetSize + x) + channel];
                require(actual == expected,
                    [NSString stringWithFormat:@"%s pixel=(%lu,%lu) channel=%lu "
                        "actual=%g expected=%g", caseName,
                        (unsigned long)x, (unsigned long)y, (unsigned long)channel,
                        actual, expected]);
            }
        }
    }
    printf("ARGUMENT_QUAD_LOD_CASE case=%s mode=%s flip=%u interior=%lux%lu "
           "channels=4 guards=exact\n", caseName, quadModeName(mode), flip,
        (unsigned long)kViewportSize, (unsigned long)kViewportSize);
}

int main(int argc, const char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        puts("Usage: test-argument-quad-lod BUNDLE|--reference OUTPUT_DIR "
             "[--capture-only] [--mode divergent|uniform|gradient]");
        return 0;
    }
    BOOL captureOnly = NO;
    QuadMode mode = QuadModeDivergent;
    for (int i = 3; i < argc; ++i) {
        if (!strcmp(argv[i], "--capture-only") && !captureOnly) {
            captureOnly = YES;
        } else if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
            const char *value = argv[++i];
            if (!strcmp(value, "divergent")) mode = QuadModeDivergent;
            else if (!strcmp(value, "uniform")) mode = QuadModeUniform;
            else if (!strcmp(value, "gradient")) mode = QuadModeGradient;
            else return 2;
        } else {
            return 2;
        }
    }
    if (argc < 3) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(110);
    @autoreleasepool {
        NSError *error = nil;
        NSString *directory = @(argv[2]);
        require([[NSFileManager defaultManager] createDirectoryAtPath:directory
            withIntermediateDirectories:YES attributes:nil error:&error],
            error.description ?: @"output directory unavailable");

        BOOL reference = !strcmp(argv[1], "--reference");
        id<MTLDevice> device = nil;
        if (reference) device = MTLCreateSystemDefaultDevice();
        else {
            NSBundle *bundle = [NSBundle bundleWithPath:@(argv[1])];
            require([bundle loadAndReturnError:&error], error.description ?: @"bundle load failed");
            device = [[[bundle classNamed:@"GarnetDevice"] alloc] init];
        }
        require(device != nil, @"Metal device unavailable");

        NSString *selection = mode == QuadModeUniform ?
            @" uint imageIndex = flip & 1u;"
             " uint samplerIndex = flip & 1u;" :
            @" uint imageIndex = (uint(position.x) & 1u) ^ (flip & 1u);"
             " uint samplerIndex = uint(position.y) & 1u;";
        NSString *implicitSample =
            @" return args.images[imageIndex].sample(args.filters[samplerIndex],"
             " position.xy * 0.5f); }\n";
        NSString *implicitBias =
            @" return args.images[imageIndex].sample(args.filters[samplerIndex],"
             " position.xy * 0.5f, bias(1.0f)); }\n";
        NSString *gradientSample =
            @" return args.images[imageIndex].sample(args.filters[samplerIndex],"
             " position.xy * 0.5f, gradient2d(float2(0.5f, 0.0f), float2(0.0f, 0.5f))); }\n";
        NSString *gradientBias =
            @" return args.images[imageIndex].sample(args.filters[samplerIndex],"
             " position.xy * 0.5f, gradient2d(float2(1.0f, 0.0f), float2(0.0f, 1.0f))); }\n";
        NSString *source = [NSString stringWithFormat:
            @"#include <metal_stdlib>\nusing namespace metal;\n"
            "struct Args { array<texture2d<float>, 2> images [[id(0)]];"
            " array<sampler, 2> filters [[id(2)]]; };\n"
            "vertex float4 quad_lod_vertex(uint vid [[vertex_id]]) {"
            " float2 p = float2(float((vid << 1u) & 2u), float(vid & 2u));"
            " return float4(p * 2.0f - 1.0f, 0.0f, 1.0f); }\n"
            "fragment float4 quad_lod_no_bias(float4 position [[position]],"
            " constant Args &args [[buffer(3)]], constant uint &flip [[buffer(1)]]) {"
            "%@%@"
            "fragment float4 quad_lod_bias1(float4 position [[position]],"
            " constant Args &args [[buffer(3)]], constant uint &flip [[buffer(1)]]) {"
            "%@%@",
            selection,
            mode == QuadModeGradient ? gradientSample : implicitSample,
            selection,
            mode == QuadModeGradient ? gradientBias : implicitBias];
        require([source writeToFile:[directory stringByAppendingPathComponent:
            @"argument-quad-lod.metal"] atomically:YES encoding:NSUTF8StringEncoding
            error:&error], @"source write failed");
        MTLCompileOptions *options = [MTLCompileOptions new];
        options.languageVersion = MTLLanguageVersion2_1;
        id<MTLLibrary> library = [device newLibraryWithSource:source options:options
            error:&error];
        require(library != nil, error.description ?: @"source compilation failed");

        id<MTLFunction> vertex = [library newFunctionWithName:@"quad_lod_vertex"];
        id<MTLFunction> noBias = [library newFunctionWithName:@"quad_lod_no_bias"];
        id<MTLFunction> bias1 = [library newFunctionWithName:@"quad_lod_bias1"];
        require(vertex && noBias && bias1, @"shader functions unavailable");
        for (id<MTLFunction> function in @[vertex, noBias, bias1])
            writeAIR(function, directory);

        if (captureOnly) {
            printf("ARGUMENT_QUAD_LOD_CAPTURE functions=3 gpu_execution=0 msl=2.1 mode=%s\n",
                quadModeName(mode));
            return 0;
        }

        id<MTLArgumentEncoder> encoder =
            [noBias newArgumentEncoderWithBufferIndex:3];
        require(encoder != nil && encoder.encodedLength > 0 &&
            encoder.alignment > 0 && encoder.alignment <= 4096,
            @"argument layout unavailable");
        NSUInteger argumentOffset = alignUp(kArgumentOffset, encoder.alignment);
        id<MTLBuffer> arguments = newSharedBuffer(device,
            argumentOffset + encoder.encodedLength + kArgumentTail,
            kArgumentGuard, @"quad LOD argument buffer");
        id<MTLTexture> image0 = newMipTexture(device, 10, @"image0");
        id<MTLTexture> image1 = newMipTexture(device, 20, @"image1");
        id<MTLSamplerState> sampler0 =
            newNearestSampler(device, YES, @"lodMaxClamp sampler");
        id<MTLSamplerState> sampler1 =
            newNearestSampler(device, NO, @"default lod sampler");

        [encoder setArgumentBuffer:arguments offset:argumentOffset];
        id<MTLTexture> images[2] = { image0, image1 };
        id<MTLSamplerState> samplers[2] = { sampler0, sampler1 };
        [encoder setTextures:images withRange:NSMakeRange(0, 2)];
        [encoder setSamplerStates:samplers withRange:NSMakeRange(2, 2)];
        requireGuard(arguments, argumentOffset, encoder.encodedLength, kArgumentGuard,
            @"quad LOD arguments after encode");
        NSData *argumentSnapshot = [NSData dataWithBytes:arguments.contents
            length:arguments.length];
        printf("ARGUMENT_QUAD_LOD_LAYOUT length=%lu alignment=%lu offset=%lu images=2 samplers=2 msl=2.1\n",
            (unsigned long)encoder.encodedLength, (unsigned long)encoder.alignment,
            (unsigned long)argumentOffset);

        id<MTLRenderPipelineState> noBiasPipeline =
            newQuadPipeline(device, vertex, noBias, @"no-bias pipeline unavailable", &error);
        error = nil;
        id<MTLRenderPipelineState> bias1Pipeline =
            newQuadPipeline(device, vertex, bias1, @"bias1 pipeline unavailable", &error);
        id<MTLCommandQueue> queue = [device newCommandQueue];
        require(queue != nil, @"command queue unavailable");
        uint32_t initialFlip = 0;
        id<MTLBuffer> flipBuffer = [device newBufferWithBytes:&initialFlip
            length:sizeof initialFlip options:MTLResourceStorageModeShared];
        require(flipBuffer != nil && flipBuffer.contents != NULL,
            @"flip buffer unavailable");

        for (uint32_t flip = 0; flip < 2; ++flip) {
            runCase(device, queue, noBiasPipeline, arguments, argumentOffset,
                encoder.encodedLength, flipBuffer, image0, image1,
                argumentSnapshot, flip, NO, mode, "no-bias");
            runCase(device, queue, bias1Pipeline, arguments, argumentOffset,
                encoder.encodedLength, flipBuffer, image0, image1,
                argumentSnapshot, flip, YES, mode, "bias1");
        }

        printf("ARGUMENT_QUAD_LOD_PASS fragments=2 air_functions=3 cases=4 "
               "interior_pixels=16 channels=4 msl=2.1 mode=%s\n", quadModeName(mode));
    }
    return 0;
}
