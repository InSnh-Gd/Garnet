#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const NSUInteger kTypedOffset = 256;
static const NSUInteger kOutputOffset = 32;
static const NSUInteger kOutputLength = 64;
static const NSUInteger kArgumentTail = 64;
static const uint8_t kSharedGuard = 0xa5;
static const uint8_t kPrivateSeed = 0xd3;
static const uint8_t kOutputGuard = 0xcd;

static void require(BOOL ok, NSString *message) {
    if (!ok) {
        fprintf(stderr, "ARGUMENT_PRIVATE_RETIRE_FAIL %s\n",
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

static void require_shared_guard(id<MTLBuffer> buffer, NSRange payload,
                                 uint8_t guard, NSString *description) {
    require(buffer.contents != NULL && payload.location <= buffer.length &&
            payload.length <= buffer.length - payload.location,
            [description stringByAppendingString:@" range invalid"]);
    const uint8_t *bytes = buffer.contents;
    NSUInteger end = NSMaxRange(payload);
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

static id<MTLSamplerState> new_sampler(id<MTLDevice> device, NSUInteger tag,
                                       MTLSamplerAddressMode addressMode) {
    MTLSamplerDescriptor *descriptor = [MTLSamplerDescriptor new];
    descriptor.supportArgumentBuffers = YES;
    descriptor.minFilter = MTLSamplerMinMagFilterNearest;
    descriptor.magFilter = MTLSamplerMinMagFilterNearest;
    descriptor.mipFilter = MTLSamplerMipFilterNotMipmapped;
    descriptor.sAddressMode = addressMode;
    descriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
    descriptor.rAddressMode = MTLSamplerAddressModeClampToEdge;
    descriptor.normalizedCoordinates = YES;
    descriptor.lodMinClamp = 0.0f;
    descriptor.lodMaxClamp = 1.0f + (float)tag;
    descriptor.borderColor = MTLSamplerBorderColorTransparentBlack;
    id<MTLSamplerState> sampler =
        [device newSamplerStateWithDescriptor:descriptor];
    require(sampler != nil,
            [NSString stringWithFormat:@"sampler tag %lu unavailable",
             (unsigned long)tag]);
    return sampler;
}

static id<MTLTexture> new_sample_texture(id<MTLDevice> device) {
    MTLTextureDescriptor *descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
            MTLPixelFormatRGBA32Float width:2 height:1 mipmapped:NO];
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
    require(texture != nil, @"sample texture unavailable");
    const float pixels[8] = {
        11.0f, 0.0f, 0.0f, 1.0f,
        29.0f, 0.0f, 0.0f, 1.0f,
    };
    [texture replaceRegion:MTLRegionMake2D(0, 0, 2, 1)
               mipmapLevel:0 withBytes:pixels bytesPerRow:sizeof pixels];
    return texture;
}

static id<MTLBuffer> new_argument_source(
        id<MTLDevice> device, id<MTLArgumentEncoder> encoder,
        id<MTLTexture> texture, id<MTLSamplerState> first,
        id<MTLSamplerState> second, NSString *description) {
    NSUInteger length = encoder.encodedLength;
    id<MTLBuffer> source = new_shared_buffer(
        device, length + kArgumentTail, kSharedGuard, description);
    [encoder setArgumentBuffer:source offset:0];
    [encoder setTexture:texture atIndex:0];
    [encoder setSamplerState:first atIndex:1];
    [encoder setSamplerState:second atIndex:2];
    [encoder setArgumentBuffer:nil offset:0];
    require_shared_guard(source, NSMakeRange(0, length), kSharedGuard,
                         description);
    return source;
}

static NSUInteger single_changed_word(id<MTLBuffer> left,
                                      id<MTLBuffer> right,
                                      NSUInteger length,
                                      NSString *description) {
    require(left.contents != NULL && right.contents != NULL &&
            length <= left.length && length <= right.length,
            [description stringByAppendingString:@" inputs invalid"]);
    const uint8_t *a = left.contents;
    const uint8_t *b = right.contents;
    NSUInteger changed = NSUIntegerMax;
    NSUInteger count = 0;
    for (NSUInteger offset = 0; offset + sizeof(uint64_t) <= length;
         offset += sizeof(uint64_t)) {
        if (memcmp(a + offset, b + offset, sizeof(uint64_t)) != 0) {
            changed = offset;
            ++count;
        }
    }
    require(count == 1,
            [NSString stringWithFormat:@"%@ changed words=%lu",
             description, (unsigned long)count]);
    return changed;
}

static void discover_token_offsets(
        id<MTLDevice> device, id<MTLArgumentEncoder> encoder,
        id<MTLTexture> texture, id<MTLSamplerState> a,
        id<MTLSamplerState> b, id<MTLSamplerState> c,
        id<MTLSamplerState> t, NSUInteger *firstOffset,
        NSUInteger *secondOffset) {
    require(firstOffset != NULL && secondOffset != NULL,
            @"token offset output missing");
    id<MTLBuffer> base = new_argument_source(
        device, encoder, texture, a, b, @"token layout base");
    id<MTLBuffer> firstVariant = new_argument_source(
        device, encoder, texture, c, b, @"token layout first variant");
    id<MTLBuffer> secondVariant = new_argument_source(
        device, encoder, texture, a, t, @"token layout second variant");
    NSUInteger first = single_changed_word(
        base, firstVariant, encoder.encodedLength,
        @"first sampler token layout");
    NSUInteger second = single_changed_word(
        base, secondVariant, encoder.encodedLength,
        @"second sampler token layout");
    require(first != second && sizeof(uint64_t) <= encoder.encodedLength &&
            first + sizeof(uint64_t) <= encoder.encodedLength &&
            second + sizeof(uint64_t) <= encoder.encodedLength,
            @"sampler token offsets overlap or escape the argument block");
    *firstOffset = first;
    *secondOffset = second;
}

static void write_air(id<MTLFunction> function, NSString *path) {
    SEL selector = NSSelectorFromString(@"bitcodeData");
    NSData *air = [function respondsToSelector:selector] ?
        ((id (*)(id, SEL))objc_msgSend)(function, selector) : nil;
    require(air.length != 0 && [air writeToFile:path atomically:YES],
            @"AIR write failed");
}

static void complete_command(id<MTLCommandBuffer> command,
                             NSMutableArray<id<MTLCommandBuffer>> *history,
                             NSString *operation) {
    [command commit];
    [command waitUntilCompleted];
    require(command.status == MTLCommandBufferStatusCompleted &&
            !command.error,
            command.error.description ?:
                [operation stringByAppendingString:@" failed"]);
    [history addObject:command];
}

static void require_output(id<MTLBuffer> output, float first, float second,
                           NSString *description) {
    require(output.contents != NULL &&
            sizeof(float) <= kOutputLength &&
            2 * sizeof(float) <= kOutputLength,
            @"output range invalid");
    require_shared_guard(output,
                         NSMakeRange(kOutputOffset, 2 * sizeof(float)),
                         kOutputGuard, description);
    float actual[2] = {0.0f, 0.0f};
    memcpy(actual, (const uint8_t *)output.contents + kOutputOffset,
           sizeof actual);
    require(fabsf(actual[0] - first) <= 1.0e-6f &&
            fabsf(actual[1] - second) <= 1.0e-6f,
            [NSString stringWithFormat:
             @"%@ output=[%.9g,%.9g] expected=[%.9g,%.9g]",
             description, actual[0], actual[1], first, second]);
}

static void require_zero_range(id<MTLBuffer> buffer, NSRange range,
                               NSString *description) {
    require(buffer.contents != NULL && range.location <= buffer.length &&
            range.length <= buffer.length - range.location,
            [description stringByAppendingString:@" range invalid"]);
    const uint8_t *bytes = buffer.contents;
    for (NSUInteger i = range.location; i < NSMaxRange(range); ++i)
        if (bytes[i] != 0)
            require(NO, [NSString stringWithFormat:
                @"%@ nonzero byte %u at %lu", description, bytes[i],
                (unsigned long)i]);
}

static void require_private_seed(id<MTLBuffer> buffer, NSRange range,
                                 NSString *description) {
    require(buffer.contents != NULL && range.location <= buffer.length &&
            range.length <= buffer.length - range.location,
            [description stringByAppendingString:@" range invalid"]);
    const uint8_t *bytes = buffer.contents;
    for (NSUInteger i = range.location; i < NSMaxRange(range); ++i)
        if (bytes[i] != kPrivateSeed)
            require(NO, [NSString stringWithFormat:
                @"%@ seed changed to %u at %lu", description, bytes[i],
                (unsigned long)i]);
}

#define WAIT_WEAK_NIL(object, message) do { \
    for (NSUInteger attempt_ = 0; attempt_ < 1000 && (object) != nil; \
         ++attempt_) \
        usleep(1000); \
    require((object) == nil, (message)); \
} while (0)

#define WAIT_WEAK_ALIVE(object, message) do { \
    for (NSUInteger attempt_ = 0; attempt_ < 1000 && (object) == nil; \
         ++attempt_) \
        usleep(1000); \
    require((object) != nil, (message)); \
} while (0)

int main(int argc, const char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        puts("Usage: test-argument-private-retire --layout-only OUTPUT_DIR\n"
             "       test-argument-private-retire --reference OUTPUT_DIR\n"
             "       test-argument-private-retire BUNDLE OUTPUT_DIR\n"
             "layout-only compiles MSL/AIR and validates argument offsets "
             "without GPU submissions.");
        return 0;
    }
    BOOL layoutOnly = argc == 3 && !strcmp(argv[1], "--layout-only");
    BOOL reference = argc == 3 && !strcmp(argv[1], "--reference");
    BOOL candidate = argc == 3 && !layoutOnly && !reference;
    if (!layoutOnly && !reference && !candidate) return 2;

    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(150);
    @autoreleasepool {
        NSError *error = nil;
        NSString *directory = @(argv[2]);
        require([[NSFileManager defaultManager]
            createDirectoryAtPath:directory
            withIntermediateDirectories:YES attributes:nil error:&error],
            error.description ?: @"output directory unavailable");

        NSString *source =
            @"#include <metal_stdlib>\n"
            "using namespace metal;\n"
            "struct PrivateArgs { texture2d<float> image [[id(0)]]; "
            "sampler first [[id(1)]]; sampler second [[id(2)]]; };\n"
            "kernel void consume_private(constant PrivateArgs &args "
            "[[buffer(1)]], device float *out [[buffer(0)]]) {\n"
            "  float2 p = float2(1.25f, 0.5f);\n"
            "  out[0] = args.image.sample(args.first, p).x;\n"
            "  out[1] = args.image.sample(args.second, p).x;\n"
            "}\n";
        NSString *sourcePath =
            [directory stringByAppendingPathComponent:
                @"argument-private-retire.metal"];
        require([source writeToFile:sourcePath atomically:YES
            encoding:NSUTF8StringEncoding error:&error],
            @"source write failed");

        id<MTLDevice> device = nil;
        if (layoutOnly || reference) {
            device = MTLCreateSystemDefaultDevice();
        } else {
            NSBundle *bundle = [NSBundle bundleWithPath:@(argv[1])];
            require([bundle loadAndReturnError:&error],
                    error.description ?: @"bundle load failed");
            device = [[[bundle classNamed:@"GarnetDevice"] alloc] init];
        }
        require(device != nil, @"Metal device unavailable");

        MTLCompileOptions *options = [MTLCompileOptions new];
        options.languageVersion = MTLLanguageVersion2_1;
        id<MTLLibrary> library =
            [device newLibraryWithSource:source options:options error:&error];
        require(library != nil,
                error.description ?: @"source compilation failed");
        id<MTLFunction> function =
            [library newFunctionWithName:@"consume_private"];
        require(function != nil, @"consume_private function missing");
        id<MTLComputePipelineState> pipeline =
            [device newComputePipelineStateWithFunction:function
                                                  error:&error];
        require(pipeline != nil,
                error.description ?: @"consumer pipeline failed");
        id<MTLArgumentEncoder> encoder =
            [function newArgumentEncoderWithBufferIndex:1];
        require(encoder != nil && encoder.encodedLength >=
                2 * sizeof(uint64_t) && encoder.alignment != 0,
                @"argument encoder unavailable");
        id<MTLTexture> texture = new_sample_texture(device);

        NSUInteger firstTokenOffset = 0;
        NSUInteger secondTokenOffset = 0;
        if (layoutOnly) {
            @autoreleasepool {
                id<MTLSamplerState> a = new_sampler(
                    device, 100, MTLSamplerAddressModeClampToEdge);
                id<MTLSamplerState> b = new_sampler(
                    device, 101, MTLSamplerAddressModeRepeat);
                id<MTLSamplerState> c = new_sampler(
                    device, 102, MTLSamplerAddressModeClampToZero);
                id<MTLSamplerState> t = new_sampler(
                    device, 103, MTLSamplerAddressModeRepeat);
                discover_token_offsets(device, encoder, texture, a, b, c, t,
                                       &firstTokenOffset, &secondTokenOffset);
            }
            write_air(function,
                      [directory stringByAppendingPathComponent:
                          @"argument-private-retire.air"]);
            printf("ARGUMENT_PRIVATE_RETIRE_LAYOUT bytes=%lu alignment=%lu "
                   "first_token_offset=%lu second_token_offset=%lu "
                   "samplers=2 texture=1 transport=separate "
                   "msl=2.1 gpu_execution=0 submissions=0\n",
                   (unsigned long)encoder.encodedLength,
                   (unsigned long)encoder.alignment,
                   (unsigned long)firstTokenOffset,
                   (unsigned long)secondTokenOffset);
            return 0;
        }

        BOOL ownerAssertions = candidate;
        NSUInteger argumentLength = encoder.encodedLength;
        require(argumentLength <= NSUIntegerMax - kTypedOffset,
                @"typed argument range overflow");
        require(!(kTypedOffset % encoder.alignment),
                @"typed argument offset is misaligned");
        require(argumentLength <=
                    NSUIntegerMax - kTypedOffset - kArgumentTail,
                @"transport placement overflow");
        NSUInteger transportOffset =
            align_up(kTypedOffset + argumentLength + kArgumentTail,
                     MAX((NSUInteger)16, encoder.alignment));
        require(transportOffset >= kTypedOffset + argumentLength &&
                transportOffset - (kTypedOffset + argumentLength) >=
                    kArgumentTail,
                @"transport region overlaps typed arguments");
        require(argumentLength <=
                    NSUIntegerMax - transportOffset - kArgumentTail,
                @"Private buffer length overflow");
        NSUInteger privateLength =
            transportOffset + argumentLength + kArgumentTail;
        id<MTLBuffer> destination =
            [device newBufferWithLength:privateLength
                               options:MTLResourceStorageModePrivate];
        id<MTLBuffer> output = new_shared_buffer(
            device, kOutputLength, kOutputGuard, @"private-retire output");
        require(destination != nil && destination.contents == NULL,
                @"Private destination is mapped or unavailable");

        __attribute__((objc_precise_lifetime))
        NSMutableArray<id<MTLCommandBuffer>> *commandHistory =
            [NSMutableArray array];
        // Keeping every completed command alive exposes stale _refs retention;
        // sampler/source wrappers deliberately never enter this history.
        NSMutableArray<id<MTLSamplerState>> *referenceSamplerKeepAlive =
            reference ? [NSMutableArray array] : nil;
        id<MTLCommandQueue> queue = [device newCommandQueue];
        require(queue != nil, @"command queue unavailable");

        id<MTLSamplerState> a = nil;
        id<MTLSamplerState> b = nil;
        id<MTLSamplerState> c = nil;
        id<MTLSamplerState> t = nil;
        __weak id<MTLSamplerState> weakA = nil;
        __weak id<MTLSamplerState> weakB = nil;
        __weak id<MTLSamplerState> weakC = nil;
        __weak id<MTLSamplerState> weakT = nil;
        @autoreleasepool {
            a = new_sampler(device, 100, MTLSamplerAddressModeClampToEdge);
            b = new_sampler(device, 101, MTLSamplerAddressModeRepeat);
            c = new_sampler(device, 102, MTLSamplerAddressModeClampToZero);
            t = new_sampler(device, 103, MTLSamplerAddressModeRepeat);
            weakA = a;
            weakB = b;
            weakC = c;
            weakT = t;
            if (reference) {
                [referenceSamplerKeepAlive addObject:a];
                [referenceSamplerKeepAlive addObject:b];
                [referenceSamplerKeepAlive addObject:c];
                [referenceSamplerKeepAlive addObject:t];
            }
            discover_token_offsets(device, encoder, texture, a, b, c, t,
                                   &firstTokenOffset, &secondTokenOffset);
        }
        printf("ARGUMENT_PRIVATE_RETIRE_LAYOUT bytes=%lu alignment=%lu "
               "typed_offset=%lu transport_offset=%lu "
               "first_token_offset=%lu second_token_offset=%lu msl=2.1\n",
               (unsigned long)argumentLength,
               (unsigned long)encoder.alignment,
               (unsigned long)kTypedOffset,
               (unsigned long)transportOffset,
               (unsigned long)firstTokenOffset,
               (unsigned long)secondTokenOffset);

        __weak id<MTLBuffer> initialSource = nil;
        @autoreleasepool {
            id<MTLBuffer> source = new_argument_source(
                device, encoder, texture, a, b,
                @"private-retire initial source");
            initialSource = source;
            require_shared_guard(source, NSMakeRange(0, argumentLength),
                                 kSharedGuard,
                                 @"private-retire initial source");

            id<MTLCommandBuffer> command = [queue commandBuffer];
            id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
            require(command != nil && blit != nil,
                    @"initial copy encoder unavailable");
            [blit fillBuffer:destination range:NSMakeRange(0, privateLength)
                       value:kPrivateSeed];
            [blit copyFromBuffer:source sourceOffset:0 toBuffer:destination
                destinationOffset:kTypedOffset size:argumentLength];
            [blit endEncoding];

            id<MTLComputeCommandEncoder> compute =
                [command computeCommandEncoder];
            require(compute != nil, @"initial consumer encoder unavailable");
            [compute setComputePipelineState:pipeline];
            [compute setBuffer:destination offset:kTypedOffset atIndex:1];
            [compute setBuffer:output offset:kOutputOffset atIndex:0];
            [compute useResource:texture usage:MTLResourceUsageRead];
            [compute dispatchThreadgroups:MTLSizeMake(1, 1, 1)
                threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
            [compute endEncoding];
            source = nil;
            // Drop application references before commit; the copy intent owns
            // the immutable token candidates until GPU retirement.
            if (ownerAssertions) {
                a = nil;
                b = nil;
            }
            complete_command(command, commandHistory,
                             @"private-retire initial copy");
        }
        require(destination.contents == NULL,
                @"Private destination became host-mapped");
        if (ownerAssertions) {
            WAIT_WEAK_NIL(initialSource,
                          @"initial Shared argument source survived completion");
            WAIT_WEAK_ALIVE(weakA,
                            @"initial first sampler owner was not retained");
            WAIT_WEAK_ALIVE(weakB,
                            @"initial second sampler owner was not retained");
        }
        require_output(output, 29.0f, 11.0f,
                       @"initial clamp/repeat sample");

        __weak id<MTLBuffer> transportSourceWeak = nil;
        @autoreleasepool {
            id<MTLBuffer> source = new_argument_source(
                device, encoder, texture, t, t,
                @"private-retire transport source");
            transportSourceWeak = source;
            require_shared_guard(source, NSMakeRange(0, argumentLength),
                                 kSharedGuard,
                                 @"private-retire transport source");
            id<MTLCommandBuffer> command = [queue commandBuffer];
            id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
            require(command != nil && blit != nil,
                    @"transport copy encoder unavailable");
            [blit copyFromBuffer:source sourceOffset:firstTokenOffset
                         toBuffer:destination
                destinationOffset:transportOffset
                             size:sizeof(uint64_t)];
            [blit endEncoding];
            source = nil;
            if (ownerAssertions) t = nil;
            complete_command(command, commandHistory,
                             @"private-retire transport copy");
        }
        if (ownerAssertions) {
            WAIT_WEAK_NIL(transportSourceWeak,
                          @"transport source survived completion");
            WAIT_WEAK_ALIVE(weakT,
                            @"transport sampler owner was not retained");
        }

        __weak id<MTLBuffer> firstOverwriteSource = nil;
        @autoreleasepool {
            id<MTLSamplerState> encodedB = weakB;
            require(encodedB != nil,
                    @"second sampler vanished before first overwrite");
            // Both slots are non-null here; no undefined null-member sampling
            // is used to stand in for an ownership check.
            id<MTLBuffer> source = new_argument_source(
                device, encoder, texture, c, encodedB,
                @"private-retire first overwrite source");
            firstOverwriteSource = source;
            require_shared_guard(source, NSMakeRange(0, argumentLength),
                                 kSharedGuard,
                                 @"private-retire first overwrite source");
            id<MTLCommandBuffer> command = [queue commandBuffer];
            id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
            require(command != nil && blit != nil,
                    @"first overwrite encoder unavailable");
            [blit copyFromBuffer:source sourceOffset:firstTokenOffset
                         toBuffer:destination
                destinationOffset:kTypedOffset + firstTokenOffset
                             size:sizeof(uint64_t)];
            [blit endEncoding];

            id<MTLComputeCommandEncoder> compute =
                [command computeCommandEncoder];
            require(compute != nil,
                    @"first overwrite consumer unavailable");
            [compute setComputePipelineState:pipeline];
            [compute setBuffer:destination offset:kTypedOffset atIndex:1];
            [compute setBuffer:output offset:kOutputOffset atIndex:0];
            [compute useResource:texture usage:MTLResourceUsageRead];
            [compute dispatchThreadgroups:MTLSizeMake(1, 1, 1)
                threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
            [compute endEncoding];
            source = nil;
            if (ownerAssertions) c = nil;
            complete_command(command, commandHistory,
                             @"private-retire first overwrite");
        }
        if (ownerAssertions) {
            WAIT_WEAK_NIL(firstOverwriteSource,
                          @"first overwrite source survived completion");
            WAIT_WEAK_NIL(weakA,
                          @"overwritten first sampler did not retire");
            WAIT_WEAK_ALIVE(weakB,
                            @"uncovered second sampler owner was lost");
            WAIT_WEAK_ALIVE(weakC,
                            @"replacement first sampler owner was not published");
            WAIT_WEAK_ALIVE(weakT,
                            @"typed overwrite retired the transport sampler");
        }
        require_output(output, 0.0f, 11.0f,
                       @"first overwrite clamp-to-zero/repeat sample");

        id<MTLBuffer> transportReadback = new_shared_buffer(
            device, argumentLength, kSharedGuard,
            @"private-retire transport readback");
        @autoreleasepool {
            id<MTLCommandBuffer> command = [queue commandBuffer];
            id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
            require(command != nil && blit != nil,
                    @"transport fill encoder unavailable");
            [blit fillBuffer:destination
                       range:NSMakeRange(transportOffset, argumentLength)
                       value:0];
            [blit copyFromBuffer:destination sourceOffset:transportOffset
                         toBuffer:transportReadback destinationOffset:0
                             size:argumentLength];
            [blit endEncoding];
            complete_command(command, commandHistory,
                             @"private-retire transport fill");
        }
        require_zero_range(transportReadback,
                           NSMakeRange(0, argumentLength),
                           @"filled Private transport range");
        if (ownerAssertions) {
            WAIT_WEAK_NIL(weakT,
                          @"filled transport sampler did not retire");
            WAIT_WEAK_ALIVE(weakB,
                            @"transport fill retired the uncovered second sampler");
            WAIT_WEAK_ALIVE(weakC,
                            @"transport fill retired the current first sampler");
        }
        transportReadback = nil;
        require(destination.contents == NULL,
                @"Private destination gained host contents after fill");

        id<MTLSamplerState> d = nil;
        id<MTLSamplerState> e = nil;
        __weak id<MTLSamplerState> weakD = nil;
        __weak id<MTLSamplerState> weakE = nil;
        @autoreleasepool {
            d = new_sampler(device, 200, MTLSamplerAddressModeClampToEdge);
            e = new_sampler(device, 201, MTLSamplerAddressModeRepeat);
            weakD = d;
            weakE = e;
            if (reference) {
                [referenceSamplerKeepAlive addObject:d];
                [referenceSamplerKeepAlive addObject:e];
            }
        }
        __weak id<MTLBuffer> roundTwoSource = nil;
        @autoreleasepool {
            id<MTLBuffer> source = new_argument_source(
                device, encoder, texture, d, e,
                @"private-retire round-two source");
            roundTwoSource = source;
            require_shared_guard(source, NSMakeRange(0, argumentLength),
                                 kSharedGuard,
                                 @"private-retire round-two source");
            id<MTLCommandBuffer> command = [queue commandBuffer];
            id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
            require(command != nil && blit != nil,
                    @"round-two copy encoder unavailable");
            [blit copyFromBuffer:source sourceOffset:0 toBuffer:destination
                destinationOffset:kTypedOffset size:argumentLength];
            [blit endEncoding];
            id<MTLComputeCommandEncoder> compute =
                [command computeCommandEncoder];
            require(compute != nil,
                    @"round-two consumer encoder unavailable");
            [compute setComputePipelineState:pipeline];
            [compute setBuffer:destination offset:kTypedOffset atIndex:1];
            [compute setBuffer:output offset:kOutputOffset atIndex:0];
            [compute useResource:texture usage:MTLResourceUsageRead];
            [compute dispatchThreadgroups:MTLSizeMake(1, 1, 1)
                threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
            [compute endEncoding];
            source = nil;
            if (ownerAssertions) e = nil;
            complete_command(command, commandHistory,
                             @"private-retire round-two copy");
        }
        if (ownerAssertions) {
            WAIT_WEAK_NIL(roundTwoSource,
                          @"round-two source survived completion");
            WAIT_WEAK_NIL(weakB,
                          @"round-two overwrite did not retire old second sampler");
            WAIT_WEAK_NIL(weakC,
                          @"round-two overwrite did not retire old first sampler");
            WAIT_WEAK_ALIVE(weakD,
                            @"round-two first sampler owner was not published");
            WAIT_WEAK_ALIVE(weakE,
                            @"round-two second sampler owner was not published");
        }
        require_output(output, 29.0f, 11.0f,
                       @"round-two clamp/repeat sample");

        id<MTLSamplerState> f = nil;
        __weak id<MTLSamplerState> weakF = nil;
        @autoreleasepool {
            f = new_sampler(device, 202, MTLSamplerAddressModeClampToZero);
            weakF = f;
            if (reference) [referenceSamplerKeepAlive addObject:f];
        }
        __weak id<MTLBuffer> secondOverwriteSource = nil;
        @autoreleasepool {
            id<MTLBuffer> source = new_argument_source(
                device, encoder, texture, d, f,
                @"private-retire second overwrite source");
            secondOverwriteSource = source;
            require_shared_guard(source, NSMakeRange(0, argumentLength),
                                 kSharedGuard,
                                 @"private-retire second overwrite source");
            id<MTLCommandBuffer> command = [queue commandBuffer];
            id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
            require(command != nil && blit != nil,
                    @"second overwrite encoder unavailable");
            [blit copyFromBuffer:source sourceOffset:secondTokenOffset
                         toBuffer:destination
                destinationOffset:kTypedOffset + secondTokenOffset
                             size:sizeof(uint64_t)];
            [blit endEncoding];
            id<MTLComputeCommandEncoder> compute =
                [command computeCommandEncoder];
            require(compute != nil,
                    @"second overwrite consumer unavailable");
            [compute setComputePipelineState:pipeline];
            [compute setBuffer:destination offset:kTypedOffset atIndex:1];
            [compute setBuffer:output offset:kOutputOffset atIndex:0];
            [compute useResource:texture usage:MTLResourceUsageRead];
            [compute dispatchThreadgroups:MTLSizeMake(1, 1, 1)
                threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
            [compute endEncoding];
            source = nil;
            if (ownerAssertions) {
                d = nil;
                f = nil;
            }
            complete_command(command, commandHistory,
                             @"private-retire second overwrite");
        }
        if (ownerAssertions) {
            WAIT_WEAK_NIL(secondOverwriteSource,
                          @"second overwrite source survived completion");
            WAIT_WEAK_NIL(weakE,
                          @"overwritten round-two second sampler did not retire");
            WAIT_WEAK_ALIVE(weakD,
                            @"uncovered round-two first sampler owner was lost");
            WAIT_WEAK_ALIVE(weakF,
                            @"replacement round-two second sampler was not published");
        }
        require_output(output, 29.0f, 0.0f,
                       @"second overwrite clamp/repeat-zero sample");

        id<MTLBuffer> finalReadback = new_shared_buffer(
            device, privateLength, kSharedGuard,
            @"private-retire final readback");
        @autoreleasepool {
            id<MTLCommandBuffer> command = [queue commandBuffer];
            id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
            require(command != nil && blit != nil,
                    @"final fill encoder unavailable");
            [blit fillBuffer:destination
                       range:NSMakeRange(kTypedOffset, argumentLength)
                       value:0];
            [blit copyFromBuffer:destination sourceOffset:0
                         toBuffer:finalReadback destinationOffset:0
                             size:privateLength];
            [blit endEncoding];
            complete_command(command, commandHistory,
                             @"private-retire final fill");
        }
        if (ownerAssertions) {
            WAIT_WEAK_NIL(weakD,
                          @"final fill did not retire round-two first sampler");
            WAIT_WEAK_NIL(weakF,
                          @"final fill did not retire round-two second sampler");
        }
        require_zero_range(finalReadback,
                           NSMakeRange(kTypedOffset, argumentLength),
                           @"final filled typed range");
        require_zero_range(finalReadback,
                           NSMakeRange(transportOffset, argumentLength),
                           @"previously filled transport range");
        require_private_seed(finalReadback, NSMakeRange(0, kTypedOffset),
                             @"Private prefix guard");
        require_private_seed(finalReadback,
            NSMakeRange(kTypedOffset + argumentLength,
                        transportOffset -
                            (kTypedOffset + argumentLength)),
            @"Private inter-range guard");
        require_private_seed(finalReadback,
            NSMakeRange(transportOffset + argumentLength,
                        privateLength -
                            (transportOffset + argumentLength)),
            @"Private suffix guard");
        finalReadback = nil;
        require(destination.contents == NULL,
                @"Private destination became mapped at final retirement");
        require(commandHistory.count == 7,
                [NSString stringWithFormat:
                    @"completed command history=%lu expected=7",
                    (unsigned long)commandHistory.count]);

        if (ownerAssertions) {
            WAIT_WEAK_NIL(weakA, @"initial first sampler remained live");
            WAIT_WEAK_NIL(weakB, @"initial second sampler remained live");
            WAIT_WEAK_NIL(weakC, @"first replacement sampler remained live");
            WAIT_WEAK_NIL(weakT, @"transport sampler remained live");
            WAIT_WEAK_NIL(weakD, @"round-two first sampler remained live");
            WAIT_WEAK_NIL(weakE, @"round-two second sampler remained live");
            WAIT_WEAK_NIL(weakF, @"final replacement sampler remained live");
            printf("ARGUMENT_PRIVATE_RETIRE_PASS rounds=2 typed_tokens=2 "
                   "transport=separate partial_overwrite=2 "
                   "uncovered_preserved=1 fill_retire=2 "
                   "command_history=7 old_command_refs_retired=1 "
                   "private_contents=null consumer_samples=6 "
                   "gpu_execution=1 backend=bundle msl=2.1\n");
        } else {
            printf("ARGUMENT_PRIVATE_RETIRE_REFERENCE_PASS rounds=2 "
                   "typed_tokens=2 transport=separate partial_overwrite=2 "
                   "command_history=7 consumer_samples=6 "
                   "gpu_execution=1 backend=reference msl=2.1\n");
        }
    }
    return 0;
}
