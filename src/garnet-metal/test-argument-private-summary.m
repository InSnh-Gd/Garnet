/*
 * Public-API GPU retirement test for the Private argument-summary runtime.
 *
 * There is deliberately no MSL consumer and no AIR translation. Synthetic
 * MTLArgumentDescriptors create the same 64-bit sampler tokens an argument
 * encoder would emit; blit copies place them in a Private buffer. The bundle's
 * existing segment/fence path must execute the summary SPIR-V and retire the
 * superseded sampler owners without host access to Private storage.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "garnet_argument_summary.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const NSUInteger kTypedOffset = 256;
static const NSUInteger kTailGuard = 64;
static const NSUInteger kRounds = 10;
static const uint8_t kPrivateSeed = 0x8d;
static const uint8_t kSharedGuard = 0xb4;
static const uint8_t kReadbackGuard = 0x6e;

static void require(BOOL ok, NSString *message) {
    if (!ok) {
        fprintf(stderr, "ARGUMENT_PRIVATE_SUMMARY_FAIL %s\n",
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

static void require_shared_suffix(id<MTLBuffer> source,
                                  NSUInteger encodedLength,
                                  NSString *description) {
    require(source.contents != NULL && encodedLength <= source.length,
            [description stringByAppendingString:@" range invalid"]);
    const uint8_t *bytes = source.contents;
    for (NSUInteger index = encodedLength; index < source.length; ++index)
        require(bytes[index] == kSharedGuard,
                [NSString stringWithFormat:
                 @"%@ changed the Shared guard at %lu", description,
                 (unsigned long)index]);
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
    require_shared_suffix(source, encoder.encodedLength, description);
    memcpy(firstToken, (const uint8_t *)source.contents,
           sizeof(*firstToken));
    memcpy(secondToken,
           (const uint8_t *)source.contents + sizeof(uint64_t),
           sizeof(*secondToken));
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

static void require_weak_nil(__weak id object, NSString *message) {
    for (NSUInteger attempt = 0; attempt < 1000 && object != nil; ++attempt)
        usleep(1000);
    require(object == nil, message);
}

static void require_weak_alive(__weak id object, NSString *message) {
    for (NSUInteger attempt = 0; attempt < 1000 && object == nil; ++attempt)
        usleep(1000);
    require(object != nil, message);
}

int main(int argc, const char *argv[]) {
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        puts("Usage: test-argument-private-summary --bundle BUNDLE\n"
             "Runs Private sampler-token retirement through the bundle's "
             "runtime summary SPIR-V.\n"
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
        NSBundle *bundle =
            [NSBundle bundleWithPath:@(bundlePath)];
        require([bundle loadAndReturnError:&error],
                error.description ?: @"bundle load failed");
        id<MTLDevice> device =
            [[[bundle classNamed:@"GarnetDevice"] alloc] init];
        id<MTLCommandQueue> queue = [device newCommandQueue];
        require(device != nil && queue != nil,
                @"bundle device or command queue unavailable");

        MTLArgumentDescriptor *samplers =
            [MTLArgumentDescriptor argumentDescriptor];
        samplers.dataType = MTLDataTypeSampler;
        samplers.index = 0;
        samplers.arrayLength = 2;
        samplers.access = MTLBindingAccessReadOnly;
        id<MTLArgumentEncoder> encoder =
            [device newArgumentEncoderWithArguments:@[samplers]];
        require(encoder != nil && encoder.encodedLength ==
                    2u * sizeof(uint64_t) && encoder.alignment != 0 &&
                !(kTypedOffset % encoder.alignment),
                @"synthetic sampler argument encoder unavailable");

        NSUInteger argumentLength = encoder.encodedLength;
        require(argumentLength <= NSUIntegerMax - kTypedOffset - kTailGuard,
                @"typed argument placement overflow");
        NSUInteger transportOffset =
            align_up(kTypedOffset + argumentLength + kTailGuard, 8) + 4;
        require(transportOffset > kTypedOffset + argumentLength &&
                (transportOffset & 3u) == 0 &&
                (transportOffset & 7u) == 4,
                @"transport offset is not 4-aligned and non-8-aligned");
        require(argumentLength <= NSUIntegerMax - transportOffset -
                    sizeof(uint64_t) - kTailGuard,
                @"Private buffer length overflow");
        NSUInteger privateLength = align_up(
            transportOffset + sizeof(uint64_t) + kTailGuard, 4);
        id<MTLBuffer> destination =
            [device newBufferWithLength:privateLength
                               options:MTLResourceStorageModePrivate];
        require(destination != nil && destination.contents == NULL,
                @"Private destination is mapped or unavailable");

        __attribute__((objc_precise_lifetime))
        NSMutableArray<id<MTLCommandBuffer>> *completedCommandBuffers =
            [NSMutableArray array];
        __weak id<MTLSamplerState> weakTyped0 = nil;
        __weak id<MTLSamplerState> weakTyped1 = nil;
        __weak id<MTLSamplerState> weakTransport = nil;
        uint64_t typedTokens[2] = {0, 0};
        uint64_t transportToken = 0;

        for (NSUInteger round = 0; round < kRounds; ++round) {
            @autoreleasepool {
                id<MTLCommandBuffer> command = nil;
                __weak id weakTyped0New = nil;
                __weak id weakTyped1New = nil;
                __weak id weakTransportNew = nil;
                __weak id weakSource = nil;
                __weak id weakTransportSource = nil;
                NSUInteger changedSlot =
                    round == 0 ? 0u : ((round - 1u) & 1u);
                uint64_t nextTyped[2] =
                    {typedTokens[0], typedTokens[1]};
                uint64_t nextTransportToken = 0;
                __weak id oldTyped = nil;
                __weak id uncoveredTyped = nil;
                __weak id oldTransport = weakTransport;
                if (round > 0) {
                    if (changedSlot == 0) {
                        oldTyped = weakTyped0;
                        uncoveredTyped = weakTyped1;
                    } else {
                        oldTyped = weakTyped1;
                        uncoveredTyped = weakTyped0;
                    }
                }

                /*
                 * Owner capture builds autoreleased dictionaries/sets that
                 * can retain candidates past the end of encode. Drain the
                 * complete encoding graph before commit and weak checks.
                 */
                @autoreleasepool {
                    id<MTLSamplerState> typed0 = nil;
                    id<MTLSamplerState> typed1 = nil;
                    id<MTLSamplerState> transport = nil;
                    uint64_t sendTokens[2] = {0, 0};
                    uint64_t sendTransportToken = 0;
                    uint64_t unusedTransportToken = 0;

                    if (round == 0) {
                        typed0 = new_sampler(device, 1000);
                        typed1 = new_sampler(device, 1001);
                        weakTyped0New = typed0;
                        weakTyped1New = typed1;
                    } else {
                        typed0 = new_sampler(device,
                            1000 + round * 3u + changedSlot);
                        weakTyped0New = typed0;
                    }
                    transport = new_sampler(device, 2000 + round);
                    weakTransportNew = transport;

                    id<MTLBuffer> source = encode_sampler_pair(
                        device, encoder,
                        typed0,
                        round == 0 ? typed1 : typed0,
                        &sendTokens[0], &sendTokens[1],
                        [NSString stringWithFormat:
                            @"private-summary round %lu typed source",
                            (unsigned long)round]);
                    weakSource = source;
                    id<MTLBuffer> transportSource = encode_sampler_pair(
                        device, encoder, transport, transport,
                        &sendTransportToken, &unusedTransportToken,
                        [NSString stringWithFormat:
                            @"private-summary round %lu transport source",
                            (unsigned long)round]);
                    weakTransportSource = transportSource;

                    if (round == 0) {
                        nextTyped[0] = sendTokens[0];
                        nextTyped[1] = sendTokens[1];
                        require(nextTyped[0] != nextTyped[1],
                            @"initial sampler tokens are identical");
                    } else {
                        nextTyped[changedSlot] = sendTokens[0];
                    }
                    nextTransportToken = sendTransportToken;

                    command = [queue commandBuffer];
                    id<MTLBlitCommandEncoder> blit =
                        [command blitCommandEncoder];
                    require(command != nil && blit != nil,
                        [NSString stringWithFormat:
                            @"private-summary round %lu encoder unavailable",
                            (unsigned long)round]);
                    if (round == 0) {
                        [blit fillBuffer:destination
                                   range:NSMakeRange(0, privateLength)
                                   value:kPrivateSeed];
                        [blit copyFromBuffer:source sourceOffset:0
                                    toBuffer:destination
                           destinationOffset:kTypedOffset
                                        size:argumentLength];
                    } else {
                        [blit copyFromBuffer:source sourceOffset:0
                                    toBuffer:destination
                           destinationOffset:kTypedOffset +
                               changedSlot * sizeof(uint64_t)
                                        size:sizeof(uint64_t)];
                    }
                    [blit copyFromBuffer:transportSource sourceOffset:0
                                toBuffer:destination
                       destinationOffset:transportOffset
                                    size:sizeof(uint64_t)];
                    [blit endEncoding];

                    source = nil;
                    transportSource = nil;
                    typed0 = nil;
                    typed1 = nil;
                    transport = nil;
                }

                require_weak_alive(weakTyped0New,
                    @"new typed sampler did not survive encode drain");
                if (round == 0)
                    require_weak_alive(weakTyped1New,
                        @"second initial sampler did not survive encode drain");
                else {
                    require_weak_alive(oldTyped,
                        @"old typed sampler was released before summary fence");
                    require_weak_alive(uncoveredTyped,
                        @"uncovered typed sampler was released during encode");
                    require_weak_alive(oldTransport,
                        @"old transport sampler was released before summary fence");
                }
                require_weak_alive(weakTransportNew,
                    @"new transport sampler did not survive encode drain");
                require_weak_alive(weakSource,
                    @"typed source was not retained to its segment fence");
                require_weak_alive(weakTransportSource,
                    @"transport source was not retained to its segment fence");

                @autoreleasepool {
                    complete_command(command,
                        [NSString stringWithFormat:
                            @"private-summary round %lu write",
                            (unsigned long)round]);
                }
                [completedCommandBuffers addObject:command];

                if (round == 0) {
                    weakTyped0 = weakTyped0New;
                    weakTyped1 = weakTyped1New;
                } else {
                    require_weak_nil(oldTyped,
                        @"partial typed overwrite retained its old owner");
                    require_weak_alive(uncoveredTyped,
                        @"partial typed overwrite lost an uncovered owner");
                    if (changedSlot == 0)
                        weakTyped0 = weakTyped0New;
                    else
                        weakTyped1 = weakTyped0New;
                }
                require_weak_nil(oldTransport,
                    @"transport overwrite retained the previous owner");
                weakTransport = weakTransportNew;

                typedTokens[0] = nextTyped[0];
                typedTokens[1] = nextTyped[1];
                transportToken = nextTransportToken;
                require_weak_nil(weakSource,
                    @"typed Shared source survived completed round");
                require_weak_nil(weakTransportSource,
                    @"transport Shared source survived completed round");
                require_weak_alive(weakTyped0,
                    @"current slot 0 sampler was not retained");
                require_weak_alive(weakTyped1,
                    @"current slot 1 sampler was not retained");
                require_weak_alive(weakTransport,
                    @"current transport sampler was not retained");

                uint8_t *expected = malloc(privateLength);
                require(expected != NULL,
                        @"expected readback allocation failed");
                memset(expected, kPrivateSeed, privateLength);
                memcpy(expected + kTypedOffset, &typedTokens[0],
                       sizeof(uint64_t));
                memcpy(expected + kTypedOffset + sizeof(uint64_t),
                       &typedTokens[1], sizeof(uint64_t));
                memcpy(expected + transportOffset, &transportToken,
                       sizeof(uint64_t));
                check_private_readback(device, queue, destination,
                    privateLength, expected,
                    [NSString stringWithFormat:
                        @"private-summary round %lu",
                        (unsigned long)round]);
                free(expected);
                require(destination.contents == NULL,
                        @"Private destination became mapped");
            }
        }

        @autoreleasepool {
            NSUInteger ownerStart = kTypedOffset;
            NSUInteger ownerEnd =
                transportOffset + sizeof(uint64_t);
            require(ownerEnd > ownerStart && ownerEnd <= privateLength &&
                    ((ownerEnd - ownerStart) & 3u) == 0,
                    @"final fill range invalid");
            id<MTLCommandBuffer> command = nil;
            @autoreleasepool {
                command = [queue commandBuffer];
                id<MTLBlitCommandEncoder> blit =
                    [command blitCommandEncoder];
                require(command != nil && blit != nil,
                        @"final fill encoder unavailable");
                [blit fillBuffer:destination
                           range:NSMakeRange(ownerStart,
                                             ownerEnd - ownerStart)
                           value:0];
                [blit endEncoding];
            }
            @autoreleasepool {
                complete_command(command, @"private-summary final fill");
            }
            [completedCommandBuffers addObject:command];

            require_weak_nil(weakTyped0,
                @"final fill did not retire current slot 0 sampler");
            require_weak_nil(weakTyped1,
                @"final fill did not retire current slot 1 sampler");
            require_weak_nil(weakTransport,
                @"final fill did not retire current transport sampler");

            uint8_t *expected = malloc(privateLength);
            require(expected != NULL,
                    @"final expected readback allocation failed");
            memset(expected, kPrivateSeed, privateLength);
            memset(expected + ownerStart, 0, ownerEnd - ownerStart);
            check_private_readback(device, queue, destination,
                privateLength, expected,
                @"private-summary final fill");
            free(expected);
        }

        require(destination.contents == NULL,
                @"Private destination gained host contents");
        require(completedCommandBuffers.count == kRounds + 1u,
                [NSString stringWithFormat:
                 @"completed command history=%lu expected=%lu",
                 (unsigned long)completedCommandBuffers.count,
                 (unsigned long)(kRounds + 1u)]);
        printf("ARGUMENT_PRIVATE_SUMMARY_PASS rounds=%lu "
               "typed_tokens=2 transport_tokens=1 "
               "partial_overwrites=%lu final_fill=1 "
               "command_history=%lu weak_owners_retired=1 "
               "private_contents=null backend=runtime-summary-gpu "
               "translator=0 abi8=0 gpu_execution=1\n",
               (unsigned long)kRounds,
               (unsigned long)(2u * (kRounds - 1u)),
               (unsigned long)completedCommandBuffers.count);
    }
    return 0;
}
