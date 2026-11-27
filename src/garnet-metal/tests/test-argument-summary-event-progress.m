/*
 * Runtime-summary GPU event-progress fixture.
 *
 * This fixture keeps the frozen Private-summary test unchanged and adds an
 * independent case:
 *   - one command buffer copies sampler tokens into Private storage,
 *   - its summary-bearing first segment completes and publishes owners,
 *   - the same command buffer then blocks on a later event wait,
 *   - the old Shared source and replaced sampler retire while that command
 *     buffer is still incomplete,
 *   - an independent queue produces the gate without being blocked by the
 *     summary work, and the blocked command buffer then advances.
 *
 * There is no Metal function, AIR, translator, or consumer shader. This is
 * runtime-summary SPIR-V GPU evidence, not an ABI8/translator acceptance test.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "garnet_argument_summary.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

@interface NSObject (RDNA4SummaryEventProgress)
- (void)encodeWaitForEvent:(id<MTLEvent>)event
                     value:(uint64_t)value
                   timeout:(uint32_t)timeout;
@end

static const NSUInteger kTypedOffset = 256;
static const NSUInteger kTailGuard = 64;
static const NSUInteger kRounds = 8;
static const uint8_t kPrivateSeed = 0x8d;
static const uint8_t kSharedGuard = 0xb4;
static const uint8_t kTailSeed = 0xa7;
static const uint8_t kTailGuardByte = 0x62;
static const uint8_t kFutureFill = 0x5c;
static const uint8_t kProducerSeed = 0x73;
static const uint8_t kProducerFill = 0xc6;
static const uint8_t kProducerGuard = 0x4e;
static const uint8_t kReadbackGuard = 0x6e;

static void require(BOOL ok, NSString *message) {
    if (!ok) {
        fprintf(stderr, "ARGUMENT_SUMMARY_EVENT_PROGRESS_FAIL %s\n",
                message.UTF8String);
        exit(1);
    }
}

static NSUInteger align_up(NSUInteger value, NSUInteger alignment) {
    require(alignment != 0, @"zero alignment");
    NSUInteger remainder = value % alignment;
    if (!remainder) return value;
    require(value <= NSUIntegerMax - (alignment - remainder),
            @"alignment overflow");
    return value + alignment - remainder;
}

static id<MTLBuffer> new_shared_buffer(id<MTLDevice> device,
                                       NSUInteger length, uint8_t fill,
                                       NSString *description) {
    id<MTLBuffer> buffer = [device newBufferWithLength:length
                                              options:MTLResourceStorageModeShared];
    require(buffer != nil && buffer.contents != NULL,
            [description stringByAppendingString:@" unavailable"]);
    memset(buffer.contents, fill, length);
    return buffer;
}

static id<MTLSamplerState> new_sampler(id<MTLDevice> device, NSUInteger tag) {
    MTLSamplerDescriptor *descriptor = [MTLSamplerDescriptor new];
    descriptor.supportArgumentBuffers = YES;
    descriptor.minFilter = MTLSamplerMinMagFilterNearest;
    descriptor.magFilter = MTLSamplerMinMagFilterNearest;
    descriptor.mipFilter = MTLSamplerMipFilterNotMipmapped;
    descriptor.sAddressMode = MTLSamplerAddressModeClampToEdge;
    descriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
    descriptor.rAddressMode = MTLSamplerAddressModeClampToEdge;
    descriptor.normalizedCoordinates = YES;
    descriptor.lodMinClamp = 0.0f;
    descriptor.lodMaxClamp = 1.0f + (float)(tag % 4096u) / 4096.0f;
    descriptor.borderColor = MTLSamplerBorderColorTransparentBlack;
    id<MTLSamplerState> sampler =
        [device newSamplerStateWithDescriptor:descriptor];
    require(sampler != nil,
            [NSString stringWithFormat:@"sampler tag %lu unavailable",
             (unsigned long)tag]);
    return sampler;
}

static id<MTLBuffer> encode_sampler_pair(
        id<MTLDevice> device, id<MTLArgumentEncoder> encoder,
        id<MTLSamplerState> first, id<MTLSamplerState> second,
        uint64_t *firstToken, uint64_t *secondToken,
        NSString *description) {
    require(first != nil && second != nil && firstToken != NULL &&
            secondToken != NULL && encoder.encodedLength >=
                2u * sizeof(uint64_t),
            @"sampler-pair inputs invalid");
    NSUInteger length = encoder.encodedLength + kTailGuard;
    id<MTLBuffer> source =
        new_shared_buffer(device, length, kSharedGuard, description);
    [encoder setArgumentBuffer:source offset:0];
    [encoder setSamplerState:first atIndex:0];
    [encoder setSamplerState:second atIndex:1];
    [encoder setArgumentBuffer:nil offset:0];
    const uint8_t *bytes = source.contents;
    require(bytes != NULL && encoder.encodedLength <= source.length,
            [description stringByAppendingString:@" range invalid"]);
    for (NSUInteger index = encoder.encodedLength; index < source.length;
         ++index)
        require(bytes[index] == kSharedGuard,
                [NSString stringWithFormat:
                 @"%@ changed the Shared guard at %lu", description,
                 (unsigned long)index]);
    memcpy(firstToken, bytes, sizeof(*firstToken));
    memcpy(secondToken, bytes + sizeof(uint64_t), sizeof(*secondToken));
    require((*firstToken >> 48u) ==
                RDNA4_ARGUMENT_SUMMARY_TOKEN_PREFIX &&
            (*secondToken >> 48u) ==
                RDNA4_ARGUMENT_SUMMARY_TOKEN_PREFIX,
            [description stringByAppendingString:
                @" did not emit sampler transport tokens"]);
    return source;
}

static void complete_command(id<MTLCommandBuffer> command,
                             NSString *description) {
    require(command != nil, [description stringByAppendingString:
        @" command unavailable"]);
    [command commit];
    [command waitUntilCompleted];
    require(command.status == MTLCommandBufferStatusCompleted &&
            command.error == nil,
            command.error.description ?:
                [description stringByAppendingString:@" failed"]);
}

static void require_pattern(id<MTLBuffer> buffer, NSUInteger offset,
                            NSUInteger length, uint8_t value,
                            NSString *description) {
    require(buffer.contents != NULL && offset <= buffer.length &&
            length <= buffer.length - offset,
            [description stringByAppendingString:@" range invalid"]);
    const uint8_t *bytes = buffer.contents;
    for (NSUInteger index = offset; index < offset + length; ++index)
        require(bytes[index] == value,
                [NSString stringWithFormat:
                 @"%@ byte %lu is 0x%02x, expected 0x%02x", description,
                 (unsigned long)index, bytes[index], value]);
}

static void check_private_readback(
        id<MTLDevice> device, id<MTLCommandQueue> queue,
        id<MTLBuffer> destination, NSUInteger privateLength,
        const uint8_t *expected, NSString *description) {
    require(destination.contents == NULL && expected != NULL,
            [description stringByAppendingString:
                @" Private access precondition failed"]);
    id<MTLBuffer> readback = new_shared_buffer(
        device, privateLength, kReadbackGuard,
        [description stringByAppendingString:@" readback"]);
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
    require(command != nil && blit != nil,
            [description stringByAppendingString:
                @" readback encoder unavailable"]);
    [blit copyFromBuffer:destination sourceOffset:0
                toBuffer:readback destinationOffset:0
                    size:privateLength];
    [blit endEncoding];
    complete_command(command, [description stringByAppendingString:
        @" readback"]);
    require(memcmp(readback.contents, expected, privateLength) == 0,
            [description stringByAppendingString:
                @" token bytes or guards differ"]);
    require(destination.contents == NULL,
            [description stringByAppendingString:
                @" made Private storage host-visible"]);
}

static void wait_event_value(id<MTLSharedEvent> event, uint64_t value,
                             NSString *description) {
    for (NSUInteger attempt = 0; attempt < 10000; ++attempt) {
        if (event.signaledValue >= value) return;
        usleep(1000);
    }
    require(NO, [NSString stringWithFormat:
        @"%@ did not reach event value %llu (observed %llu)", description,
        (unsigned long long)value,
        (unsigned long long)event.signaledValue]);
}

static void require_weak_nil(__weak id const *object, NSString *message) {
    for (NSUInteger attempt = 0; attempt < 2000; ++attempt) {
        BOOL retired;
        @autoreleasepool { retired = *object == nil; }
        if (retired) return;
        usleep(1000);
    }
    require(NO, message);
}

static void require_weak_alive(__weak id const *object, NSString *message) {
    BOOL alive;
    @autoreleasepool { alive = *object != nil; }
    require(alive, message);
}

int main(int argc, const char *argv[]) {
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        puts("Usage: test-argument-summary-event-progress --bundle BUNDLE\n"
             "Runs a same-command-buffer event-blocked future segment through "
             "the bundle's runtime summary SPIR-V.\n"
             "No Metal function, AIR, translator, or consumer shader is used; "
             "this is not an ABI8 acceptance test.");
        return 0;
    }
    const char *bundlePath = NULL;
    if (argc == 3 && !strcmp(argv[1], "--bundle"))
        bundlePath = argv[2];
    if (!bundlePath) return 2;

    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(300);
    @autoreleasepool {
        NSError *error = nil;
        NSBundle *bundle = [NSBundle bundleWithPath:@(bundlePath)];
        require([bundle loadAndReturnError:&error],
                error.description ?: @"bundle load failed");
        id<MTLDevice> device =
            [[[bundle classNamed:@"GarnetDevice"] alloc] init];
        id<MTLCommandQueue> writerQueue = [device newCommandQueue];
        id<MTLCommandQueue> producerQueue = [device newCommandQueue];
        require(device != nil && writerQueue != nil &&
                producerQueue != nil,
                @"bundle device or command queues unavailable");

        MTLArgumentDescriptor *samplers =
            [MTLArgumentDescriptor argumentDescriptor];
        samplers.dataType = MTLDataTypeSampler;
        samplers.index = 0;
        samplers.arrayLength = 2;
        samplers.access = MTLBindingAccessReadOnly;
        id<MTLArgumentEncoder> encoder =
            [device newArgumentEncoderWithArguments:@[samplers]];
        require(encoder != nil && encoder.encodedLength ==
                    2u * sizeof(uint64_t) && encoder.alignment != 0,
                @"synthetic sampler argument encoder unavailable");

        NSUInteger argumentLength = encoder.encodedLength;
        require(argumentLength <= NSUIntegerMax - kTypedOffset - kTailGuard,
                @"typed argument placement overflow");
        NSUInteger privateLength = align_up(
            kTypedOffset + argumentLength + kTailGuard, 4);
        id<MTLBuffer> destination =
            [device newBufferWithLength:privateLength
                               options:MTLResourceStorageModePrivate];
        require(destination != nil && destination.contents == NULL,
                @"Private destination is mapped or unavailable");

        id<MTLSharedEvent> summaryReady = [device newSharedEvent];
        id<MTLSharedEvent> futureGate = [device newSharedEvent];
        require(summaryReady != nil && futureGate != nil,
                @"shared events unavailable");

        __attribute__((objc_precise_lifetime))
        NSMutableArray<id<MTLCommandBuffer>> *commandHistory =
            [NSMutableArray array];
        __weak id<MTLSamplerState> currentOwner = nil;
        uint64_t currentTokens[2] = {0, 0};
        NSUInteger sourceReleasedBeforeGate = 0;
        NSUInteger writerIncompleteCount = 0;
        NSUInteger producerProgressCount = 0;
        NSUInteger ownerRetirements = 0;

        for (NSUInteger round = 0; round < kRounds; ++round) {
            @autoreleasepool {
                uint64_t readyValue = (uint64_t)round + 1u;
                uint64_t gateValue = (uint64_t)round + 1u;
                __weak id oldOwner = nil;
                @autoreleasepool { oldOwner = currentOwner; }
                if (round)
                    require_weak_alive(&oldOwner,
                        @"previous sampled owner was not retained before replacement");

                id<MTLCommandBuffer> writer = nil;
                id<MTLBuffer> futureTail = nil;
                __weak id<MTLSamplerState> replacement = nil;
                __weak id<MTLBuffer> source = nil;
                uint64_t tokens[2] = {0, 0};
                @autoreleasepool {
                    id<MTLSamplerState> nextSampler =
                        new_sampler(device, 5000u + round);
                    replacement = nextSampler;
                    id<MTLBuffer> nextSource = encode_sampler_pair(
                        device, encoder, nextSampler, nextSampler,
                        &tokens[0], &tokens[1],
                        [NSString stringWithFormat:
                            @"summary-event round %lu source",
                            (unsigned long)round]);
                    source = nextSource;
                    futureTail = new_shared_buffer(
                        device, 256, kTailGuardByte,
                        [NSString stringWithFormat:
                            @"summary-event round %lu future tail",
                            (unsigned long)round]);
                    memset((uint8_t *)futureTail.contents + 64,
                           kTailSeed, 128);

                    writer = [writerQueue commandBuffer];
                    id<MTLBlitCommandEncoder> blit =
                        [writer blitCommandEncoder];
                    require(writer != nil && blit != nil,
                        [NSString stringWithFormat:
                            @"summary-event round %lu writer unavailable",
                            (unsigned long)round]);
                    if (!round) {
                        [blit fillBuffer:destination
                                   range:NSMakeRange(0, privateLength)
                                   value:kPrivateSeed];
                    }
                    [blit copyFromBuffer:nextSource sourceOffset:0
                                toBuffer:destination
                       destinationOffset:kTypedOffset
                                    size:argumentLength];
                    [blit endEncoding];
                    [writer encodeSignalEvent:summaryReady value:readyValue];
                    [(id)writer encodeWaitForEvent:futureGate
                                              value:gateValue
                                            timeout:20000];
                    blit = [writer blitCommandEncoder];
                    require(blit != nil,
                        [NSString stringWithFormat:
                            @"summary-event round %lu future encoder unavailable",
                            (unsigned long)round]);
                    [blit fillBuffer:futureTail
                               range:NSMakeRange(64, 128)
                               value:kFutureFill];
                    [blit endEncoding];
                }

                require_weak_alive(&source,
                    @"encoded first segment did not retain its Shared source");
                require_weak_alive(&replacement,
                    @"encoded first segment did not retain its replacement sampler");
                [writer commit];
                [commandHistory addObject:writer];
                wait_event_value(summaryReady, readyValue,
                    [NSString stringWithFormat:
                        @"summary-event round %lu first segment",
                        (unsigned long)round]);

                require(writer.status != MTLCommandBufferStatusCompleted &&
                        writer.status != MTLCommandBufferStatusError,
                        @"same command buffer completed before its future gate");
                require_pattern(futureTail, 64, 128, kTailSeed,
                    @"future segment ran before its event gate");
                require_pattern(futureTail, 0, 64, kTailGuardByte,
                    @"future segment prefix guard");
                require_pattern(futureTail, 192, 64, kTailGuardByte,
                    @"future segment suffix guard");
                ++writerIncompleteCount;

                require_weak_alive(&replacement,
                    @"replacement sampler was not published by the first segment");
                require_weak_nil(&source,
                    @"Shared source survived its completed first segment while "
                    @"the command buffer remained event-blocked");
                ++sourceReleasedBeforeGate;
                if (round) {
                    require_weak_nil(&oldOwner,
                        @"summary did not retire the replaced sampler owner "
                        @"before the future gate");
                    ++ownerRetirements;
                }

                id<MTLBuffer> producer = new_shared_buffer(
                    device, 256, kProducerGuard,
                    [NSString stringWithFormat:
                        @"summary-event round %lu producer buffer",
                        (unsigned long)round]);
                memset((uint8_t *)producer.contents + 64,
                       kProducerSeed, 128);
                id<MTLCommandBuffer> producerCommand =
                    [producerQueue commandBuffer];
                require(producerCommand != nil,
                    @"independent producer command unavailable");
                id<MTLBlitCommandEncoder> producerBlit =
                    [producerCommand blitCommandEncoder];
                require(producerBlit != nil,
                    @"independent producer encoder unavailable");
                [producerBlit fillBuffer:producer
                                   range:NSMakeRange(64, 128)
                                   value:kProducerFill];
                [producerBlit endEncoding];
                [producerCommand encodeSignalEvent:futureGate
                                            value:gateValue];
                complete_command(producerCommand,
                    [NSString stringWithFormat:
                        @"summary-event round %lu independent producer",
                        (unsigned long)round]);
                [commandHistory addObject:producerCommand];
                ++producerProgressCount;
                require_pattern(producer, 64, 128, kProducerFill,
                    @"independent producer output");
                require_pattern(producer, 0, 64, kProducerGuard,
                    @"independent producer prefix guard");
                require_pattern(producer, 192, 64, kProducerGuard,
                    @"independent producer suffix guard");

                [writer waitUntilCompleted];
                require(writer.status == MTLCommandBufferStatusCompleted &&
                        writer.error == nil,
                        writer.error.description ?:
                            @"event-blocked command buffer failed after its gate");
                require_pattern(futureTail, 64, 128, kFutureFill,
                    @"same-command-buffer future segment did not advance");
                require_pattern(futureTail, 0, 64, kTailGuardByte,
                    @"completed future segment prefix guard");
                require_pattern(futureTail, 192, 64, kTailGuardByte,
                    @"completed future segment suffix guard");

                currentTokens[0] = tokens[0];
                currentTokens[1] = tokens[1];
                currentOwner = replacement;
                require_weak_alive(&currentOwner,
                    @"current replacement owner was not retained");

                uint8_t *expected = malloc(privateLength);
                require(expected != NULL,
                    @"Private expected-readback allocation failed");
                memset(expected, kPrivateSeed, privateLength);
                memcpy(expected + kTypedOffset, currentTokens,
                       sizeof(currentTokens));
                check_private_readback(device, producerQueue, destination,
                    privateLength, expected,
                    [NSString stringWithFormat:
                        @"summary-event round %lu",
                        (unsigned long)round]);
                free(expected);
                require(destination.contents == NULL,
                    @"Private destination became host-visible");

                printf("ARGUMENT_SUMMARY_EVENT_PROGRESS_ROUND round=%lu "
                       "owner_retired=%d current_live=1 source_live=0 "
                       "writer_blocked=1 producer_progress=1 "
                       "fixture_live_samplers_after=%d\n",
                       (unsigned long)round, round ? 1 : 0,
                       currentOwner != nil ? 1 : 0);
            }
        }

        @autoreleasepool {
            id<MTLCommandBuffer> finalFill =
                [writerQueue commandBuffer];
            id<MTLBlitCommandEncoder> blit =
                [finalFill blitCommandEncoder];
            require(finalFill != nil && blit != nil,
                @"final fill encoder unavailable");
            [blit fillBuffer:destination
                       range:NSMakeRange(kTypedOffset, argumentLength)
                       value:0];
            [blit endEncoding];
            complete_command(finalFill, @"summary-event final fill");
            [commandHistory addObject:finalFill];
        }
        require_weak_nil(&currentOwner,
            @"final fill did not retire the current sampler owner");
        require(destination.contents == NULL,
            @"Private destination gained host contents");

        uint8_t *expected = malloc(privateLength);
        require(expected != NULL,
            @"final expected-readback allocation failed");
        memset(expected, kPrivateSeed, privateLength);
        memset(expected + kTypedOffset, 0, argumentLength);
        check_private_readback(device, producerQueue, destination,
            privateLength, expected, @"summary-event final fill");
        free(expected);

        printf("ARGUMENT_SUMMARY_EVENT_PROGRESS_PASS rounds=%lu "
               "source_released_before_gate=%lu writer_incomplete=%lu "
               "owner_retirements=%lu producer_progress=%lu "
               "same_cb_event_blocked=1 independent_producer_queue=1 "
               "final_owner_live=0 private_contents=null "
               "fixture_peak_live_samplers=2 fixture_steady_live_samplers=1 "
               "driver_peak_counters=unavailable_public_api "
               "scratch_pool_counters=unavailable_public_api "
               "command_history=%lu backend=runtime-summary-gpu "
               "translator=0 abi8=0 gpu_execution=1\n",
               (unsigned long)kRounds,
               (unsigned long)sourceReleasedBeforeGate,
               (unsigned long)writerIncompleteCount,
               (unsigned long)ownerRetirements,
               (unsigned long)producerProgressCount,
               (unsigned long)commandHistory.count);
    }
    return 0;
}
