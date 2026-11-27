#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const uint8_t kArgumentGuard = 0xa5;
static const uint8_t kUploadGuard = 0x5a;
static const uint8_t kOutputGuard = 0xcd;
static const NSUInteger kArgumentOffset = 256;
static const NSUInteger kArgumentTail = 256;
static const NSUInteger kUploadOffset = 64;
static const NSUInteger kOutputOffset = 64;
static const NSUInteger kOutputLength = 128;
static const NSUInteger kComputeThreads = 4;
static const NSUInteger kDepthSize = 2;
static const NSUInteger kDepthBytes = 2 * 2 * sizeof(float);
static const NSUInteger kRenderSize = 4;
static const NSUInteger kRenderOrigin = 1;
static const NSUInteger kRenderSpan = 2;
static const float kRenderGuard = 0.5f;

static void require(BOOL ok, NSString *message) {
    if (!ok) {
        fprintf(stderr, "ARGUMENT_DEPTH_SAMPLER_FAIL %s\n", message.UTF8String);
        exit(1);
    }
}

static NSUInteger alignUp(NSUInteger value, NSUInteger alignment) {
    require(alignment != 0, @"argument encoder alignment is zero");
    return (value + alignment - 1) / alignment * alignment;
}

static id<MTLBuffer> newSharedBuffer(id<MTLDevice> device, NSUInteger length,
                                     uint8_t fill, NSString *description) {
    id<MTLBuffer> buffer = [device newBufferWithLength:length options:MTLResourceStorageModeShared];
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
            require(NO, [NSString stringWithFormat:@"%@ prefix guard changed at %lu",
                description, (unsigned long)i]);
    for (NSUInteger i = end; i < buffer.length; ++i)
        if (bytes[i] != guard)
            require(NO, [NSString stringWithFormat:@"%@ suffix guard changed at %lu",
                description, (unsigned long)i]);
}

static void commitAndWait(id<MTLCommandBuffer> command, NSString *operation) {
    require(command != nil, [operation stringByAppendingString:@" command buffer unavailable"]);
    [command commit];
    [command waitUntilCompleted];
    require(command.status == MTLCommandBufferStatusCompleted && command.error == nil,
        command.error.description ?: [operation stringByAppendingString:@" failed"]);
}

static id<MTLSamplerState> newCompareSampler(id<MTLDevice> device) {
    MTLSamplerDescriptor *descriptor = [MTLSamplerDescriptor new];
    descriptor.normalizedCoordinates = YES;
    descriptor.minFilter = MTLSamplerMinMagFilterNearest;
    descriptor.magFilter = MTLSamplerMinMagFilterNearest;
    descriptor.mipFilter = MTLSamplerMipFilterNotMipmapped;
    descriptor.sAddressMode = MTLSamplerAddressModeClampToEdge;
    descriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
    descriptor.rAddressMode = MTLSamplerAddressModeClampToEdge;
    descriptor.compareFunction = MTLCompareFunctionLess;
    descriptor.supportArgumentBuffers = YES;
    id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:descriptor];
    require(sampler != nil, @"comparison sampler unavailable");
    return sampler;
}

static id<MTLBuffer> newArgumentBuffer(id<MTLDevice> device, id<MTLArgumentEncoder> encoder,
                                       NSUInteger *offset) {
    *offset = alignUp(kArgumentOffset, encoder.alignment);
    return newSharedBuffer(device, *offset + encoder.encodedLength + kArgumentTail,
        kArgumentGuard, @"depth argument buffer");
}

static void encodeDepthArgs(id<MTLArgumentEncoder> encoder, id<MTLBuffer> argumentBuffer,
                            NSUInteger offset, id<MTLTexture> depthTexture,
                            id<MTLSamplerState> sampler) {
    require(offset <= argumentBuffer.length &&
        encoder.encodedLength <= argumentBuffer.length - offset,
        @"depth argument encoding range invalid");
    [encoder setArgumentBuffer:argumentBuffer offset:offset];
    [encoder setTexture:depthTexture atIndex:0];
    [encoder setSamplerState:sampler atIndex:1];
}

static id<MTLRenderPipelineState> newRenderPipeline(id<MTLDevice> device,
                                                    id<MTLFunction> vertexFunction,
                                                    id<MTLFunction> fragmentFunction,
                                                    NSError **error) {
    MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = vertexFunction;
    descriptor.fragmentFunction = fragmentFunction;
    descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
    require(descriptor.vertexFunction != nil && descriptor.fragmentFunction != nil,
        @"depth sampler render functions unavailable");
    id<MTLRenderPipelineState> pipeline =
        [device newRenderPipelineStateWithDescriptor:descriptor error:error];
    NSError *failure = error ? *error : nil;
    require(pipeline != nil, failure.description ?: @"depth sampler render pipeline unavailable");
    return pipeline;
}

static void writeAIR(id<MTLFunction> function, NSString *path) {
    SEL selector = NSSelectorFromString(@"bitcodeData");
    NSData *air = [function respondsToSelector:selector] ?
        ((id (*)(id, SEL))objc_msgSend)(function, selector) : nil;
    require(air.length > 0 && [air writeToFile:path atomically:YES],
        [NSString stringWithFormat:@"AIR write failed for %@", function.name]);
}

static void runComputeCase(id<MTLDevice> device, id<MTLCommandQueue> queue,
                           id<MTLComputePipelineState> pipeline,
                           id<MTLArgumentEncoder> encoder,
                           id<MTLSamplerState> sampler, id<MTLTexture> depthTexture,
                           BOOL nilImage, BOOL reference) {
    const char *caseName = nilImage ? "compute-sample-compare-nil-image" :
        "compute-sample-compare-nonnull";

    NSUInteger argumentOffset = 0;
    id<MTLBuffer> argumentBuffer = newArgumentBuffer(device, encoder, &argumentOffset);
    id<MTLBuffer> output = newSharedBuffer(device, kOutputLength, kOutputGuard,
        @"depth compute output");
    encodeDepthArgs(encoder, argumentBuffer, argumentOffset,
        nilImage ? nil : depthTexture, sampler);
    NSRange argumentRange = NSMakeRange(argumentOffset, encoder.encodedLength);
    requireGuard(argumentBuffer, argumentRange, kArgumentGuard,
        @"depth compute argument buffer after encode");
    NSData *argumentSnapshot = [NSData dataWithBytes:argumentBuffer.contents
        length:argumentBuffer.length];

    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLComputeCommandEncoder> compute = [command computeCommandEncoder];
    require(command != nil && compute != nil, @"depth compute encoder unavailable");
    [compute setComputePipelineState:pipeline];
    [compute setBuffer:argumentBuffer offset:argumentOffset atIndex:3];
    [compute setBuffer:output offset:kOutputOffset atIndex:0];
    if (!nilImage) [compute useResource:depthTexture usage:MTLResourceUsageRead];
    [compute dispatchThreads:MTLSizeMake(kComputeThreads, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(kComputeThreads, 1, 1)];
    [compute endEncoding];
    commitAndWait(command, @"depth comparison compute");

    NSRange outputRange = NSMakeRange(kOutputOffset, kComputeThreads * sizeof(float));
    requireGuard(output, outputRange, kOutputGuard,
        @"depth compute output after GPU");
    const float *values =
        (const float *)((const uint8_t *)output.contents + kOutputOffset);
    if (nilImage) {
        for (NSUInteger i = 0; i < kComputeThreads; ++i)
            require(values[i] == 0.0f,
                [NSString stringWithFormat:@"nil-image compare output[%lu]=%.9g expected=0",
                    (unsigned long)i, values[i]]);
    } else {
        // Less compares the reference (0.5) against sampled depth: 0.5 < 0.25 is false,
        // and 0.5 < 0.75 is true.
        static const float expected[4] = {0.0f, 1.0f, 1.0f, 0.0f};
        for (NSUInteger i = 0; i < kComputeThreads; ++i)
            require(values[i] == expected[i],
                [NSString stringWithFormat:@"compute output[%lu]=%.9g expected=%.9g",
                    (unsigned long)i, values[i], expected[i]]);
    }
    requireGuard(argumentBuffer, argumentRange, kArgumentGuard,
        @"depth compute argument buffer after GPU");
    require(memcmp(argumentSnapshot.bytes, argumentBuffer.contents,
        argumentBuffer.length) == 0, @"GPU modified the compute argument buffer");

    printf("ARGUMENT_DEPTH_SAMPLER_CASE_PASS case=%s backend=%s "
           "outputs=%.1f,%.1f,%.1f,%.1f oracle=%s guards=argument,output\n",
        caseName, reference ? "reference" : "bundle",
        values[0], values[1], values[2], values[3],
        nilImage ? "null_depth_compare_zero" :
        "compare_value<depth,0.5<0.25=0,0.5<0.75=1");
}

static void checkPixelGuard(const float *pixels, NSString *caseName) {
    for (NSUInteger y = 0; y < kRenderSize; ++y) {
        for (NSUInteger x = 0; x < kRenderSize; ++x) {
            if (x >= kRenderOrigin && x < kRenderOrigin + kRenderSpan &&
                y >= kRenderOrigin && y < kRenderOrigin + kRenderSpan)
                continue;
            const float *pixel = pixels + (y * kRenderSize + x) * 4;
            for (NSUInteger channel = 0; channel < 4; ++channel)
                require(pixel[channel] == kRenderGuard,
                    [NSString stringWithFormat:@"%@ pixel guard x=%lu y=%lu channel=%lu "
                        "actual=%.9g expected=%.9g", caseName,
                        (unsigned long)x, (unsigned long)y, (unsigned long)channel,
                        pixel[channel], kRenderGuard]);
        }
    }
}

static void runFragmentCase(id<MTLDevice> device, id<MTLCommandQueue> queue,
                            id<MTLRenderPipelineState> pipeline,
                            id<MTLArgumentEncoder> encoder,
                            id<MTLSamplerState> sampler, id<MTLTexture> depthTexture,
                            BOOL gather, BOOL nilImage, BOOL reference) {
    const char *caseName = gather ?
        (nilImage ? "fragment-gather-compare-nil-image" :
                    "fragment-gather-compare-nonnull") :
        (nilImage ? "fragment-sample-compare-nil-image" :
                    "fragment-sample-compare-nonnull");
    NSString *caseString = [NSString stringWithUTF8String:caseName];

    NSUInteger argumentOffset = 0;
    id<MTLBuffer> argumentBuffer = newArgumentBuffer(device, encoder, &argumentOffset);
    encodeDepthArgs(encoder, argumentBuffer, argumentOffset,
        nilImage ? nil : depthTexture, sampler);
    NSRange argumentRange = NSMakeRange(argumentOffset, encoder.encodedLength);
    requireGuard(argumentBuffer, argumentRange, kArgumentGuard,
        @"depth fragment argument buffer after encode");
    NSData *argumentSnapshot = [NSData dataWithBytes:argumentBuffer.contents
        length:argumentBuffer.length];

    MTLTextureDescriptor *targetDescriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
            width:kRenderSize height:kRenderSize mipmapped:NO];
    targetDescriptor.storageMode = MTLStorageModeShared;
    targetDescriptor.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> target = [device newTextureWithDescriptor:targetDescriptor];
    require(target != nil, @"depth sampler render target unavailable");

    id<MTLCommandBuffer> command = [queue commandBuffer];
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor =
        MTLClearColorMake(kRenderGuard, kRenderGuard, kRenderGuard, kRenderGuard);
    id<MTLRenderCommandEncoder> draw = [command renderCommandEncoderWithDescriptor:pass];
    require(command != nil && draw != nil, @"depth sampler render encoder unavailable");
    [draw setRenderPipelineState:pipeline];
    [draw setViewport:(MTLViewport){
        (double)kRenderOrigin, (double)kRenderOrigin,
        (double)kRenderSpan, (double)kRenderSpan, 0.0, 1.0}];
    [draw setScissorRect:(MTLScissorRect){
        kRenderOrigin, kRenderOrigin, kRenderSpan, kRenderSpan}];
    [draw setFragmentBuffer:argumentBuffer offset:argumentOffset atIndex:3];
    if (!nilImage)
        [draw useResource:depthTexture usage:MTLResourceUsageRead
            stages:MTLRenderStageFragment];
    [draw drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [draw endEncoding];
    commitAndWait(command, gather ? @"depth gather_compare render" :
        @"depth sample_compare render");

    float pixels[64];
    [target getBytes:pixels bytesPerRow:kRenderSize * 4 * sizeof(float)
        fromRegion:MTLRegionMake2D(0, 0, kRenderSize, kRenderSize) mipmapLevel:0];
    checkPixelGuard(pixels, caseString);

    static const float sampleExpected[4] = {0.0f, 1.0f, 1.0f, 0.0f};
    // gather_compare lanes are counter-clockwise from lower-left: BL, BR, TR, TL.
    static const float gatherExpected[4] = {1.0f, 0.0f, 1.0f, 0.0f};
    for (NSUInteger y = 0; y < kRenderSpan; ++y) {
        for (NSUInteger x = 0; x < kRenderSpan; ++x) {
            const float *pixel =
                pixels + ((y + kRenderOrigin) * kRenderSize + x + kRenderOrigin) * 4;
            NSUInteger index = y * kRenderSpan + x;
            if (!nilImage) {
                if (gather) {
                    for (NSUInteger channel = 0; channel < 4; ++channel)
                        require(pixel[channel] == gatherExpected[channel],
                            [NSString stringWithFormat:@"%@ pixel[%lu,%lu] channel=%lu "
                                "actual=%.9g expected=%.9g", caseString,
                                (unsigned long)x, (unsigned long)y,
                                (unsigned long)channel, pixel[channel],
                                gatherExpected[channel]]);
                } else {
                    require(pixel[0] == sampleExpected[index] &&
                        pixel[1] == 0.0f && pixel[2] == 0.0f && pixel[3] == 1.0f,
                        [NSString stringWithFormat:@"%@ pixel[%lu,%lu] rgba=%.9g,%.9g,"
                            "%.9g,%.9g expected=%.9g,0,0,1", caseString,
                            (unsigned long)x, (unsigned long)y,
                            pixel[0], pixel[1], pixel[2], pixel[3],
                            sampleExpected[index]]);
                }
            } else if (gather) {
                for (NSUInteger channel = 0; channel < 4; ++channel)
                    require(pixel[channel] == 0.0f,
                        [NSString stringWithFormat:@"%@ pixel[%lu,%lu] channel=%lu "
                            "actual=%.9g expected=0", caseString,
                            (unsigned long)x, (unsigned long)y,
                            (unsigned long)channel, pixel[channel]]);
            } else {
                require(pixel[0] == 0.0f && pixel[1] == 0.0f &&
                    pixel[2] == 0.0f && pixel[3] == 1.0f,
                    [NSString stringWithFormat:@"%@ pixel[%lu,%lu] rgba=%.9g,%.9g,"
                        "%.9g,%.9g expected=0,0,0,1", caseString,
                        (unsigned long)x, (unsigned long)y,
                        pixel[0], pixel[1], pixel[2], pixel[3]]);
            }
        }
    }
    requireGuard(argumentBuffer, argumentRange, kArgumentGuard,
        @"depth fragment argument buffer after GPU");
    require(memcmp(argumentSnapshot.bytes, argumentBuffer.contents,
        argumentBuffer.length) == 0, @"GPU modified the fragment argument buffer");

    const float *first = pixels + (kRenderOrigin * kRenderSize + kRenderOrigin) * 4;
    printf("ARGUMENT_DEPTH_SAMPLER_CASE_PASS case=%s backend=%s pixels=4 "
           "first_rgba=%.1f,%.1f,%.1f,%.1f oracle=%s guards=argument,pixels\n",
        caseName, reference ? "reference" : "bundle",
        first[0], first[1], first[2], first[3],
        nilImage ? "null_depth_compare_zero" :
        gather ? "gather_compare_lanes=1,0,1,0" :
        "sample_compare_matrix=0,1;1,0");
}

int main(int argc, const char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        puts("Usage: test-argument-depth-sampler BUNDLE|--reference OUTPUT_DIR [--capture-only] [--array]");
        return 0;
    }
    if (argc < 3 || argc > 5) return 2;
    BOOL captureOnly = NO, arrayed = NO;
    for (int i = 3; i < argc; ++i) {
        if (!strcmp(argv[i], "--capture-only") && !captureOnly) captureOnly = YES;
        else if (!strcmp(argv[i], "--array") && !arrayed) arrayed = YES;
        else return 2;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(120);
    @autoreleasepool {
        NSError *error = nil;
        BOOL reference = !strcmp(argv[1], "--reference");
        NSString *directory = @(argv[2]);
        require([[NSFileManager defaultManager] createDirectoryAtPath:directory
            withIntermediateDirectories:YES attributes:nil error:&error],
            error.description ?: @"output directory unavailable");

        id<MTLDevice> device = nil;
        if (reference) device = MTLCreateSystemDefaultDevice();
        else {
            NSBundle *bundle = [NSBundle bundleWithPath:@(argv[1])];
            require([bundle loadAndReturnError:&error],
                error.description ?: @"bundle load failed");
            device = [[[bundle classNamed:@"GarnetDevice"] alloc] init];
        }
        require(device != nil, @"Metal device unavailable");

        NSString *source = @"#include <metal_stdlib>\n"
            "using namespace metal;\n"
            "struct DepthArgs { depth2d<float> image [[id(0)]]; sampler filter [[id(1)]]; };\n"
            "kernel void depth_compare_compute(constant DepthArgs &args [[buffer(3)]], "
            "device float *out [[buffer(0)]], uint i [[thread_position_in_grid]]) {\n"
            "  float2 uv = float2((i & 1u) ? 0.75f : 0.25f, "
            "(i & 2u) ? 0.75f : 0.25f);\n"
            "  out[i] = args.image.sample_compare(args.filter, uv, 0.5f, level(0.0f));\n"
            "}\n"
            "struct DepthVertex { float4 position [[position]]; };\n"
            "vertex DepthVertex depth_compare_vertex(uint vid [[vertex_id]]) {\n"
            "  const float2 p[3] = { float2(-1.0f, -1.0f), float2(3.0f, -1.0f), "
            "float2(-1.0f, 3.0f) };\n"
            "  DepthVertex out; out.position = float4(p[vid], 0.0f, 1.0f); return out;\n"
            "}\n"
            "fragment float4 depth_compare_sample_fragment(DepthVertex in [[stage_in]], "
            "constant DepthArgs &args [[buffer(3)]]) {\n"
            "  float2 uv = (in.position.xy - float2(1.0f)) * 0.5f;\n"
            "  float pass = args.image.sample_compare(args.filter, uv, 0.5f);\n"
            "  return float4(pass, 0.0f, 0.0f, 1.0f);\n"
            "}\n"
            "fragment float4 depth_compare_gather_fragment(DepthVertex in [[stage_in]], "
            "constant DepthArgs &args [[buffer(3)]]) {\n"
            "  float4 pass = args.image.gather_compare(args.filter, "
            "float2(0.5f), 0.5f);\n"
            "  return pass;\n"
            "}\n";
        if (arrayed) {
            source = [source stringByReplacingOccurrencesOfString:@"depth2d<float>"
                withString:@"depth2d_array<float>"];
            source = [source stringByReplacingOccurrencesOfString:@"args.filter, uv, 0.5f"
                withString:@"args.filter, uv, 1u, 0.5f"];
            source = [source stringByReplacingOccurrencesOfString:@"float2(0.5f), 0.5f"
                withString:@"float2(0.5f), 1u, 0.5f"];
        }
        require([source writeToFile:[directory stringByAppendingPathComponent:
            @"depth-sampler.metal"] atomically:YES encoding:NSUTF8StringEncoding
            error:&error], @"source write failed");
        MTLCompileOptions *options = [MTLCompileOptions new];
        options.languageVersion = MTLLanguageVersion2_1;
        id<MTLLibrary> library = [device newLibraryWithSource:source options:options
            error:&error];
        require(library != nil, error.description ?: @"source compilation failed");

        id<MTLFunction> computeFunction = [library newFunctionWithName:@"depth_compare_compute"];
        id<MTLFunction> vertexFunction = [library newFunctionWithName:@"depth_compare_vertex"];
        id<MTLFunction> sampleFunction =
            [library newFunctionWithName:@"depth_compare_sample_fragment"];
        id<MTLFunction> gatherFunction =
            [library newFunctionWithName:@"depth_compare_gather_fragment"];
        require(computeFunction != nil && vertexFunction != nil &&
            sampleFunction != nil && gatherFunction != nil,
            @"depth sampler shader function unavailable");
        writeAIR(computeFunction, [directory stringByAppendingPathComponent:
            @"depth-sampler-compute.air"]);
        writeAIR(vertexFunction, [directory stringByAppendingPathComponent:
            @"depth-sampler-vertex.air"]);
        writeAIR(sampleFunction, [directory stringByAppendingPathComponent:
            @"depth-sampler-fragment-sample.air"]);
        writeAIR(gatherFunction, [directory stringByAppendingPathComponent:
            @"depth-sampler-fragment-gather.air"]);
        if (captureOnly) {
            printf("ARGUMENT_DEPTH_SAMPLER_CAPTURE backend=%s arrayed=%d air=4 "
                   "gpu_execution=0\n",
                reference ? "reference" : "bundle", arrayed);
            return 0;
        }

        id<MTLArgumentEncoder> computeEncoder =
            [computeFunction newArgumentEncoderWithBufferIndex:3];
        id<MTLArgumentEncoder> sampleEncoder =
            [sampleFunction newArgumentEncoderWithBufferIndex:3];
        id<MTLArgumentEncoder> gatherEncoder =
            [gatherFunction newArgumentEncoderWithBufferIndex:3];
        require(computeEncoder != nil && sampleEncoder != nil && gatherEncoder != nil,
            @"depth argument encoder unavailable");
        require(computeEncoder.encodedLength == 16 &&
            sampleEncoder.encodedLength == 16 &&
            gatherEncoder.encodedLength == 16,
            @"depth argument layout is not depth-resource followed by sampler");

        id<MTLComputePipelineState> computePipeline =
            [device newComputePipelineStateWithFunction:computeFunction error:&error];
        require(computePipeline != nil,
            error.description ?: @"depth compute pipeline unavailable");
        id<MTLRenderPipelineState> samplePipeline =
            newRenderPipeline(device, vertexFunction, sampleFunction, &error);
        id<MTLRenderPipelineState> gatherPipeline =
            newRenderPipeline(device, vertexFunction, gatherFunction, &error);
        id<MTLCommandQueue> queue = [device newCommandQueue];
        require(queue != nil, @"depth command queue unavailable");

        id<MTLSamplerState> sampler = newCompareSampler(device);

        MTLTextureDescriptor *depthDescriptor =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
                MTLPixelFormatDepth32Float width:kDepthSize height:kDepthSize
                mipmapped:NO];
        depthDescriptor.storageMode = MTLStorageModePrivate;
        depthDescriptor.usage = MTLTextureUsageShaderRead;
        if (arrayed) {
            depthDescriptor.textureType = MTLTextureType2DArray;
            depthDescriptor.arrayLength = 2;
        }
        id<MTLTexture> depthTexture = [device newTextureWithDescriptor:depthDescriptor];
        require(depthTexture != nil, @"Depth32Float sample texture unavailable");

        NSUInteger uploadBytes = kDepthBytes * (arrayed ? 2 : 1);
        id<MTLBuffer> upload = newSharedBuffer(device,
            kUploadOffset + uploadBytes + 64, kUploadGuard, @"depth upload");
        const float texels[4] = {0.25f, 0.75f, 0.75f, 0.25f};
        const float decoy[4] = {0.75f, 0.25f, 0.25f, 0.75f};
        memcpy((uint8_t *)upload.contents + kUploadOffset,
            arrayed ? decoy : texels, kDepthBytes);
        if (arrayed)
            memcpy((uint8_t *)upload.contents + kUploadOffset + kDepthBytes,
                texels, kDepthBytes);
        id<MTLCommandBuffer> uploadCommand = [queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [uploadCommand blitCommandEncoder];
        require(uploadCommand != nil && blit != nil, @"depth upload encoder unavailable");
        for (NSUInteger slice = 0; slice < (arrayed ? 2u : 1u); ++slice)
            [blit copyFromBuffer:upload sourceOffset:kUploadOffset + slice * kDepthBytes
            sourceBytesPerRow:kDepthSize * sizeof(float)
            sourceBytesPerImage:kDepthBytes
            sourceSize:MTLSizeMake(kDepthSize, kDepthSize, 1)
            toTexture:depthTexture destinationSlice:slice destinationLevel:0
            destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit endEncoding];
        commitAndWait(uploadCommand, @"Depth32Float upload");
        requireGuard(upload, NSMakeRange(kUploadOffset, uploadBytes),
            kUploadGuard, @"depth upload after GPU");

        runComputeCase(device, queue, computePipeline, computeEncoder,
            sampler, depthTexture, NO, reference);
        runFragmentCase(device, queue, samplePipeline, sampleEncoder,
            sampler, depthTexture, NO, NO, reference);
        runFragmentCase(device, queue, gatherPipeline, gatherEncoder,
            sampler, depthTexture, YES, NO, reference);
        if (!reference) {
            runComputeCase(device, queue, computePipeline, computeEncoder,
                sampler, depthTexture, YES, reference);
            runFragmentCase(device, queue, samplePipeline, sampleEncoder,
                sampler, depthTexture, NO, YES, reference);
            runFragmentCase(device, queue, gatherPipeline, gatherEncoder,
                sampler, depthTexture, YES, YES, reference);
        }

        printf("ARGUMENT_DEPTH_SAMPLER_PASS backend=%s arrayed=%d strong_cases=3 "
               "nil_image_cases=%d msl=2.1 depth=Depth32Float "
               "pattern=0.25,0.75 compare=Less threshold=0.5 "
               "guards=argument,output,pixels\n",
            reference ? "reference" : "bundle", arrayed, reference ? 0 : 3);
    }
    return 0;
}
