#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const NSUInteger kDecoyCount = 128;
static const NSUInteger kArgumentOffset = 256;
static const NSUInteger kOutputOffset = 16;
static const NSUInteger kOutputLength = 64;
static const NSUInteger kCandidateLength = 64;
static const NSUInteger kTailGuard = 64;
static const uint8_t kArgumentGuard = 0xa5;
static const uint8_t kOutputGuard = 0xcd;

static void require(BOOL ok, NSString *message) {
    if (!ok) { fprintf(stderr, "ARGUMENT_SAMPLER_RETIRE_FAIL %s\n", message.UTF8String); exit(1); }
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

static void requireAllZero(id<MTLBuffer> buffer, NSString *description) {
    require(buffer.contents != NULL, [description stringByAppendingString:@" unavailable"]);
    const uint8_t *bytes = buffer.contents;
    NSUInteger words = buffer.length / sizeof(uint64_t);
    const uint64_t *values = (const uint64_t *)bytes;
    for (NSUInteger i = 0; i < words; ++i)
        if (values[i])
            require(NO, [NSString stringWithFormat:@"%@ changed at word %lu", description,
                (unsigned long)i]);
    for (NSUInteger i = words * sizeof(uint64_t); i < buffer.length; ++i)
        if (bytes[i])
            require(NO, [NSString stringWithFormat:@"%@ changed at byte %lu", description,
                (unsigned long)i]);
}

static void requireZeroWordAndGuard(id<MTLBuffer> buffer, NSString *description) {
    require(buffer.contents != NULL && buffer.length >= sizeof(uint32_t),
        [description stringByAppendingString:@" unavailable"]);
    uint32_t word = 1;
    memcpy(&word, buffer.contents, sizeof word);
    require(word == 0, [NSString stringWithFormat:@"%@ word=%#x expected=0", description, word]);
    const uint8_t *bytes = buffer.contents;
    for (NSUInteger i = sizeof(uint32_t); i < buffer.length; ++i)
        if (bytes[i] != kOutputGuard)
            require(NO, [NSString stringWithFormat:@"%@ guard changed at %lu", description,
                (unsigned long)i]);
}

static id<MTLSamplerState> newSampler(id<MTLDevice> device, NSUInteger tag) {
    MTLSamplerDescriptor *descriptor = [MTLSamplerDescriptor new];
    descriptor.supportArgumentBuffers = YES;
    descriptor.minFilter = MTLSamplerMinMagFilterNearest;
    descriptor.magFilter = MTLSamplerMinMagFilterNearest;
    descriptor.mipFilter = MTLSamplerMipFilterNotMipmapped;
    descriptor.sAddressMode = (tag & 1u) ? MTLSamplerAddressModeRepeat :
        MTLSamplerAddressModeClampToEdge;
    descriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
    descriptor.rAddressMode = MTLSamplerAddressModeClampToEdge;
    descriptor.normalizedCoordinates = YES;
    descriptor.lodMinClamp = 0.0f;
    descriptor.lodMaxClamp = 1.0f + (float)tag;
    id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:descriptor];
    require(sampler != nil, @"sampler allocation failed");
    return sampler;
}

static id<MTLTexture> newSampleTexture(id<MTLDevice> device) {
    MTLTextureDescriptor *descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
            width:1 height:1 mipmapped:NO];
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
    require(texture != nil, @"sample texture unavailable");
    const float texel[4] = {7.0f, 0.0f, 0.0f, 1.0f};
    [texture replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0
        withBytes:texel bytesPerRow:sizeof texel];
    return texture;
}

static id<MTLArgumentEncoder> newEncoder(id<MTLLibrary> library, NSString *name) {
    id<MTLFunction> function = [library newFunctionWithName:name];
    require(function != nil, [NSString stringWithFormat:@"missing function %@", name]);
    id<MTLArgumentEncoder> encoder = [function newArgumentEncoderWithBufferIndex:1];
    require(encoder != nil && encoder.encodedLength > 0 && encoder.alignment > 0,
        [NSString stringWithFormat:@"missing argument encoder %@", name]);
    return encoder;
}

static id<MTLArgumentEncoder> newFunctionEncoder(id<MTLFunction> function) {
    require(function != nil, @"missing sampling function");
    id<MTLArgumentEncoder> encoder = [function newArgumentEncoderWithBufferIndex:1];
    require(encoder != nil && encoder.encodedLength > 0 && encoder.alignment > 0,
        @"missing sampling argument encoder");
    return encoder;
}

static void encodeSamplerArguments(id<MTLArgumentEncoder> encoder, id<MTLTexture> texture,
                                   id<MTLSamplerState> sampler, id<MTLBuffer> buffer,
                                   NSUInteger offset) {
    require(buffer.contents != NULL && offset <= buffer.length &&
        encoder.encodedLength <= buffer.length - offset, @"sampler argument range invalid");
    [encoder setArgumentBuffer:buffer offset:offset];
    [encoder setTexture:texture atIndex:0];
    [encoder setSamplerState:sampler atIndex:1];
}

static id<MTLBuffer> rawSamplerArguments(id<MTLDevice> device,
                                         id<MTLFunction> samplingFunction,
                                         id<MTLTexture> texture,
                                         id<MTLSamplerState> sampler,
                                         NSString *description) {
    id<MTLArgumentEncoder> encoder = newFunctionEncoder(samplingFunction);
    id<MTLBuffer> encoded = newSharedBuffer(device, encoder.encodedLength, 0,
        @"raw sampler argument source");
    encodeSamplerArguments(encoder, texture, sampler, encoded, 0);
    id<MTLBuffer> raw = newSharedBuffer(device, encoder.encodedLength + kTailGuard,
        kArgumentGuard, description);
    memcpy(raw.contents, encoded.contents, encoder.encodedLength);
    [encoder setArgumentBuffer:nil offset:0];
    return raw;
}

static float runSamplingProbe(id<MTLDevice> device, id<MTLCommandQueue> queue,
                              id<MTLComputePipelineState> samplingPipeline,
                              id<MTLTexture> texture, id<MTLBuffer> arguments, NSUInteger offset,
                              NSString *description) {
    require(!(offset & 7u), [description stringByAppendingString:@" offset is misaligned"]);
    id<MTLBuffer> output = newSharedBuffer(device, kOutputLength, kOutputGuard, description);
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLComputeCommandEncoder> compute = [command computeCommandEncoder];
    require(command != nil && compute != nil,
        [description stringByAppendingString:@" encoder unavailable"]);
    [compute setComputePipelineState:samplingPipeline];
    [compute setBuffer:arguments offset:offset atIndex:1];
    [compute setBuffer:output offset:kOutputOffset atIndex:0];
    [compute useResource:texture usage:MTLResourceUsageRead];
    [compute dispatchThreadgroups:MTLSizeMake(1, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [compute endEncoding];
    [command commit];
    [command waitUntilCompleted];
    require(command.status == MTLCommandBufferStatusCompleted && !command.error,
        command.error.description ?: [description stringByAppendingString:@" sampling failed"]);
    requireGuard(output, kOutputOffset, sizeof(float), kOutputGuard, description);
    float actual = 0.0f;
    memcpy(&actual, (const uint8_t *)output.contents + kOutputOffset, sizeof actual);
    require(fabsf(actual - 7.0f) <= 1.0e-6f,
        [NSString stringWithFormat:@"%@ output=%g expected=7", description, actual]);
    return actual;
}

static void runScanBudgetCase(id<MTLDevice> device, id<MTLCommandQueue> queue,
                              id<MTLComputePipelineState> samplingPipeline,
                              id<MTLArgumentEncoder> encoder, id<MTLTexture> texture,
                              id<MTLBuffer> largeA, id<MTLBuffer> largeB,
                              BOOL reference) {
    NSMutableArray<id<MTLSamplerState>> *decoys = [NSMutableArray array];
    id<MTLBuffer> scratch = newSharedBuffer(device,
        MAX((NSUInteger)16, encoder.encodedLength), 0xd4, @"decoy scratch");
    [encoder setArgumentBuffer:scratch offset:0];
    for (NSUInteger i = 0; i < kDecoyCount; ++i) {
        id<MTLSamplerState> decoy = newSampler(device, i);
        [decoys addObject:decoy];
        [encoder setSamplerState:decoy atIndex:1];
    }
    [encoder setSamplerState:nil atIndex:1];
    require(decoys.count == kDecoyCount, @"decoy samplers were released");

    require(!(kArgumentOffset % encoder.alignment), @"argument offset misaligned");
    id<MTLBuffer> arguments = newSharedBuffer(device,
        kArgumentOffset + encoder.encodedLength + kTailGuard, kArgumentGuard,
        @"sampling arguments");
    id<MTLBuffer> output = newSharedBuffer(device, kOutputLength, kOutputGuard,
        @"sampling output");
    id<MTLSamplerState> usedSampler = newSampler(device, 200);
    [encoder setArgumentBuffer:arguments offset:kArgumentOffset];
    [encoder setTexture:texture atIndex:0];
    [encoder setSamplerState:usedSampler atIndex:1];
    requireGuard(arguments, kArgumentOffset, encoder.encodedLength, kArgumentGuard,
        @"sampling arguments after encode");

    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLComputeCommandEncoder> compute = [command computeCommandEncoder];
    require(command != nil && compute != nil, @"sampling encoder unavailable");
    [compute setComputePipelineState:samplingPipeline];
    [compute setBuffer:arguments offset:kArgumentOffset atIndex:1];
    [compute setBuffer:output offset:kOutputOffset atIndex:0];
    [compute useResource:texture usage:MTLResourceUsageRead];
    [compute useResource:largeA usage:MTLResourceUsageRead];
    [compute useResource:largeB usage:MTLResourceUsageRead];
    [compute dispatchThreadgroups:MTLSizeMake(1, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [compute endEncoding];
    [command commit];
    [command waitUntilCompleted];
    require(command.status == MTLCommandBufferStatusCompleted && !command.error,
        command.error.description ?: @"sampling dispatch failed");

    requireGuard(arguments, kArgumentOffset, encoder.encodedLength, kArgumentGuard,
        @"sampling arguments after GPU");
    requireGuard(output, kOutputOffset, sizeof(float), kOutputGuard, @"sampling output");
    float actual = 0.0f;
    memcpy(&actual, (const uint8_t *)output.contents + kOutputOffset, sizeof actual);
    require(fabsf(actual - 7.0f) <= 1.0e-6f,
        [NSString stringWithFormat:@"sampling output=%g expected=7", actual]);
    requireAllZero(largeA, @"large scan buffer A");
    requireAllZero(largeB, @"large scan buffer B");
    printf("ARGUMENT_SAMPLER_RETIRE_SCAN_PASS backend=%s active=%lu used=1 "
           "bytes=%lu,%lu output=%.1f guards=1\n",
        reference ? "reference" : "bundle", (unsigned long)kDecoyCount,
        (unsigned long)largeA.length, (unsigned long)largeB.length, actual);
}

static void runProducerRound(id<MTLDevice> device, id<MTLCommandQueue> queue,
                             id<MTLComputePipelineState> producer,
                             id<MTLFunction> producerFunction, id<MTLTexture> texture,
                             id<MTLBuffer> destination, NSUInteger round, BOOL ownerAssertions,
                             NSMutableArray<id<MTLSamplerState>> *referenceKeepAlive) {
    __weak id<MTLSamplerState> retiredSampler = nil;
    __weak id<MTLBuffer> retiredSource = nil;
    @autoreleasepool {
        id<MTLArgumentEncoder> encoder = [producerFunction newArgumentEncoderWithBufferIndex:1];
        require(encoder != nil && encoder.encodedLength > 0 && encoder.alignment > 0,
            @"producer argument encoder unavailable");
        require(!(kArgumentOffset % encoder.alignment), @"producer argument offset misaligned");
        id<MTLSamplerState> sampler = newSampler(device, 200 + round);
        retiredSampler = sampler;
        id<MTLBuffer> source = newSharedBuffer(device,
            kArgumentOffset + encoder.encodedLength + kTailGuard, kArgumentGuard,
            @"producer source");
        retiredSource = source;
        [encoder setArgumentBuffer:source offset:kArgumentOffset];
        [encoder setTexture:texture atIndex:0];
        [encoder setSamplerState:sampler atIndex:1];
        requireGuard(source, kArgumentOffset, encoder.encodedLength, kArgumentGuard,
            @"producer source after encode");

        id<MTLCommandBuffer> command = [queue commandBuffer];
        id<MTLComputeCommandEncoder> compute = [command computeCommandEncoder];
        require(command != nil && compute != nil, @"producer encoder unavailable");
        [compute setComputePipelineState:producer];
        [compute setBuffer:destination offset:0 atIndex:0];
        [compute setBuffer:source offset:kArgumentOffset atIndex:1];
        [compute useResource:source usage:MTLResourceUsageRead];
        [compute useResource:texture usage:MTLResourceUsageRead];
        [compute dispatchThreadgroups:MTLSizeMake(1, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        [compute endEncoding];

        if (ownerAssertions) sampler = nil;
        else [referenceKeepAlive addObject:sampler];
        [command commit];
        [command waitUntilCompleted];
        require(command.status == MTLCommandBufferStatusCompleted && !command.error,
            command.error.description ?: [NSString stringWithFormat:@"producer round %lu failed",
                (unsigned long)round]);
        requireGuard(source, kArgumentOffset, encoder.encodedLength, kArgumentGuard,
            @"producer source after GPU");
        requireZeroWordAndGuard(destination,
            [NSString stringWithFormat:@"producer round %lu destination", (unsigned long)round]);

        compute = nil;
        command = nil;
        source = nil;
        encoder = nil;
    }
    if (ownerAssertions)
        for (NSUInteger attempt = 0; attempt < 1000 && (retiredSource || retiredSampler); ++attempt)
            usleep(1000);
    BOOL sourceRetired = retiredSource == nil;
    BOOL retired = retiredSampler == nil;
    if (ownerAssertions) {
        require(sourceRetired, [NSString stringWithFormat:
            @"round %lu source/command was retained after producer completion", (unsigned long)round]);
        require(retired, [NSString stringWithFormat:
            @"round %lu sampler was retained after producer completion", (unsigned long)round]);
    }
    printf("ARGUMENT_SAMPLER_RETIRE_ROUND_PASS round=%lu weak_retired=%d backend=%s\n",
        (unsigned long)round, retired, ownerAssertions ? "bundle" : "reference");
}

static void runAbandonedFillCase(id<MTLDevice> device, id<MTLCommandQueue> queue,
                                 id<MTLFunction> samplingFunction,
                                 id<MTLComputePipelineState> samplingPipeline,
                                 id<MTLTexture> texture, BOOL ownerAssertions,
                                 NSMutableArray<id<MTLSamplerState>> *referenceKeepAlive) {
    id<MTLArgumentEncoder> encoder = newFunctionEncoder(samplingFunction);
    NSUInteger argumentLength = encoder.encodedLength;
    id<MTLBuffer> destination = newSharedBuffer(device, argumentLength + kTailGuard,
        0x5c, @"abandoned-fill destination");
    __weak id<MTLSamplerState> retiredSampler = nil;
    __weak id<MTLCommandBuffer> abandonedCommand = nil;
    @autoreleasepool {
        id<MTLSamplerState> sampler = newSampler(device, 300);
        retiredSampler = sampler;
        encodeSamplerArguments(encoder, texture, sampler, destination, 0);
        if (ownerAssertions) sampler = nil;
        else [referenceKeepAlive addObject:sampler];

        @autoreleasepool {
            id<MTLCommandBuffer> command = [queue commandBuffer];
            abandonedCommand = command;
            id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
            require(command != nil && blit != nil, @"abandoned-fill encoder unavailable");
            [blit fillBuffer:destination range:NSMakeRange(0, argumentLength) value:0x7a];
            [blit endEncoding];
            blit = nil;
            command = nil;
        }
        [encoder setArgumentBuffer:nil offset:0];
        encoder = nil;
    }
    for (NSUInteger attempt = 0; attempt < 1000 && abandonedCommand; ++attempt)
        usleep(1000);
    require(abandonedCommand == nil, @"abandoned fill command was retained");
    require(retiredSampler != nil, @"abandoned encoded fill released the sampler owner");
    float actual = runSamplingProbe(device, queue, samplingPipeline, texture, destination, 0,
        @"abandoned-fill sampling output");
    printf("ARGUMENT_SAMPLER_RETIRE_ABANDONED_PASS abandoned=1 owner_alive=1 "
           "sampler_output=%.1f backend=%s\n", actual,
        ownerAssertions ? "bundle" : "reference");
}

static void runPendingWriterCase(id<MTLDevice> device, id<MTLCommandQueue> priorQueue,
                                 id<MTLCommandQueue> copyQueue,
                                 id<MTLFunction> samplingFunction,
                                 id<MTLComputePipelineState> samplingPipeline,
                                 id<MTLTexture> texture, BOOL ownerAssertions,
                                 NSMutableArray<id<MTLSamplerState>> *referenceKeepAlive) {
    id<MTLArgumentEncoder> sizingEncoder = newFunctionEncoder(samplingFunction);
    NSUInteger argumentLength = sizingEncoder.encodedLength;
    sizingEncoder = nil;
    id<MTLBuffer> destination = newSharedBuffer(device, argumentLength + kTailGuard,
        0x5d, @"pending-writer destination");
    id<MTLSharedEvent> gate = [device newSharedEvent];
    require(gate != nil, @"pending-writer shared event unavailable");
    gate.signaledValue = 0;

    __weak id<MTLSamplerState> retiredSampler = nil;
    __weak id<MTLBuffer> retiredSource = nil;
    @autoreleasepool {
        id<MTLSamplerState> sampler = newSampler(device, 301);
        retiredSampler = sampler;
        id<MTLBuffer> source = rawSamplerArguments(device, samplingFunction, texture, sampler,
            @"pending-writer raw argument source");
        retiredSource = source;

        id<MTLCommandBuffer> copyCommand = [copyQueue commandBuffer];
        require(copyCommand != nil, @"pending-writer copy command unavailable");
        [(id)copyCommand encodeWaitForEvent:gate value:1];
        id<MTLBlitCommandEncoder> copy = [copyCommand blitCommandEncoder];
        require(copy != nil, @"pending-writer copy encoder unavailable");
        [copy copyFromBuffer:source sourceOffset:0 toBuffer:destination destinationOffset:0
            size:argumentLength];
        [copy endEncoding];
        [copyCommand commit];

        // Release the only owning reference to the sampler and the raw source before
        // the copy is unblocked. The destination's conservative union must keep it alive.
        if (ownerAssertions) sampler = nil;
        else [referenceKeepAlive addObject:sampler];
        source = nil;

        id<MTLCommandBuffer> priorCommand = [priorQueue commandBuffer];
        require(priorCommand != nil, @"pending-writer prior command unavailable");
        id<MTLBlitCommandEncoder> prior = [priorCommand blitCommandEncoder];
        require(prior != nil, @"pending-writer prior encoder unavailable");
        [prior fillBuffer:destination range:NSMakeRange(0, argumentLength) value:0x6a];
        [prior endEncoding];
        [priorCommand commit];
        [priorCommand waitUntilCompleted];
        require(priorCommand.status == MTLCommandBufferStatusCompleted && !priorCommand.error,
            priorCommand.error.description ?: @"pending-writer prior command failed");
        require(copyCommand.status != MTLCommandBufferStatusCompleted,
            @"pending-writer copy completed before its gate");
        usleep(10000);
        require(retiredSampler != nil,
            @"pending writer lost the destination sampler before the copy ran");

        gate.signaledValue = 1;
        [copyCommand waitUntilCompleted];
        require(copyCommand.status == MTLCommandBufferStatusCompleted && !copyCommand.error,
            copyCommand.error.description ?: @"pending-writer copy failed");
        copyCommand = nil;
        source = nil;
    }
    for (NSUInteger attempt = 0; attempt < 1000 && retiredSource; ++attempt)
        usleep(1000);
    require(retiredSource == nil, @"pending-writer raw source was retained after completion");
    require(retiredSampler != nil,
        @"destination lost the GPU-written sampler after source retirement");
    float actual = runSamplingProbe(device, priorQueue, samplingPipeline, texture, destination, 0,
        @"pending-writer sampling output");
    printf("ARGUMENT_SAMPLER_RETIRE_PENDING_PASS prior_completed=1 copy_pending=1 "
           "source_retired=1 sampler_alive=1 sampler_output=%.1f backend=%s\n", actual,
        ownerAssertions ? "bundle" : "reference");
}

static void runOverlappingWriterCase(id<MTLDevice> device, id<MTLCommandQueue> queue,
                                     id<MTLCommandQueue> copyQueue,
                                     id<MTLFunction> samplingFunction,
                                     id<MTLComputePipelineState> samplingPipeline,
                                     id<MTLTexture> texture, BOOL ownerAssertions,
                                     NSMutableArray<id<MTLSamplerState>> *referenceKeepAlive) {
    id<MTLArgumentEncoder> sizingEncoder = newFunctionEncoder(samplingFunction);
    NSUInteger argumentLength = sizingEncoder.encodedLength;
    sizingEncoder = nil;
    id<MTLBuffer> destination = newSharedBuffer(device, argumentLength + kTailGuard,
        0x5e, @"overlapping-writer destination");
    id<MTLSharedEvent> gate = [device newSharedEvent];
    require(gate != nil, @"overlapping-writer shared event unavailable");
    gate.signaledValue = 0;

    __weak id<MTLSamplerState> firstSampler = nil;
    __weak id<MTLSamplerState> secondSampler = nil;
    @autoreleasepool {
        id<MTLSamplerState> first = newSampler(device, 310);
        id<MTLSamplerState> second = newSampler(device, 311);
        firstSampler = first;
        secondSampler = second;
        id<MTLBuffer> firstSource = rawSamplerArguments(device, samplingFunction, texture, first,
            @"overlapping-writer first source");
        id<MTLBuffer> secondSource = rawSamplerArguments(device, samplingFunction, texture, second,
            @"overlapping-writer second source");

        id<MTLCommandBuffer> secondCommand = [copyQueue commandBuffer];
        require(secondCommand != nil, @"overlapping-writer second command unavailable");
        [(id)secondCommand encodeWaitForEvent:gate value:1];
        id<MTLBlitCommandEncoder> secondCopy = [secondCommand blitCommandEncoder];
        require(secondCopy != nil, @"overlapping-writer second encoder unavailable");
        [secondCopy copyFromBuffer:secondSource sourceOffset:0 toBuffer:destination
            destinationOffset:0 size:argumentLength];
        [secondCopy endEncoding];
        [secondCommand commit];

        if (ownerAssertions) {
            first = nil;
            second = nil;
        } else {
            [referenceKeepAlive addObject:first];
            [referenceKeepAlive addObject:second];
        }

        id<MTLCommandBuffer> firstCommand = [queue commandBuffer];
        require(firstCommand != nil, @"overlapping-writer first command unavailable");
        id<MTLBlitCommandEncoder> firstCopy = [firstCommand blitCommandEncoder];
        require(firstCopy != nil, @"overlapping-writer first encoder unavailable");
        [firstCopy copyFromBuffer:firstSource sourceOffset:0 toBuffer:destination
            destinationOffset:0 size:argumentLength];
        [firstCopy endEncoding];
        [firstCommand commit];
        [firstCommand waitUntilCompleted];
        require(firstCommand.status == MTLCommandBufferStatusCompleted && !firstCommand.error,
            firstCommand.error.description ?: @"overlapping-writer first copy failed");
        require(secondCommand.status != MTLCommandBufferStatusCompleted,
            @"overlapping-writer second copy completed before its gate");
        usleep(10000);
        require(firstSampler != nil && secondSampler != nil,
            @"overlapping writer retired a sampler before the last writer completed");

        gate.signaledValue = 1;
        [secondCommand waitUntilCompleted];
        require(secondCommand.status == MTLCommandBufferStatusCompleted && !secondCommand.error,
            secondCommand.error.description ?: @"overlapping-writer second copy failed");
        firstCommand = nil;
        secondCommand = nil;
        firstSource = nil;
        secondSource = nil;
    }
    require(secondSampler != nil,
        @"last overlapping writer's sampler was retired after both copies completed");
    float actual = runSamplingProbe(device, queue, samplingPipeline, texture, destination, 0,
        @"overlapping-writer sampling output");
    printf("ARGUMENT_SAMPLER_RETIRE_OVERLAP_PASS first_completed=1 second_pending=1 "
           "first_handoff_alive=1 final_owner_alive=1 sampler_output=%.1f backend=%s\n",
        actual, ownerAssertions ? "bundle" : "reference");
}

static void runLateSetterCase(id<MTLDevice> device, id<MTLCommandQueue> queue,
                              id<MTLCommandQueue> copyQueue,
                              id<MTLFunction> samplingFunction,
                              id<MTLComputePipelineState> samplingPipeline,
                              id<MTLTexture> texture, BOOL ownerAssertions,
                              NSMutableArray<id<MTLSamplerState>> *referenceKeepAlive) {
    id<MTLArgumentEncoder> encoder = newFunctionEncoder(samplingFunction);
    NSUInteger argumentLength = encoder.encodedLength;
    id<MTLBuffer> destination = newSharedBuffer(device, argumentLength + kTailGuard,
        0x5f, @"late-setter destination");
    id<MTLSharedEvent> gate = [device newSharedEvent];
    require(gate != nil, @"late-setter shared event unavailable");
    gate.signaledValue = 0;

    __weak id<MTLSamplerState> copySampler = nil;
    __weak id<MTLSamplerState> setterSampler = nil;
    @autoreleasepool {
        id<MTLSamplerState> sourceSampler = newSampler(device, 320);
        id<MTLSamplerState> lateSampler = newSampler(device, 321);
        copySampler = sourceSampler;
        setterSampler = lateSampler;
        id<MTLBuffer> source = rawSamplerArguments(device, samplingFunction, texture, sourceSampler,
            @"late-setter raw source");

        [encoder setArgumentBuffer:destination offset:0];
        [encoder setSamplerState:lateSampler atIndex:1];
        [encoder setArgumentBuffer:nil offset:0];

        id<MTLCommandBuffer> copyCommand = [copyQueue commandBuffer];
        require(copyCommand != nil, @"late-setter copy command unavailable");
        [(id)copyCommand encodeWaitForEvent:gate value:1];
        id<MTLBlitCommandEncoder> copy = [copyCommand blitCommandEncoder];
        require(copy != nil, @"late-setter copy encoder unavailable");
        [copy copyFromBuffer:source sourceOffset:0 toBuffer:destination destinationOffset:0
            size:argumentLength];
        [copy endEncoding];
        [copyCommand commit];

        [encoder setArgumentBuffer:destination offset:0];
        [encoder setTexture:texture atIndex:0];
        [encoder setSamplerState:lateSampler atIndex:1];
        [encoder setArgumentBuffer:nil offset:0];
        if (ownerAssertions) {
            sourceSampler = nil;
            lateSampler = nil;
        } else {
            [referenceKeepAlive addObject:sourceSampler];
            [referenceKeepAlive addObject:lateSampler];
        }
        source = nil;

        gate.signaledValue = 1;
        [copyCommand waitUntilCompleted];
        require(copyCommand.status == MTLCommandBufferStatusCompleted && !copyCommand.error,
            copyCommand.error.description ?: @"late-setter copy failed");
        copyCommand = nil;
    }
    if (ownerAssertions) {
        require(copySampler != nil,
            @"late setter caused the overlapping GPU writer's sampler owner to be lost");
        require(setterSampler != nil,
            @"earlier GPU completion cleared the later CPU setter's sampler owner");
    }
    float actual = runSamplingProbe(device, queue, samplingPipeline, texture, destination, 0,
        @"late-setter sampling output");
    printf("ARGUMENT_SAMPLER_RETIRE_LATE_SETTER_PASS copy_pending=1 "
           "setter_order=host-after-copy gpu_owner_alive=%d setter_owner_alive=%d "
           "sampler_output=%.1f backend=%s\n",
        copySampler != nil, setterSampler != nil, actual,
        ownerAssertions ? "bundle" : "reference");
}

static void runAbandonedConcurrentCopyCase(id<MTLDevice> device, id<MTLCommandQueue> queue,
                                           id<MTLCommandQueue> copyQueue,
                                           id<MTLFunction> samplingFunction,
                                           id<MTLComputePipelineState> samplingPipeline,
                                           id<MTLTexture> texture, BOOL ownerAssertions,
                                           NSMutableArray<id<MTLSamplerState>> *referenceKeepAlive) {
    id<MTLArgumentEncoder> encoder = newFunctionEncoder(samplingFunction);
    NSUInteger argumentLength = encoder.encodedLength;
    id<MTLBuffer> destination = newSharedBuffer(device, argumentLength + kTailGuard,
        0x60, @"abandoned-concurrent destination");
    id<MTLSharedEvent> gate = [device newSharedEvent];
    require(gate != nil, @"abandoned-concurrent shared event unavailable");
    gate.signaledValue = 0;

    __weak id<MTLSamplerState> priorSampler = nil;
    __weak id<MTLSamplerState> pendingSampler = nil;
    __weak id<MTLSamplerState> abandonedSampler = nil;
    __weak id<MTLBuffer> abandonedSource = nil;
    __weak id<MTLBlitCommandEncoder> abandonedEncoder = nil;
    __weak id<MTLCommandBuffer> abandonedCommandBuffer = nil;
    BOOL neighborOwnersAlive = NO;
    id<MTLCommandBuffer> pendingCommand = nil;
    id<MTLBuffer> pendingSource = nil;

    // Keep the gated peer alive deliberately; every other wrapper is scoped so its
    // autorelease cannot masquerade as a production ownership leak.
    @autoreleasepool {
        // These LOD/address configurations are distinct from every other fixture tag.
        id<MTLSamplerState> prior = newSampler(device, 400);
        id<MTLSamplerState> pending = newSampler(device, 401);
        priorSampler = prior;
        pendingSampler = pending;

        encodeSamplerArguments(encoder, texture, prior, destination, 0);
        [encoder setArgumentBuffer:nil offset:0];
        encoder = nil;

        pendingSource = rawSamplerArguments(device, samplingFunction,
            texture, pending, @"abandoned-concurrent pending source");

        pendingCommand = [copyQueue commandBuffer];
        require(pendingCommand != nil,
            @"abandoned-concurrent pending command unavailable");
        [(id)pendingCommand encodeWaitForEvent:gate value:1];
        id<MTLBlitCommandEncoder> pendingCopy = [pendingCommand blitCommandEncoder];
        require(pendingCopy != nil, @"abandoned-concurrent pending encoder unavailable");
        [pendingCopy copyFromBuffer:pendingSource sourceOffset:0 toBuffer:destination
            destinationOffset:0 size:argumentLength];
        [pendingCopy endEncoding];
        [pendingCommand commit];

        if (ownerAssertions) {
            prior = nil;
            pending = nil;
        } else {
            [referenceKeepAlive addObject:prior];
            [referenceKeepAlive addObject:pending];
            prior = nil;
            pending = nil;
        }
    }

    // Drop every abandoned wrapper in its own pool. From here on, a non-nil weak
    // object is a backend/owner retention result rather than fixture autorelease.
    @autoreleasepool {
        id<MTLSamplerState> abandoned = newSampler(device, 402);
        abandonedSampler = abandoned;
        id<MTLBuffer> abandonedRaw = rawSamplerArguments(device, samplingFunction,
            texture, abandoned, @"abandoned-concurrent abandoned source");
        abandonedSource = abandonedRaw;
        id<MTLCommandBuffer> abandonedCommand = [queue commandBuffer];
        require(abandonedCommand != nil,
            @"abandoned-concurrent abandoned command unavailable");
        abandonedCommandBuffer = abandonedCommand;
        id<MTLBlitCommandEncoder> abandonedCopy =
            [abandonedCommand blitCommandEncoder];
        require(abandonedCopy != nil,
            @"abandoned-concurrent abandoned encoder unavailable");
        abandonedEncoder = abandonedCopy;
        [abandonedCopy copyFromBuffer:abandonedRaw sourceOffset:0 toBuffer:destination
            destinationOffset:0 size:argumentLength];
        [abandonedCopy endEncoding];
        if (!ownerAssertions) [referenceKeepAlive addObject:abandoned];
    }

    if (ownerAssertions) {
        for (NSUInteger attempt = 0; attempt < 1000 &&
             (abandonedCommandBuffer || abandonedSource || abandonedEncoder ||
              abandonedSampler); ++attempt)
            usleep(1000);
        require(abandonedCommandBuffer == nil && abandonedSource == nil &&
            abandonedEncoder == nil && abandonedSampler == nil,
            [NSString stringWithFormat:@"abandoned concurrent copy leaked command=%d "
                "source=%d encoder=%d sampler=%d", abandonedCommandBuffer != nil,
                abandonedSource != nil, abandonedEncoder != nil,
                abandonedSampler != nil]);
        neighborOwnersAlive = priorSampler != nil && pendingSampler != nil;
        require(neighborOwnersAlive,
            @"abandoned intent retired the prior owner or pending copy's sampler");
    } else {
        neighborOwnersAlive = priorSampler != nil && pendingSampler != nil;
    }

    require(pendingCommand.status != MTLCommandBufferStatusCompleted,
        @"abandoned-concurrent pending copy completed before its gate");
    usleep(10000);
    require(pendingCommand.status != MTLCommandBufferStatusCompleted,
        @"abandoned-concurrent pending copy bypassed its event");
    requireGuard(pendingSource, 0, argumentLength, kArgumentGuard,
        @"abandoned-concurrent pending source");

    gate.signaledValue = 1;
    [pendingCommand waitUntilCompleted];
    require(pendingCommand.status == MTLCommandBufferStatusCompleted && !pendingCommand.error,
        pendingCommand.error.description ?: @"abandoned-concurrent pending copy failed");
    requireGuard(destination, 0, argumentLength, 0x60,
        @"abandoned-concurrent destination");
    requireGuard(pendingSource, 0, argumentLength, kArgumentGuard,
        @"abandoned-concurrent pending source after completion");
    @autoreleasepool {
        pendingCommand = nil;
        pendingSource = nil;
    }

    if (ownerAssertions) {
        for (NSUInteger attempt = 0; attempt < 1000 &&
             (priorSampler != nil || pendingSampler == nil); ++attempt)
            usleep(1000);
        require(priorSampler == nil,
            @"done pending copy did not retire the replaced prior sampler owner");
        require(pendingSampler != nil,
            @"done pending copy did not publish the copied sampler owner");
        require(abandonedSampler == nil,
            @"abandoned concurrent sampler remained after pending completion");
    }
    float actual = runSamplingProbe(device, queue, samplingPipeline, texture, destination, 0,
        @"abandoned-concurrent sampling output");
    printf("ARGUMENT_SAMPLER_RETIRE_ABANDONED_COPY_PASS neighbor_owners_alive=%d "
           "prior_owner_retired=%d pending_owner_alive=%d abandoned_command_retired=%d "
           "abandoned_source_retired=%d abandoned_encoder_retired=%d "
           "abandoned_sampler_retired=%d "
           "owner_handoff=%s sampler_output=%.1f weak_checks=%s backend=%s\n",
        neighborOwnersAlive, priorSampler == nil, pendingSampler != nil,
        abandonedCommandBuffer == nil, abandonedSource == nil, abandonedEncoder == nil,
        abandonedSampler == nil,
        ownerAssertions ? "asserted" : "functional-only", actual,
        ownerAssertions ? "asserted" : "skipped",
        ownerAssertions ? "bundle" : "reference");
}

static void runFinalConsumer(id<MTLDevice> device, id<MTLCommandQueue> queue,
                             id<MTLComputePipelineState> consumer,
                             id<MTLBuffer> source) {
    id<MTLBuffer> output = newSharedBuffer(device, kOutputLength, kOutputGuard,
        @"retirement consumer output");
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLComputeCommandEncoder> compute = [command computeCommandEncoder];
    require(command != nil && compute != nil, @"retirement consumer encoder unavailable");
    [compute setComputePipelineState:consumer];
    [compute setBuffer:source offset:0 atIndex:1];
    [compute setBuffer:output offset:kOutputOffset atIndex:0];
    [compute dispatchThreadgroups:MTLSizeMake(1, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [compute endEncoding];
    [command commit];
    [command waitUntilCompleted];
    require(command.status == MTLCommandBufferStatusCompleted && !command.error,
        command.error.description ?: @"retirement consumer failed");
    requireGuard(output, kOutputOffset, sizeof(uint32_t), kOutputGuard,
        @"retirement consumer output");
    uint32_t actual = UINT32_MAX;
    memcpy(&actual, (const uint8_t *)output.contents + kOutputOffset, sizeof actual);
    require(actual == 0, [NSString stringWithFormat:@"retirement consumer output=%u expected=0",
        actual]);
    requireZeroWordAndGuard(source, @"retirement consumer source");
}

int main(int argc, const char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        puts("Usage: test-argument-sampler-retire BUNDLE|--reference OUTPUT_DIR\n"
             "       test-argument-sampler-retire BUNDLE|--reference OUTPUT_DIR "
             "--only writer-epochs");
        return 0;
    }
    BOOL writerEpochsOnly = argc == 5 && !strcmp(argv[3], "--only") &&
        !strcmp(argv[4], "writer-epochs");
    if (argc != 3 && !writerEpochsOnly) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(110);
    @autoreleasepool {
        NSError *error = nil;
        BOOL reference = !strcmp(argv[1], "--reference");
        BOOL ownerAssertions = !reference;
        NSString *directory = @(argv[2]);
        require([[NSFileManager defaultManager] createDirectoryAtPath:directory
            withIntermediateDirectories:YES attributes:nil error:&error],
            error.description ?: @"output directory unavailable");

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
            "kernel void sample_probe(constant Args &args [[buffer(1)]], device float *out [[buffer(0)]]) {"
            " out[0] = args.image.sample(args.filter, float2(0.5f, 0.5f)).x; }\n"
            "kernel void producer_zero(constant Args &args [[buffer(1)]], device uint *out [[buffer(0)]]) {"
            " float value = args.image.sample(args.filter, float2(0.5f, 0.5f)).x;"
            " out[0] = uint(value) == 0xffffffffu ? 1u : 0u; }\n"
            "kernel void consume_zero(device const uint *src [[buffer(1)]], device uint *out [[buffer(0)]]) {"
            " out[0] = src[0]; }\n";
        require([source writeToFile:[directory stringByAppendingPathComponent:
            @"argument-sampler-retire.metal"] atomically:YES encoding:NSUTF8StringEncoding
            error:&error], @"source write failed");
        MTLCompileOptions *options = [MTLCompileOptions new];
        options.languageVersion = MTLLanguageVersion2_1;
        id<MTLLibrary> library = [device newLibraryWithSource:source options:options error:&error];
        require(library != nil, error.description ?: @"source compilation failed");

        id<MTLFunction> samplingFunction = [library newFunctionWithName:@"sample_probe"];
        id<MTLFunction> producerFunction = [library newFunctionWithName:@"producer_zero"];
        id<MTLFunction> consumerFunction = [library newFunctionWithName:@"consume_zero"];
        require(samplingFunction && producerFunction && consumerFunction,
            @"shader functions unavailable");
        id<MTLComputePipelineState> samplingPipeline =
            [device newComputePipelineStateWithFunction:samplingFunction error:&error];
        require(samplingPipeline != nil, error.description ?: @"sampling pipeline failed");
        error = nil;
        id<MTLComputePipelineState> producerPipeline =
            [device newComputePipelineStateWithFunction:producerFunction error:&error];
        require(producerPipeline != nil, error.description ?: @"producer pipeline failed");
        error = nil;
        id<MTLComputePipelineState> consumerPipeline =
            [device newComputePipelineStateWithFunction:consumerFunction error:&error];
        require(consumerPipeline != nil, error.description ?: @"consumer pipeline failed");
        id<MTLArgumentEncoder> encoder = newEncoder(library, @"sample_probe");
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLCommandQueue> queue2 = [device newCommandQueue];
        require(queue != nil && queue2 != nil, @"command queues unavailable");
        id<MTLTexture> texture = newSampleTexture(device);

        id<MTLBuffer> largeA = nil;
        id<MTLBuffer> largeB = nil;
        if (!writerEpochsOnly) {
            NSUInteger largeBytes = 32u << 20;
            largeA = newSharedBuffer(device, largeBytes, 0, @"large scan buffer A");
            largeB = newSharedBuffer(device, largeBytes + 1, 0, @"large scan buffer B");
            printf("ARGUMENT_SAMPLER_RETIRE_LAYOUT decoys=%lu max_argument_samplers=%lu "
                   "scan_bytes=%lu+%lu\n", (unsigned long)kDecoyCount,
                (unsigned long)device.maxArgumentBufferSamplerCount,
                (unsigned long)largeA.length, (unsigned long)largeB.length);
            runScanBudgetCase(device, queue, samplingPipeline, encoder, texture, largeA, largeB,
                reference);
        }

        id<MTLBuffer> candidate = newSharedBuffer(device, kCandidateLength, kOutputGuard,
            @"candidate destination");
        __attribute__((objc_precise_lifetime))
        NSMutableArray<id<MTLSamplerState>> *referenceKeepAlive = [NSMutableArray array];
        const NSUInteger rounds = 4;
        for (NSUInteger round = 1; round <= rounds; ++round)
            runProducerRound(device, queue, producerPipeline, producerFunction,
                texture, candidate, round, ownerAssertions, referenceKeepAlive);
        requireZeroWordAndGuard(candidate, @"final candidate destination");
        runFinalConsumer(device, queue, consumerPipeline, candidate);
        runAbandonedFillCase(device, queue, samplingFunction, samplingPipeline, texture,
            ownerAssertions, referenceKeepAlive);
        runPendingWriterCase(device, queue, queue2, samplingFunction, samplingPipeline, texture,
            ownerAssertions, referenceKeepAlive);
        runOverlappingWriterCase(device, queue, queue2, samplingFunction, samplingPipeline,
            texture, ownerAssertions, referenceKeepAlive);
        runLateSetterCase(device, queue, queue2, samplingFunction, samplingPipeline,
            texture, ownerAssertions, referenceKeepAlive);
        if (writerEpochsOnly)
            runAbandonedConcurrentCopyCase(device, queue, queue2, samplingFunction,
                samplingPipeline, texture, ownerAssertions, referenceKeepAlive);

        if (writerEpochsOnly) {
            printf("ARGUMENT_SAMPLER_WRITER_EPOCHS_PASS scan_budget=not_run "
                   "candidate_rounds=%lu candidate_tokens=0 weak_retire=%s "
                   "abandoned_fill=1 pending_union=1 overlapping_writers=1 late_setter=1 "
                   "abandoned_concurrent_copy=1 abandoned_own_intent=1 "
                   "pending_peer_preserved=1 abandoned_source_encoder_retired=1 "
                   "abandoned_owner_handoff=1 "
                   "reference_keepalive=%lu msl=2.1 backend=%s\n",
                (unsigned long)rounds,
                ownerAssertions ? "asserted" : "functional-only",
                (unsigned long)referenceKeepAlive.count,
                reference ? "reference" : "bundle");
        } else {
            printf("ARGUMENT_SAMPLER_RETIRE_PASS scan_decoys=%lu scan_bytes=%lu "
                   "candidate_rounds=%lu candidate_tokens=0 weak_retire=%s "
                   "abandoned_fill=1 pending_union=1 overlapping_writers=1 late_setter=1 "
                   "reference_keepalive=%lu msl=2.1\n",
                (unsigned long)kDecoyCount, (unsigned long)(largeA.length + largeB.length),
                (unsigned long)rounds, ownerAssertions ? "asserted" : "functional-only",
                (unsigned long)referenceKeepAlive.count);
        }
    }
    return 0;
}
