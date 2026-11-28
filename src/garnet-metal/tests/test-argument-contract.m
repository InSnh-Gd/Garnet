#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdint.h>
#include <string.h>

static void require(BOOL ok, const char *message) {
    if (!ok) { fprintf(stderr, "ARGUMENT_CONTRACT_FAIL %s\n", message); exit(1); }
}

static void rejected(void (^operation)(void), const char *name) {
    @try { operation(); }
    @catch (NSException *exception) {
        require([exception.name isEqualToString:NSInvalidArgumentException], "unexpected exception type");
        printf("ARGUMENT_CONTRACT_REJECT %s\n", name);
        return;
    }
    require(NO, name);
}

int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc == 2 && !strcmp(argv[1], "--help")) {
            puts("Usage: test-argument-contract BUNDLE\nChecks descriptor factories, array destinations, GPU output and invalid encoding.");
            return 0;
        }
        if (argc != 2) return 2;
        setvbuf(stdout, NULL, _IONBF, 0);
        NSError *error = nil;
        NSBundle *bundle = [NSBundle bundleWithPath:@(argv[1])];
        require([bundle loadAndReturnError:&error], "bundle load failed");
        id<MTLDevice> device = [[[bundle classNamed:@"GarnetDevice"] alloc] init];
        require(device != nil, "device missing");
        MTLArgumentDescriptor *pointer = [MTLArgumentDescriptor argumentDescriptor];
        pointer.dataType = MTLDataTypePointer; pointer.index = 2; pointer.arrayLength = 2;
        MTLArgumentDescriptor *constant = [MTLArgumentDescriptor argumentDescriptor];
        constant.dataType = MTLDataTypeUInt; constant.index = 11; constant.arrayLength = 1;
        printf("ARGUMENT_CONTRACT_DESCRIPTORS pointer_type=%lu pointer_count=%lu constant_type=%lu constant_count=%lu\n",
               (unsigned long)pointer.dataType, (unsigned long)pointer.arrayLength,
               (unsigned long)constant.dataType, (unsigned long)constant.arrayLength);
        id<MTLArgumentEncoder> encoder = [device newArgumentEncoderWithArguments:@[constant, pointer]];
        require(encoder != nil && encoder.encodedLength > 0 && encoder.alignment > 0, "descriptor encoder missing");
        NSUInteger length = encoder.encodedLength;
        require(length <= 256 && !(length % encoder.alignment), "invalid descriptor layout");
        NSUInteger base = 256 - length;
        id<MTLBuffer> destination = [device newBufferWithLength:base + length * 3 options:0];
        id<MTLBuffer> resource = [device newBufferWithLength:1024 options:0];
        require(destination != nil && resource != nil, "buffer allocation failed");
        memset(destination.contents, 0x6d, destination.length);
        [encoder setArgumentBuffer:destination startOffset:base arrayElement:1];
        id<MTLBuffer> buffers[2] = {resource, nil};
        NSUInteger offsets[2] = {36, 0};
        [encoder setBuffers:buffers offsets:offsets withRange:NSMakeRange(2, 2)];
        uint32_t *value = [encoder constantDataAtIndex:11];
        require(value != NULL, "constant destination missing");
        *value = 0x31415926;
        const uint8_t *bytes = destination.contents;
        for (NSUInteger i = 0; i < destination.length; ++i)
            if (i < base + length || i >= base + 2 * length)
                require(bytes[i] == 0x6d, "array element encoding crossed guards");
        [encoder setBuffer:resource offset:256 atIndex:3];
        uint32_t *words = resource.contents;
        for (NSUInteger i = 0; i < resource.length / 4; ++i) words[i] = 0xa1234500u + (uint32_t)i;
        NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
            "struct Args { array<device uint *, 2> words [[id(2)]]; uint delta [[id(11)]]; };\n"
            "kernel void descriptor_encoder(device const Args &args [[buffer(3)]], uint i [[thread_position_in_grid]]) "
            "{ args.words[1][i] = (args.words[0][i] ^ args.delta) + i; }\n";
        id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
        require(library != nil, "descriptor shader compilation failed");
        id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:
            [library newFunctionWithName:@"descriptor_encoder"] error:&error];
        require(pipeline != nil, "descriptor shader pipeline failed");
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLCommandBuffer> command = [queue commandBuffer];
        id<MTLComputeCommandEncoder> compute = [command computeCommandEncoder];
        [compute setComputePipelineState:pipeline];
        [compute setBuffer:destination offset:256 atIndex:3];
        [compute useResource:resource usage:MTLResourceUsageRead | MTLResourceUsageWrite];
        [compute dispatchThreadgroups:MTLSizeMake(16, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        [compute endEncoding]; [command commit]; [command waitUntilCompleted];
        require(command.status == MTLCommandBufferStatusCompleted && command.error == nil, "descriptor GPU execution failed");
        for (NSUInteger i = 0; i < resource.length / 4; ++i) {
            uint32_t expected = 0xa1234500u + (uint32_t)i;
            if (i >= 64 && i < 80) expected = ((0xa1234500u + 9 + (uint32_t)i - 64) ^ 0x31415926u) + (uint32_t)i - 64;
            require(words[i] == expected, "descriptor GPU output or guard mismatch");
        }
        NSData *before = [NSData dataWithBytes:destination.contents length:destination.length];
        rejected(^{ [encoder setBuffer:resource offset:1025 atIndex:2]; }, "resource-offset");
        rejected(^{ [encoder setBuffer:resource offset:0 atIndex:9]; }, "missing-id");
        rejected(^{ (void)[encoder constantDataAtIndex:2]; }, "pointer-as-constant");
        rejected(^{
            id<MTLBuffer> invalidBuffers[2] = {resource, nil};
            NSUInteger invalidOffsets[2] = {0, 0};
            [encoder setBuffers:invalidBuffers offsets:invalidOffsets withRange:NSMakeRange(NSUIntegerMax, 2)];
        }, "id-overflow");
        rejected(^{ [encoder setArgumentBuffer:destination offset:destination.length]; }, "short-destination");
        rejected(^{ [encoder setArgumentBuffer:destination startOffset:NSUIntegerMax arrayElement:1]; }, "array-overflow");
        [encoder setArgumentBuffer:nil offset:0];
        rejected(^{ [encoder setBuffer:resource offset:0 atIndex:2]; }, "unbound-destination");
        require(memcmp(before.bytes, destination.contents, destination.length) == 0, "invalid input changed storage");
        printf("ARGUMENT_CONTRACT_PASS length=%lu alignment=%lu invalid=7 array_element=1 gpu_words=16\n",
               (unsigned long)length, (unsigned long)encoder.alignment);
    }
    return 0;
}
