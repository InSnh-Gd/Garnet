#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const uint8_t kArgumentGuard = 0xa5;
static const uint8_t kOutputGuard = 0xcd;
static const NSUInteger kArgumentOffset = 256;
static const NSUInteger kOutputOffset = 16;
static const NSUInteger kOutputLength = 64;
static const NSUInteger kTailGuard = 256;

static void require(BOOL ok, NSString *message) {
    if (!ok) { fprintf(stderr, "ARGUMENT_INTEGER_GATHER_FAIL %s\n", message.UTF8String); exit(1); }
}

static NSUInteger alignUp(NSUInteger value, NSUInteger alignment) {
    return alignment ? (value + alignment - 1) / alignment * alignment : value;
}

static id<MTLBuffer> newSharedBuffer(id<MTLDevice> device, NSUInteger length, uint8_t fill,
                                     NSString *description) {
    id<MTLBuffer> buffer = [device newBufferWithLength:length options:MTLResourceStorageModeShared];
    require(buffer != nil && buffer.contents != NULL,
        [description stringByAppendingString:@" unavailable"]);
    memset(buffer.contents, fill, length);
    return buffer;
}

static void requireGuard(id<MTLBuffer> buffer, NSUInteger offset, NSUInteger length,
                         uint8_t guard, NSString *description) {
    require(buffer.contents != NULL && offset <= buffer.length && length <= buffer.length - offset,
        [description stringByAppendingString:@" range invalid"]);
    const uint8_t *bytes = buffer.contents;
    for (NSUInteger i = 0; i < offset; ++i)
        if (bytes[i] != guard)
            require(NO, [NSString stringWithFormat:@"%@ prefix guard changed at %lu", description,
                (unsigned long)i]);
    for (NSUInteger i = offset + length; i < buffer.length; ++i)
        if (bytes[i] != guard)
            require(NO, [NSString stringWithFormat:@"%@ suffix guard changed at %lu", description,
                (unsigned long)i]);
}

static id<MTLTexture> newConstantR32Texture(id<MTLDevice> device, MTLPixelFormat format,
                                            uint32_t bits, NSString *description) {
    MTLTextureDescriptor *descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:2 height:2 mipmapped:NO];
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
    require(texture != nil, [description stringByAppendingString:@" unavailable"]);
    const uint32_t texels[4] = {bits, bits, bits, bits};
    [texture replaceRegion:MTLRegionMake2D(0, 0, 2, 2) mipmapLevel:0
        withBytes:texels bytesPerRow:2 * sizeof(uint32_t)];
    return texture;
}

static id<MTLSamplerState> newGatherSampler(id<MTLDevice> device) {
    MTLSamplerDescriptor *descriptor = [MTLSamplerDescriptor new];
    descriptor.minFilter = MTLSamplerMinMagFilterNearest;
    descriptor.magFilter = MTLSamplerMinMagFilterNearest;
    descriptor.mipFilter = MTLSamplerMipFilterNotMipmapped;
    descriptor.sAddressMode = MTLSamplerAddressModeClampToEdge;
    descriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
    descriptor.rAddressMode = MTLSamplerAddressModeClampToEdge;
    descriptor.normalizedCoordinates = YES;
    descriptor.supportArgumentBuffers = YES;
    id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:descriptor];
    require(sampler != nil, @"gather sampler unavailable");
    return sampler;
}

static void writeAIR(id<MTLFunction> function, NSString *directory) {
    SEL selector = NSSelectorFromString(@"bitcodeData");
    NSData *air = [function respondsToSelector:selector] ?
        ((id (*)(id, SEL))objc_msgSend)(function, selector) : nil;
    NSString *path = [directory stringByAppendingPathComponent:
        [function.name stringByAppendingString:@".air"]];
    require(air.length && [air writeToFile:path atomically:YES],
        [NSString stringWithFormat:@"AIR write failed for %@", function.name]);
    printf("ARGUMENT_INTEGER_GATHER_AIR function=%s file=%s bytes=%lu\n",
        function.name.UTF8String, path.UTF8String, (unsigned long)air.length);
}

static void runComputeCase(id<MTLDevice> device, id<MTLCommandQueue> queue,
                           id<MTLComputePipelineState> pipeline, id<MTLBuffer> arguments,
                           NSUInteger argumentOffset, NSUInteger argumentLength,
                           id<MTLBuffer> selection, id<MTLTexture> noiseTexture,
                           id<MTLTexture> selectedTexture, uint32_t expectedWord,
                           const char *caseName, BOOL nullImage) {
    id<MTLBuffer> output = newSharedBuffer(device, kOutputLength, kOutputGuard,
        @"integer gather compute output");
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLComputeCommandEncoder> compute = [command computeCommandEncoder];
    require(command != nil && compute != nil, @"integer gather compute encoder unavailable");
    [compute setComputePipelineState:pipeline];
    [compute setBuffer:arguments offset:argumentOffset atIndex:3];
    [compute setBuffer:selection offset:0 atIndex:1];
    [compute setBuffer:output offset:kOutputOffset atIndex:0];
    [compute useResource:noiseTexture usage:MTLResourceUsageRead];
    if (selectedTexture) [compute useResource:selectedTexture usage:MTLResourceUsageRead];
    [compute dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [compute endEncoding];
    [command commit];
    [command waitUntilCompleted];
    require(command.status == MTLCommandBufferStatusCompleted && !command.error,
        command.error.description ?: [NSString stringWithFormat:@"%s execution failed", caseName]);

    requireGuard(arguments, argumentOffset, argumentLength, kArgumentGuard,
        [NSString stringWithFormat:@"%s arguments", caseName]);
    requireGuard(output, kOutputOffset, 4 * sizeof(uint32_t), kOutputGuard,
        [NSString stringWithFormat:@"%s output", caseName]);
    const uint32_t *words = (const uint32_t *)((const uint8_t *)output.contents + kOutputOffset);
    for (NSUInteger i = 0; i < 4; ++i)
        require(words[i] == expectedWord,
            [NSString stringWithFormat:@"%s component[%lu]=%#x expected=%#x", caseName,
                (unsigned long)i, words[i], expectedWord]);
    printf("ARGUMENT_INTEGER_GATHER_CASE_PASS case=%s null=%d selection=1 gather=component::x "
           "components=%#x,%#x,%#x,%#x guards=1\n",
        caseName, nullImage, words[0], words[1], words[2], words[3]);
}

static void runFragmentCase(id<MTLDevice> device, id<MTLCommandQueue> queue,
                            id<MTLRenderPipelineState> pipeline, id<MTLBuffer> arguments,
                            NSUInteger argumentOffset, NSUInteger argumentLength,
                            id<MTLBuffer> selection, id<MTLTexture> noiseTexture,
                            id<MTLTexture> selectedTexture, float expectedValue,
                            const char *caseName, BOOL nullImage) {
    MTLTextureDescriptor *targetDescriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
            width:4 height:4 mipmapped:NO];
    targetDescriptor.storageMode = MTLStorageModeShared;
    targetDescriptor.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> target = [device newTextureWithDescriptor:targetDescriptor];
    require(target != nil, @"integer gather render target unavailable");

    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(99.0, 99.0, 99.0, 99.0);

    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLRenderCommandEncoder> draw = [command renderCommandEncoderWithDescriptor:pass];
    require(command != nil && draw != nil, @"integer gather render encoder unavailable");
    [draw setRenderPipelineState:pipeline];
    [draw setViewport:(MTLViewport){0, 0, 2, 2, 0, 1}];
    [draw setScissorRect:(MTLScissorRect){0, 0, 2, 2}];
    [draw setFragmentBuffer:arguments offset:argumentOffset atIndex:3];
    [draw setFragmentBuffer:selection offset:0 atIndex:1];
    [draw useResource:noiseTexture usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
    if (selectedTexture)
        [draw useResource:selectedTexture usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
    [draw drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [draw endEncoding];
    [command commit];
    [command waitUntilCompleted];
    require(command.status == MTLCommandBufferStatusCompleted && !command.error,
        command.error.description ?: [NSString stringWithFormat:@"%s execution failed", caseName]);

    requireGuard(arguments, argumentOffset, argumentLength, kArgumentGuard,
        [NSString stringWithFormat:@"%s arguments", caseName]);
    float pixels[4 * 4 * 4];
    [target getBytes:pixels bytesPerRow:4 * 4 * sizeof(float)
        fromRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0];
    for (NSUInteger y = 0; y < 4; ++y) {
        for (NSUInteger x = 0; x < 4; ++x) {
            BOOL covered = x < 2 && y < 2;
            for (NSUInteger channel = 0; channel < 4; ++channel) {
                NSUInteger index = 4 * (y * 4 + x) + channel;
                float expected = covered ? expectedValue : 99.0f;
                require(fabsf(pixels[index] - expected) <= 1.0e-6f,
                    [NSString stringWithFormat:@"%s pixel=(%lu,%lu) channel=%lu actual=%g expected=%g",
                        caseName, (unsigned long)x, (unsigned long)y, (unsigned long)channel,
                        pixels[index], expected]);
            }
        }
    }
    printf("ARGUMENT_INTEGER_GATHER_CASE_PASS case=%s null=%d selection=1 gather=component::x "
           "pixels=4 pixel_guards=12 value=%g guards=1\n",
        caseName, nullImage, expectedValue);
}

int main(int argc, const char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        puts("Usage: test-argument-integer-gather BUNDLE OUTPUT_DIR [--capture-only]");
        puts("       test-argument-integer-gather --reference OUTPUT_DIR [--capture-only]");
        return 0;
    }
    BOOL captureOnly = argc == 4 && !strcmp(argv[3], "--capture-only");
    if ((!captureOnly && argc != 3) || (captureOnly && argc != 4)) return 2;
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

        NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
            "struct UArgs { array<texture2d<uint>, 2> images [[id(0)]]; sampler filter [[id(2)]]; };\n"
            "struct IArgs { array<texture2d<int>, 2> images [[id(0)]]; sampler filter [[id(2)]]; };\n"
            "kernel void gather_uint_compute(constant UArgs &a [[buffer(3)]],"
            " constant uint &selection [[buffer(1)]], device uint *out [[buffer(0)]]) {"
            " uint imageIndex = selection;"
            " uint4 v = a.images[imageIndex].gather(a.filter, float2(0.5f, 0.5f), int2(0), component::x);"
            " out[0] = v.x; out[1] = v.y; out[2] = v.z; out[3] = v.w; }\n"
            "kernel void gather_int_compute(constant IArgs &a [[buffer(3)]],"
            " constant uint &selection [[buffer(1)]], device int *out [[buffer(0)]]) {"
            " uint imageIndex = selection;"
            " int4 v = a.images[imageIndex].gather(a.filter, float2(0.5f, 0.5f), int2(0), component::x);"
            " out[0] = v.x; out[1] = v.y; out[2] = v.z; out[3] = v.w; }\n"
            "vertex float4 gather_vertex(uint vid [[vertex_id]]) {"
            " float2 p = float2(float((vid << 1u) & 2u), float(vid & 2u));"
            " return float4(p * 2.0 - 1.0, 0.0, 1.0); }\n"
            "fragment float4 gather_uint_fragment(float4 p [[position]], constant UArgs &a [[buffer(3)]],"
            " constant uint &selection [[buffer(1)]]) {"
            " uint imageIndex = selection;"
            " uint4 v = a.images[imageIndex].gather(a.filter, p.xy * 0.25f, int2(0), component::x);"
            " return float4(float(v.x), float(v.y), float(v.z), float(v.w)); }\n"
            "fragment float4 gather_int_fragment(float4 p [[position]], constant IArgs &a [[buffer(3)]],"
            " constant uint &selection [[buffer(1)]]) {"
            " uint imageIndex = selection;"
            " int4 v = a.images[imageIndex].gather(a.filter, p.xy * 0.25f, int2(0), component::x);"
            " return float4(float(v.x), float(v.y), float(v.z), float(v.w)); }\n";
        require([source writeToFile:[directory stringByAppendingPathComponent:
            @"argument-integer-gather.metal"] atomically:YES encoding:NSUTF8StringEncoding error:&error],
            @"source write failed");
        MTLCompileOptions *options = [MTLCompileOptions new];
        options.languageVersion = MTLLanguageVersion2_1;
        id<MTLLibrary> library = [device newLibraryWithSource:source options:options error:&error];
        require(library != nil, error.description ?: @"source compilation failed");

        id<MTLFunction> uintComputeFunction = [library newFunctionWithName:@"gather_uint_compute"];
        id<MTLFunction> intComputeFunction = [library newFunctionWithName:@"gather_int_compute"];
        id<MTLFunction> vertexFunction = [library newFunctionWithName:@"gather_vertex"];
        id<MTLFunction> uintFragmentFunction = [library newFunctionWithName:@"gather_uint_fragment"];
        id<MTLFunction> intFragmentFunction = [library newFunctionWithName:@"gather_int_fragment"];
        require(uintComputeFunction && intComputeFunction && vertexFunction &&
            uintFragmentFunction && intFragmentFunction, @"shader functions unavailable");
        for (id<MTLFunction> function in @[uintComputeFunction, intComputeFunction, vertexFunction,
                 uintFragmentFunction, intFragmentFunction])
            writeAIR(function, directory);

        if (captureOnly) {
            puts("ARGUMENT_INTEGER_GATHER_CAPTURE_PASS functions=5 gpu_execution=0 msl=2.1");
            return 0;
        }

        id<MTLArgumentEncoder> uintEncoder = [uintComputeFunction newArgumentEncoderWithBufferIndex:3];
        id<MTLArgumentEncoder> intEncoder = [intComputeFunction newArgumentEncoderWithBufferIndex:3];
        require(uintEncoder && intEncoder && uintEncoder.encodedLength >= 24 &&
            intEncoder.encodedLength >= 24 && uintEncoder.alignment && intEncoder.alignment &&
            uintEncoder.alignment <= 4096 && intEncoder.alignment <= 4096,
            @"integer gather argument layout unavailable");
        NSUInteger uintOffset = alignUp(kArgumentOffset, uintEncoder.alignment);
        NSUInteger intOffset = alignUp(kArgumentOffset, intEncoder.alignment);
        printf("ARGUMENT_INTEGER_GATHER_LAYOUT format=R32Uint length=%lu alignment=%lu offset=%lu\n",
            (unsigned long)uintEncoder.encodedLength, (unsigned long)uintEncoder.alignment,
            (unsigned long)uintOffset);
        printf("ARGUMENT_INTEGER_GATHER_LAYOUT format=R32Sint length=%lu alignment=%lu offset=%lu\n",
            (unsigned long)intEncoder.encodedLength, (unsigned long)intEncoder.alignment,
            (unsigned long)intOffset);

        id<MTLComputePipelineState> uintCompute =
            [device newComputePipelineStateWithFunction:uintComputeFunction error:&error];
        require(uintCompute != nil, error.description ?: @"R32Uint gather compute pipeline failed");
        error = nil;
        id<MTLComputePipelineState> intCompute =
            [device newComputePipelineStateWithFunction:intComputeFunction error:&error];
        require(intCompute != nil, error.description ?: @"R32Sint gather compute pipeline failed");

        MTLRenderPipelineDescriptor *uintRenderDescriptor = [MTLRenderPipelineDescriptor new];
        uintRenderDescriptor.vertexFunction = vertexFunction;
        uintRenderDescriptor.fragmentFunction = uintFragmentFunction;
        uintRenderDescriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
        error = nil;
        id<MTLRenderPipelineState> uintRender =
            [device newRenderPipelineStateWithDescriptor:uintRenderDescriptor error:&error];
        require(uintRender != nil, error.description ?: @"R32Uint gather fragment pipeline failed");
        MTLRenderPipelineDescriptor *intRenderDescriptor = [MTLRenderPipelineDescriptor new];
        intRenderDescriptor.vertexFunction = vertexFunction;
        intRenderDescriptor.fragmentFunction = intFragmentFunction;
        intRenderDescriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
        error = nil;
        id<MTLRenderPipelineState> intRender =
            [device newRenderPipelineStateWithDescriptor:intRenderDescriptor error:&error];
        require(intRender != nil, error.description ?: @"R32Sint gather fragment pipeline failed");

        id<MTLSamplerState> sampler = newGatherSampler(device);
        id<MTLTexture> uintNoise = newConstantR32Texture(device, MTLPixelFormatR32Uint, 23u,
            @"R32Uint noise texture");
        id<MTLTexture> uintSelected = newConstantR32Texture(device, MTLPixelFormatR32Uint, 17u,
            @"R32Uint selected texture");
        id<MTLTexture> intNoise = newConstantR32Texture(device, MTLPixelFormatR32Sint,
            (uint32_t)(int32_t)-23, @"R32Sint noise texture");
        id<MTLTexture> intSelected = newConstantR32Texture(device, MTLPixelFormatR32Sint,
            (uint32_t)(int32_t)-19, @"R32Sint selected texture");
        uint32_t selectionValue = 1u;
        id<MTLBuffer> selection =
            [device newBufferWithBytes:&selectionValue length:sizeof selectionValue
            options:MTLResourceStorageModeShared];
        require(selection != nil, @"selection buffer unavailable");

        id<MTLBuffer> uintArguments = newSharedBuffer(device,
            uintOffset + uintEncoder.encodedLength + kTailGuard, kArgumentGuard,
            @"R32Uint argument buffer");
        id<MTLBuffer> intArguments = newSharedBuffer(device,
            intOffset + intEncoder.encodedLength + kTailGuard, kArgumentGuard,
            @"R32Sint argument buffer");
        [uintEncoder setArgumentBuffer:uintArguments offset:uintOffset];
        [uintEncoder setTexture:uintNoise atIndex:0];
        [uintEncoder setTexture:uintSelected atIndex:1];
        [uintEncoder setSamplerState:sampler atIndex:2];
        [intEncoder setArgumentBuffer:intArguments offset:intOffset];
        [intEncoder setTexture:intNoise atIndex:0];
        [intEncoder setTexture:intSelected atIndex:1];
        [intEncoder setSamplerState:sampler atIndex:2];
        requireGuard(uintArguments, uintOffset, uintEncoder.encodedLength, kArgumentGuard,
            @"R32Uint arguments after encoding");
        requireGuard(intArguments, intOffset, intEncoder.encodedLength, kArgumentGuard,
            @"R32Sint arguments after encoding");

        id<MTLBuffer> uintNullArguments = nil;
        id<MTLBuffer> intNullArguments = nil;
        if (!reference) {
            uintNullArguments = newSharedBuffer(device,
                uintOffset + uintEncoder.encodedLength + kTailGuard, kArgumentGuard,
                @"R32Uint null argument buffer");
            intNullArguments = newSharedBuffer(device,
                intOffset + intEncoder.encodedLength + kTailGuard, kArgumentGuard,
                @"R32Sint null argument buffer");
            [uintEncoder setArgumentBuffer:uintNullArguments offset:uintOffset];
            [uintEncoder setTexture:uintNoise atIndex:0];
            [uintEncoder setTexture:nil atIndex:1];
            [uintEncoder setSamplerState:sampler atIndex:2];
            [intEncoder setArgumentBuffer:intNullArguments offset:intOffset];
            [intEncoder setTexture:intNoise atIndex:0];
            [intEncoder setTexture:nil atIndex:1];
            [intEncoder setSamplerState:sampler atIndex:2];
            requireGuard(uintNullArguments, uintOffset, uintEncoder.encodedLength, kArgumentGuard,
                @"R32Uint null arguments after encoding");
            requireGuard(intNullArguments, intOffset, intEncoder.encodedLength, kArgumentGuard,
                @"R32Sint null arguments after encoding");
        }

        id<MTLCommandQueue> queue = [device newCommandQueue];
        require(queue != nil, @"command queue unavailable");
        runComputeCase(device, queue, uintCompute, uintArguments, uintOffset,
            uintEncoder.encodedLength, selection, uintNoise, uintSelected, 17u,
            "compute-uint", NO);
        runComputeCase(device, queue, intCompute, intArguments, intOffset,
            intEncoder.encodedLength, selection, intNoise, intSelected,
            (uint32_t)(int32_t)-19, "compute-int", NO);
        runFragmentCase(device, queue, uintRender, uintArguments, uintOffset,
            uintEncoder.encodedLength, selection, uintNoise, uintSelected, 17.0f,
            "fragment-uint", NO);
        runFragmentCase(device, queue, intRender, intArguments, intOffset,
            intEncoder.encodedLength, selection, intNoise, intSelected, -19.0f,
            "fragment-int", NO);

        if (!reference) {
            runComputeCase(device, queue, uintCompute, uintNullArguments, uintOffset,
                uintEncoder.encodedLength, selection, uintNoise, nil, 0u,
                "null-compute-uint", YES);
            runComputeCase(device, queue, intCompute, intNullArguments, intOffset,
                intEncoder.encodedLength, selection, intNoise, nil, 0u,
                "null-compute-int", YES);
            runFragmentCase(device, queue, uintRender, uintNullArguments, uintOffset,
                uintEncoder.encodedLength, selection, uintNoise, nil, 0.0f,
                "null-fragment-uint", YES);
            runFragmentCase(device, queue, intRender, intNullArguments, intOffset,
                intEncoder.encodedLength, selection, intNoise, nil, 0.0f,
                "null-fragment-int", YES);
        }

        printf("ARGUMENT_INTEGER_GATHER_PASS formats=R32Uint,R32Sint compute=1 fragment=1 "
               "selection=dynamic-buffer-index1 gather=component::x null_cases=%u "
               "guards=exact msl=2.1\n", reference ? 0u : 4u);
    }
    return 0;
}
