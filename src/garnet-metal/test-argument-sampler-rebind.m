#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const uint8_t kSourceGuard = 0xa5;
static const uint8_t kCopyGuard = 0xb6;
static const uint8_t kPrivateGuard = 0xd3;
static const uint8_t kOutputGuard = 0xcd;
static const NSUInteger kSourceOffset = 256;
static const NSUInteger kDestinationOffset = 192;
static const NSUInteger kOutputOffset = 64;
static const NSUInteger kOutputLength = 128;
static const NSUInteger kTailGuard = 256;
static const NSUInteger kSamplerTokenOffset = sizeof(uint64_t);

static void require(BOOL ok, NSString *message) {
    if (!ok) { fprintf(stderr, "ARGUMENT_SAMPLER_REBIND_FAIL %s\n", message.UTF8String); exit(1); }
}

static void commitAndWait(id<MTLCommandBuffer> command, NSString *operation) {
    [command commit];
    [command waitUntilCompleted];
    require(command.status == MTLCommandBufferStatusCompleted && !command.error,
        command.error.description ?: [operation stringByAppendingString:@" failed"]);
}

static id<MTLSamplerState> newSampler(id<MTLDevice> device, BOOL clampToEdge) {
    MTLSamplerDescriptor *descriptor = [MTLSamplerDescriptor new];
    descriptor.minFilter = MTLSamplerMinMagFilterNearest;
    descriptor.magFilter = MTLSamplerMinMagFilterNearest;
    descriptor.mipFilter = MTLSamplerMipFilterNotMipmapped;
    descriptor.sAddressMode = clampToEdge ? MTLSamplerAddressModeClampToEdge : MTLSamplerAddressModeRepeat;
    descriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
    descriptor.normalizedCoordinates = YES;
    descriptor.supportArgumentBuffers = YES;
    id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:descriptor];
    require(sampler != nil, clampToEdge ? @"clamp sampler unavailable" : @"repeat sampler unavailable");
    return sampler;
}

static id<MTLBuffer> newSharedBuffer(id<MTLDevice> device, NSUInteger length, uint8_t fill,
                                     NSString *description) {
    id<MTLBuffer> buffer = [device newBufferWithLength:length options:MTLResourceStorageModeShared];
    require(buffer != nil && buffer.contents != NULL, [description stringByAppendingString:@" unavailable"]);
    memset(buffer.contents, fill, length);
    return buffer;
}

static void requireGuard(id<MTLBuffer> buffer, NSRange payload, uint8_t guard, NSString *description) {
    require(buffer.contents != NULL && payload.location <= buffer.length &&
        payload.length <= buffer.length - payload.location, [description stringByAppendingString:@" range invalid"]);
    const uint8_t *bytes = buffer.contents;
    NSUInteger end = payload.location + payload.length;
    for (NSUInteger i = 0; i < payload.location; ++i)
        if (bytes[i] != guard)
            require(NO, [NSString stringWithFormat:@"%@ prefix guard changed at %lu", description,
                (unsigned long)i]);
    for (NSUInteger i = end; i < buffer.length; ++i)
        if (bytes[i] != guard)
            require(NO, [NSString stringWithFormat:@"%@ suffix guard changed at %lu", description,
                (unsigned long)i]);
}

static void encodeArguments(id<MTLArgumentEncoder> encoder, id<MTLTexture> texture,
                            id<MTLSamplerState> sampler, id<MTLBuffer> buffer, NSUInteger offset) {
    require(buffer.contents != NULL && offset <= buffer.length &&
        encoder.encodedLength <= buffer.length - offset, @"argument encoding range invalid");
    [encoder setArgumentBuffer:buffer offset:offset];
    [encoder setTexture:texture atIndex:0];
    [encoder setSamplerState:sampler atIndex:1];
}

static float requireOutput(id<MTLBuffer> output, float expected, NSString *description) {
    require(output.contents != NULL && kOutputOffset + sizeof(float) <= output.length,
        @"output buffer range invalid");
    requireGuard(output, NSMakeRange(kOutputOffset, sizeof(float)), kOutputGuard, description);
    float actual = 0.0f;
    memcpy(&actual, (const uint8_t *)output.contents + kOutputOffset, sizeof actual);
    require(fabsf(actual - expected) <= 1.0e-6f,
        [NSString stringWithFormat:@"%@ output=%.9g expected=%.9g", description, actual, expected]);
    return actual;
}

static void printCase(const char *name, NSUInteger round, float actual, float expected, BOOL reference) {
    printf("ARGUMENT_SAMPLER_REBIND_CASE_PASS case=%s round=%lu output=%.9g expected=%.9g "
           "guards=1 candidate_early_release=%d\n",
        name, (unsigned long)round, actual, expected, !reference);
}

static void runCpuCopyCase(id<MTLDevice> device, id<MTLCommandQueue> queue,
                           id<MTLComputePipelineState> pipeline, id<MTLArgumentEncoder> encoder,
                           id<MTLTexture> texture, BOOL clampToEdge, BOOL reference,
                           NSMutableArray<id<MTLSamplerState>> *referenceKeepAlive) {
    NSUInteger argLength = encoder.encodedLength;
    float expected = clampToEdge ? 29.0f : 11.0f;
    id<MTLSamplerState> sampler = newSampler(device, clampToEdge);
    id<MTLBuffer> source = newSharedBuffer(device, kSourceOffset + argLength + kTailGuard,
        kSourceGuard, @"CPU-copy source");
    id<MTLBuffer> destination = newSharedBuffer(device, kDestinationOffset + argLength + kTailGuard,
        kCopyGuard, @"CPU-copy destination");
    id<MTLBuffer> output = newSharedBuffer(device, kOutputLength, kOutputGuard, @"CPU-copy output");

    encodeArguments(encoder, texture, sampler, source, kSourceOffset);
    NSRange sourceRange = NSMakeRange(kSourceOffset, argLength);
    NSRange destinationRange = NSMakeRange(kDestinationOffset, argLength);
    requireGuard(source, sourceRange, kSourceGuard, @"CPU-copy source after encode");
    memcpy((uint8_t *)destination.contents + kDestinationOffset,
        (const uint8_t *)source.contents + kSourceOffset, argLength);
    requireGuard(source, sourceRange, kSourceGuard, @"CPU-copy source after memcpy");
    requireGuard(destination, destinationRange, kCopyGuard, @"CPU-copy destination after memcpy");
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLComputeCommandEncoder> compute = [command computeCommandEncoder];
    require(command != nil && compute != nil, @"CPU-copy command encoder unavailable");
    [compute setComputePipelineState:pipeline];
    [compute setBuffer:destination offset:kDestinationOffset atIndex:1];
    [compute setBuffer:output offset:kOutputOffset atIndex:0];
    [compute useResource:texture usage:MTLResourceUsageRead];
    [compute dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [compute endEncoding];

    if (reference) [referenceKeepAlive addObject:sampler];
    else sampler = nil;
    commitAndWait(command, @"CPU-copy consumer");

    requireGuard(source, sourceRange, kSourceGuard, @"CPU-copy source after GPU");
    requireGuard(destination, destinationRange, kCopyGuard, @"CPU-copy destination after GPU");
    float actual = requireOutput(output, expected,
        clampToEdge ? @"CPU-copy clamp" : @"CPU-copy repeat");
    printCase(clampToEdge ? "cpu-copy-clamp" : "cpu-copy-repeat", 1, actual, expected, reference);
}

static void runSharedWritePatchCase(id<MTLDevice> device, id<MTLCommandQueue> queue,
                                    id<MTLComputePipelineState> producer,
                                    id<MTLComputePipelineState> consumer,
                                    id<MTLArgumentEncoder> encoder, id<MTLTexture> texture,
                                    BOOL reference,
                                    NSMutableArray<id<MTLSamplerState>> *referenceKeepAlive) {
    NSUInteger argLength = encoder.encodedLength;
    id<MTLSamplerState> repeat = newSampler(device, NO);
    id<MTLSamplerState> clamp = newSampler(device, YES);
    id<MTLBuffer> source = newSharedBuffer(device, kSourceOffset + argLength + kTailGuard,
        kSourceGuard, @"shared GPU producer source");
    id<MTLBuffer> patch = newSharedBuffer(device, kDestinationOffset + argLength + kTailGuard,
        kCopyGuard, @"shared CPU patch source");
    id<MTLBuffer> destination = newSharedBuffer(device, kDestinationOffset + argLength + kTailGuard,
        kPrivateGuard, @"shared GPU-written destination");
    id<MTLBuffer> output = newSharedBuffer(device, kOutputLength, kOutputGuard,
        @"shared GPU-written output");

    encodeArguments(encoder, texture, repeat, source, kSourceOffset);
    encodeArguments(encoder, texture, clamp, patch, kDestinationOffset);
    NSRange sourceRange = NSMakeRange(kSourceOffset, argLength);
    NSRange destinationRange = NSMakeRange(kDestinationOffset, argLength);
    requireGuard(source, sourceRange, kSourceGuard, @"shared producer source after encode");
    requireGuard(patch, destinationRange, kCopyGuard, @"shared patch source after encode");
    requireGuard(destination, destinationRange, kPrivateGuard,
        @"shared GPU-written destination before producer");

    id<MTLCommandBuffer> producerCommand = [queue commandBuffer];
    id<MTLComputeCommandEncoder> producerEncoder = [producerCommand computeCommandEncoder];
    require(producerCommand != nil && producerEncoder != nil, @"shared producer encoder unavailable");
    [producerEncoder setComputePipelineState:producer];
    [producerEncoder setBuffer:destination offset:kDestinationOffset atIndex:1];
    [producerEncoder setBuffer:source offset:kSourceOffset atIndex:2];
    [producerEncoder useResource:texture usage:MTLResourceUsageRead];
    [producerEncoder dispatchThreadgroups:MTLSizeMake(1, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [producerEncoder endEncoding];
    commitAndWait(producerCommand, @"shared GPU producer");

    requireGuard(destination, destinationRange, kPrivateGuard,
        @"shared GPU-written destination after producer");
    // Keep the GPU-produced image token and replace only the sampler token.
    memcpy((uint8_t *)destination.contents + kDestinationOffset + kSamplerTokenOffset,
        (const uint8_t *)patch.contents + kDestinationOffset + kSamplerTokenOffset, sizeof(uint64_t));
    requireGuard(destination, destinationRange, kPrivateGuard,
        @"shared destination after CPU patch");

    id<MTLCommandBuffer> consumerCommand = [queue commandBuffer];
    id<MTLComputeCommandEncoder> consumerEncoder = [consumerCommand computeCommandEncoder];
    require(consumerCommand != nil && consumerEncoder != nil, @"shared consumer encoder unavailable");
    [consumerEncoder setComputePipelineState:consumer];
    [consumerEncoder setBuffer:destination offset:kDestinationOffset atIndex:1];
    [consumerEncoder setBuffer:output offset:kOutputOffset atIndex:0];
    [consumerEncoder useResource:texture usage:MTLResourceUsageRead];
    [consumerEncoder dispatchThreadgroups:MTLSizeMake(1, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [consumerEncoder endEncoding];

    if (reference) {
        [referenceKeepAlive addObject:repeat];
        [referenceKeepAlive addObject:clamp];
    } else {
        repeat = nil;
        clamp = nil;
    }
    commitAndWait(consumerCommand, @"shared GPU-write then CPU-patch consumer");

    requireGuard(source, sourceRange, kSourceGuard, @"shared producer source after GPU");
    requireGuard(patch, destinationRange, kCopyGuard, @"shared patch source after GPU");
    requireGuard(destination, destinationRange, kPrivateGuard, @"shared destination after GPU");
    float actual = requireOutput(output, 29.0f, @"shared GPU-write then CPU-patch clamp");
    printCase("gpu-shared-write-cpu-patch-clamp", 2, actual, 29.0f, reference);
}

static void runPrivateBlitCase(id<MTLDevice> device, id<MTLCommandQueue> queue,
                               id<MTLComputePipelineState> consumer,
                               id<MTLArgumentEncoder> encoder, id<MTLTexture> texture,
                               BOOL clampToEdge, BOOL reference,
                               NSMutableArray<id<MTLSamplerState>> *referenceKeepAlive) {
    NSUInteger argLength = encoder.encodedLength;
    NSUInteger bufferLength = kDestinationOffset + argLength + kTailGuard;
    float expected = clampToEdge ? 29.0f : 11.0f;
    id<MTLSamplerState> sampler = newSampler(device, clampToEdge);
    id<MTLBuffer> source = newSharedBuffer(device, kSourceOffset + argLength + kTailGuard,
        kSourceGuard, @"private-blit source");
    id<MTLBuffer> guardSeed = newSharedBuffer(device, bufferLength, kPrivateGuard,
        @"private-blit guard seed");
    id<MTLBuffer> readback = newSharedBuffer(device, bufferLength, 0, @"private-blit readback");
    id<MTLBuffer> output = newSharedBuffer(device, kOutputLength, kOutputGuard,
        @"private-blit output");
    id<MTLBuffer> destination = [device newBufferWithLength:bufferLength
        options:MTLResourceStorageModePrivate];
    require(destination != nil, @"private-blit destination unavailable");

    encodeArguments(encoder, texture, sampler, source, kSourceOffset);
    NSRange sourceRange = NSMakeRange(kSourceOffset, argLength);
    NSRange destinationRange = NSMakeRange(kDestinationOffset, argLength);
    requireGuard(source, sourceRange, kSourceGuard, @"private-blit source after encode");

    id<MTLCommandBuffer> blitCommand = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [blitCommand blitCommandEncoder];
    require(blitCommand != nil && blit != nil, @"private-blit setup encoder unavailable");
    [blit copyFromBuffer:guardSeed sourceOffset:0 toBuffer:destination destinationOffset:0
        size:bufferLength];
    [blit copyFromBuffer:source sourceOffset:kSourceOffset toBuffer:destination
        destinationOffset:kDestinationOffset size:argLength];
    [blit endEncoding];
    commitAndWait(blitCommand, @"private-blit setup");

    id<MTLCommandBuffer> consumerCommand = [queue commandBuffer];
    id<MTLComputeCommandEncoder> compute = [consumerCommand computeCommandEncoder];
    require(consumerCommand != nil && compute != nil, @"private-blit consumer encoder unavailable");
    [compute setComputePipelineState:consumer];
    [compute setBuffer:destination offset:kDestinationOffset atIndex:1];
    [compute setBuffer:output offset:kOutputOffset atIndex:0];
    [compute useResource:texture usage:MTLResourceUsageRead];
    [compute dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [compute endEncoding];

    blit = [consumerCommand blitCommandEncoder];
    require(blit != nil, @"private-blit readback encoder unavailable");
    [blit copyFromBuffer:destination sourceOffset:0 toBuffer:readback destinationOffset:0
        size:bufferLength];
    [blit endEncoding];

    if (reference) [referenceKeepAlive addObject:sampler];
    else sampler = nil;
    commitAndWait(consumerCommand, @"private-blit consumer");

    requireGuard(source, sourceRange, kSourceGuard, @"private-blit source after GPU");
    requireGuard(readback, destinationRange, kPrivateGuard, @"private-blit destination after GPU");
    float actual = requireOutput(output, expected,
        clampToEdge ? @"private-blit clamp" : @"private-blit repeat");
    printCase(clampToEdge ? "gpu-blit-private-clamp" : "gpu-blit-private-repeat",
        3, actual, expected, reference);
}

static void writeAIR(id<MTLFunction> function, NSString *path) {
    SEL selector = NSSelectorFromString(@"bitcodeData");
    NSData *air = [function respondsToSelector:selector] ?
        ((id (*)(id, SEL))objc_msgSend)(function, selector) : nil;
    require(air.length && [air writeToFile:path atomically:YES], @"AIR write failed");
}

int main(int argc, const char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        puts("Usage: test-argument-sampler-rebind BUNDLE OUTPUT_DIR");
        puts("       test-argument-sampler-rebind --reference OUTPUT_DIR");
        return 0;
    }
    if (argc != 3) return 2;
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
            "struct Args { texture2d<float> image [[id(0)]]; sampler filter [[id(1)]]; };\n"
            "kernel void sample_sampler(constant Args &args [[buffer(1)]], device float *out [[buffer(0)]]) {"
            " out[0] = args.image.sample(args.filter, float2(1.25, 0.5)).x; }\n"
            "kernel void copy_sampler_args(device Args &dst [[buffer(1)]], constant Args &src [[buffer(2)]]) {"
            " dst.image = src.image; dst.filter = src.filter; }\n";
        require([source writeToFile:[directory stringByAppendingPathComponent:@"sampler-rebind.metal"]
            atomically:YES encoding:NSUTF8StringEncoding error:&error], @"source write failed");
        MTLCompileOptions *options = [MTLCompileOptions new];
        options.languageVersion = MTLLanguageVersion2_1;
        id<MTLLibrary> library = [device newLibraryWithSource:source options:options error:&error];
        require(library != nil, error.description ?: @"source compilation failed");

        id<MTLFunction> consumerFunction = [library newFunctionWithName:@"sample_sampler"];
        id<MTLFunction> producerFunction = [library newFunctionWithName:@"copy_sampler_args"];
        require(consumerFunction != nil && producerFunction != nil, @"shader function unavailable");
        writeAIR(consumerFunction,
            [directory stringByAppendingPathComponent:@"sampler-rebind-consumer.air"]);
        writeAIR(producerFunction,
            [directory stringByAppendingPathComponent:@"sampler-rebind-producer.air"]);

        id<MTLArgumentEncoder> encoder = [consumerFunction newArgumentEncoderWithBufferIndex:1];
        require(encoder != nil && encoder.encodedLength == 16 &&
            encoder.alignment && !(kSourceOffset % encoder.alignment) &&
            !(kDestinationOffset % encoder.alignment), @"argument encoder unavailable");
        id<MTLComputePipelineState> consumer =
            [device newComputePipelineStateWithFunction:consumerFunction error:&error];
        require(consumer != nil, error.description ?: @"sampler consumer pipeline failed");
        id<MTLComputePipelineState> producer =
            [device newComputePipelineStateWithFunction:producerFunction error:&error];
        require(producer != nil, error.description ?: @"argument producer pipeline failed");
        id<MTLCommandQueue> queue = [device newCommandQueue];
        require(queue != nil, @"command queue unavailable");

        MTLTextureDescriptor *textureDescriptor =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                width:2 height:1 mipmapped:NO];
        textureDescriptor.storageMode = MTLStorageModeShared;
        textureDescriptor.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> texture = [device newTextureWithDescriptor:textureDescriptor];
        require(texture != nil, @"sampler texture unavailable");
        const float pixels[8] = {11.0f, 0.0f, 0.0f, 1.0f, 29.0f, 0.0f, 0.0f, 1.0f};
        [texture replaceRegion:MTLRegionMake2D(0, 0, 2, 1) mipmapLevel:0
            withBytes:pixels bytesPerRow:sizeof pixels];

        __attribute__((objc_precise_lifetime))
        NSMutableArray<id<MTLSamplerState>> *referenceKeepAlive = [NSMutableArray array];
        __attribute__((objc_precise_lifetime))
        NSMutableArray<id<MTLSamplerState>> *decoys = [NSMutableArray array];
        id<MTLBuffer> scratch = newSharedBuffer(device, encoder.encodedLength, 0xd4,
            @"decoy sampler scratch");
        [encoder setArgumentBuffer:scratch offset:0];
        for (NSUInteger i = 0; i < 128; ++i) {
            MTLSamplerDescriptor *descriptor = [MTLSamplerDescriptor new];
            descriptor.supportArgumentBuffers = YES;
            descriptor.lodMaxClamp = (float)i + 1.0f;
            descriptor.sAddressMode = (i & 1) ? MTLSamplerAddressModeClampToEdge :
                MTLSamplerAddressModeRepeat;
            descriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
            id<MTLSamplerState> decoy = [device newSamplerStateWithDescriptor:descriptor];
            require(decoy != nil, @"decoy sampler allocation failed");
            [decoys addObject:decoy];
            [encoder setSamplerState:decoy atIndex:1];
        }
        [encoder setSamplerState:nil atIndex:1];
        printf("ARGUMENT_SAMPLER_REBIND_DECOYS tokenized=128 alive=128 device_capacity=%lu\n",
            (unsigned long)device.maxArgumentBufferSamplerCount);

        runCpuCopyCase(device, queue, consumer, encoder, texture, NO, reference, referenceKeepAlive);
        runCpuCopyCase(device, queue, consumer, encoder, texture, YES, reference, referenceKeepAlive);
        runSharedWritePatchCase(device, queue, producer, consumer, encoder, texture,
            reference, referenceKeepAlive);
        runPrivateBlitCase(device, queue, consumer, encoder, texture, NO,
            reference, referenceKeepAlive);
        runPrivateBlitCase(device, queue, consumer, encoder, texture, YES,
            reference, referenceKeepAlive);

        require(decoys.count == 128, @"decoy samplers were released during the test");
        printf("ARGUMENT_SAMPLER_REBIND_PASS rounds=3 cases=5 decoys=128 repeat=11 clamp=29 "
               "modes=cpu-copy,gpu-shared-cpu-patch,gpu-blit-private reference_keepalive=%lu\n",
            (unsigned long)referenceKeepAlive.count);
    }
    return 0;
}
