#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void require(BOOL ok, NSString *message) {
    if (!ok) {
        fprintf(stderr, "ARGUMENT_ENCODER_FAIL %s\n", message.UTF8String);
        exit(1);
    }
}

static NSString *artifact(NSString *directory, NSString *name) {
    return [directory stringByAppendingPathComponent:name];
}

static void save(NSData *data, NSString *directory, NSString *name) {
    NSError *error = nil;
    require([data writeToFile:artifact(directory, name) options:NSDataWritingAtomic error:&error],
            error.description ?: @"artifact write failed");
}

static uint32_t pattern(unsigned buffer, NSUInteger word) {
    return 0x12345678u + (uint32_t)word * 0x10203u + buffer * 0x11111111u;
}

#include "test-argument-coverage.inc"
#include "test-argument-layout.inc"

int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc == 2 && !strcmp(argv[1], "--help")) {
            puts("Usage: test-argument-encoder BUNDLE|--reference [OUTPUT_DIR [--encode-only]]\n"
                 "Without OUTPUT_DIR, check the eight A5 layouts and print isolated A7 measurements; no GPU dispatch.\n"
                 "--layout-only checks the eight layouts without the exploratory A7 calls.\n"
                 "With OUTPUT_DIR, execute GPU writes; --encode-only compiles and encodes without pipeline or dispatch.");
            return 0;
        }
        if (argc == 3 && !strcmp(argv[2], "--layout-only")) return argumentLayoutSurvey(argv[1]);
        if (argc == 4 && !strcmp(argv[2], "--a7-probe")) {
            alarm(60);
            return argumentCoverageProbe(argv[1], argv[3]);
        }
        if (argc == 2) {
            int result = argumentLayoutSurvey(argv[1]);
            if (!result) argumentCoverageSurvey(argv[0], argv[1]);
            return result;
        }
        if (argc < 3 || argc > 4) return 2;
        BOOL encodeOnly = argc == 4 && !strcmp(argv[3], "--encode-only");
        if (argc == 4 && !encodeOnly) return 2;
        setvbuf(stdout, NULL, _IONBF, 0);
        NSString *directory = @(argv[2]);
        NSError *error = nil;
        require([[NSFileManager defaultManager] createDirectoryAtPath:directory
                    withIntermediateDirectories:YES attributes:nil error:&error],
                error.description ?: @"output directory creation failed");
        id<MTLDevice> device = nil;
        if (!strcmp(argv[1], "--reference")) device = MTLCreateSystemDefaultDevice();
        else {
            setenv("RDNA4M_ALLOW", "1", 1);
            setenv("RDNA4M_TEST_INPROC", "1", 1);
            require([[NSBundle bundleWithPath:@(argv[1])] loadAndReturnError:&error],
                error.description ?: @"bundle load failed");
            Class cls = objc_getClass("GarnetDevice");
            require(cls != Nil, @"Metal class missing");
            device = [[cls alloc] init];
            if (device) ((void (*)(id, SEL))objc_msgSend)(device, sel_registerName("initLimits"));
        }
        require(device != nil, @"Metal device unavailable");
        NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
            "struct EncodedArgs {\n"
            "  device const uint *input [[id(2)]];\n"
            "  array<device uint *, 2> outputs [[id(5)]];\n"
            "  uint bias [[id(11)]];\n"
            "  array<uint, 3> weights [[id(16)]];\n"
            "};\n"
            "kernel void argument_encoder(device const EncodedArgs &args [[buffer(3)]], "
            "uint i [[thread_position_in_grid]]) {\n"
            "  uint value = args.input[i];\n"
            "  args.outputs[0][i] = (value ^ args.weights[0]) + args.bias + i;\n"
            "  args.outputs[1][i] = value * args.weights[i % 3] + args.bias;\n"
            "}\n";
        save([source dataUsingEncoding:NSUTF8StringEncoding], directory, @"argument-encoder.metal");
        id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
        require(library != nil, error.description ?: @"MSL compilation failed");
        id<MTLFunction> function = [library newFunctionWithName:@"argument_encoder"];
        require(function != nil, @"kernel missing");
        SEL bitcode = sel_registerName("bitcodeData");
        NSData *air = [function respondsToSelector:bitcode]
            ? ((id (*)(id, SEL))objc_msgSend)(function, bitcode) : nil;
        require(air.length > 0, @"function AIR unavailable for layout verification");
        save(air, directory, @"argument-encoder.air");
        id<MTLArgumentEncoder> arguments = [function newArgumentEncoderWithBufferIndex:3];
        require(arguments != nil, @"argument encoder missing for buffer(3)");
        NSUInteger length = arguments.encodedLength, alignment = arguments.alignment;
        require(length >= sizeof(uint32_t) && length <= (1u << 20), @"invalid encodedLength");
        require(alignment > 0 && alignment <= 4096 && !(alignment & (alignment - 1)),
                @"invalid argument encoder alignment");
        NSUInteger argumentOffset = ((256 + alignment - 1) / alignment) * alignment;
        NSUInteger allocationLength = argumentOffset + length + 256;
        id<MTLBuffer> argumentBuffer = [device newBufferWithLength:allocationLength
                                                          options:MTLResourceStorageModeShared];
        require(argumentBuffer != nil && argumentBuffer.contents != NULL, @"argument buffer unavailable");
        memset(argumentBuffer.contents, 0xa7, allocationLength);
        [arguments setArgumentBuffer:argumentBuffer offset:argumentOffset];

        enum { count = 64, guard = 4 };
        NSUInteger offsets[3] = {256 + guard * 4, 256 + 2 * guard * 4, 512 + guard * 4};
        id<MTLBuffer> resources[3];
        for (unsigned b = 0; b < 3; ++b) {
            NSUInteger words = offsets[b] / 4 + count + guard;
            resources[b] = [device newBufferWithLength:words * 4 options:MTLResourceStorageModeShared];
            require(resources[b] != nil && resources[b].contents != NULL, @"resource allocation failed");
            uint32_t *data = resources[b].contents;
            for (NSUInteger j = 0; j < words; ++j) data[j] = pattern(b, j);
        }
        [arguments setBuffer:resources[0] offset:offsets[0] atIndex:2];
        [arguments setBuffer:resources[1] offset:offsets[1] atIndex:5];
        [arguments setBuffer:resources[2] offset:offsets[2] atIndex:6];
        NSUInteger ids[4] = {11, 16, 17, 18};
        uint32_t values[4] = {0x31415926u, 0xa5c39e71u, 17u, 0x10203u};
        NSMutableDictionary *constantOffsets = [NSMutableDictionary dictionary];
        uintptr_t first = (uintptr_t)argumentBuffer.contents + argumentOffset;
        for (unsigned k = 0; k < 4; ++k) {
            void *constant = [arguments constantDataAtIndex:ids[k]];
            uintptr_t address = (uintptr_t)constant;
            require(constant != NULL && address >= first && address - first <= length - sizeof(uint32_t),
                    [NSString stringWithFormat:@"constant ID %lu is outside encoded storage", (unsigned long)ids[k]]);
            for (NSNumber *previous in constantOffsets.allValues)
                require(address - first + 4 <= previous.unsignedLongLongValue ||
                        address - first >= previous.unsignedLongLongValue + 4, @"constant IDs overlap");
            memcpy(constant, &values[k], sizeof values[k]);
            constantOffsets[[@(ids[k]) stringValue]] = @(address - first);
            printf("ARGUMENT_ENCODER_CONSTANT id=%lu offset=%lu bytes=4\n",
                   (unsigned long)ids[k], (unsigned long)(address - first));
        }
        const uint8_t *encoded = argumentBuffer.contents;
        for (NSUInteger j = 0; j < allocationLength; ++j)
            if (j < argumentOffset || j >= argumentOffset + length)
                require(encoded[j] == 0xa7, @"argument encoding crossed destination guards");
        NSDictionary *layout = @{@"encoded_length": @(length), @"alignment": @(alignment),
            @"argument_buffer_index": @3, @"argument_buffer_offset": @(argumentOffset),
            @"constant_offsets": constantOffsets,
            @"resource_offsets": @[@(offsets[0]), @(offsets[1]), @(offsets[2])],
            @"resource_ids": @[@2, @5, @6]};
        NSData *json = [NSJSONSerialization dataWithJSONObject:layout options:NSJSONWritingPrettyPrinted error:&error];
        require(json != nil, error.description ?: @"layout serialization failed");
        save(json, directory, @"encoder-layout.json");
        NSData *argumentSnapshot = [NSData dataWithBytes:argumentBuffer.contents length:allocationLength];
        save(argumentSnapshot, directory, @"argument-buffer.bin");
        printf("ARGUMENT_ENCODER_ENCODING_PASS length=%lu alignment=%lu offset=%lu device=%s\n",
               (unsigned long)length, (unsigned long)alignment, (unsigned long)argumentOffset,
               device.name.UTF8String);
        if (encodeOnly) return 0;

        id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function error:&error];
        require(pipeline != nil, error.description ?: @"pipeline creation failed");
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLCommandBuffer> command = [queue commandBuffer];
        id<MTLComputeCommandEncoder> compute = [command computeCommandEncoder];
        require(compute != nil, @"compute encoder unavailable");
        [compute setComputePipelineState:pipeline];
        [compute setBuffer:argumentBuffer offset:argumentOffset atIndex:3];
        [compute useResource:resources[0] usage:MTLResourceUsageRead];
        [compute useResource:resources[1] usage:MTLResourceUsageWrite];
        [compute useResource:resources[2] usage:MTLResourceUsageWrite];
        [compute dispatchThreadgroups:MTLSizeMake(count, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        [compute endEncoding];
        [command commit];
        [command waitUntilCompleted];
        require(command.status == MTLCommandBufferStatusCompleted && command.error == nil,
                command.error.description ?: @"command did not complete");
        for (unsigned b = 0; b < 3; ++b) {
            const uint32_t *data = resources[b].contents;
            NSUInteger start = offsets[b] / 4;
            for (NSUInteger j = 0; j < resources[b].length / 4; ++j) {
                uint32_t expected = pattern(b, j);
                if (b != 0 && j >= start && j < start + count) {
                    NSUInteger i = j - start;
                    uint32_t input = pattern(0, offsets[0] / 4 + i);
                    expected = b == 1 ? (input ^ values[1]) + values[0] + (uint32_t)i
                                      : input * values[1 + i % 3] + values[0];
                }
                require(data[j] == expected,
                        [NSString stringWithFormat:@"buffer %u word %lu actual=%08x expected=%08x",
                         b, (unsigned long)j, data[j], expected]);
            }
            save([NSData dataWithBytes:resources[b].contents length:resources[b].length], directory,
                 [NSString stringWithFormat:@"result-%u.bin", b]);
        }
        require(memcmp(argumentBuffer.contents, argumentSnapshot.bytes, allocationLength) == 0,
                @"GPU modified argument storage");
        puts("ARGUMENT_ENCODER_PASS words=64 outputs=2 sparse_ids=2,5,6,11,16,17,18 guards=4 resource_offsets=272,288,528");
    }
    return 0;
}
