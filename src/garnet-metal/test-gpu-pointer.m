#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdint.h>
#include <string.h>

static void require(BOOL ok, NSString *message) {
    if (!ok) {
        fprintf(stderr, "GPU_POINTER_FAIL %s\n", message.UTF8String);
        exit(1);
    }
}

int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc < 2 || argc > 3 || strcmp(argv[1], "--help") == 0) {
            puts("Usage: test-gpu-pointer BUNDLE|--reference [--pipeline-only]\n"
                 "Default dispatches GPU writes; --pipeline-only compiles without dispatch.");
            return argc == 2 ? 0 : 2;
        }
        BOOL pipelineOnly = argc == 3 && strcmp(argv[2], "--pipeline-only") == 0;
        if (argc == 3 && !pipelineOnly) return 2;
        setvbuf(stdout, NULL, _IONBF, 0);
        id<MTLDevice> device = nil;
        if (strcmp(argv[1], "--reference") == 0) device = MTLCreateSystemDefaultDevice();
        else {
            NSError *error = nil;
            NSBundle *bundle = [NSBundle bundleWithPath:@(argv[1])];
            require([bundle loadAndReturnError:&error], error.description ?: @"bundle load failed");
            Class cls = [bundle classNamed:@"GarnetDevice"];
            require(cls != Nil, @"Metal class missing");
            device = [[cls alloc] init];
        }
        require(device != nil, @"Metal device unavailable");
        NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
            "struct PointerArgs { device uint *words; uint bias; };\n"
            "kernel void gpu_pointer(device const PointerArgs &args [[buffer(3)]], "
            "device uint *observed [[buffer(7)]], uint i [[thread_position_in_grid]]) {\n"
            "  uint old = args.words[i];\n"
            "  observed[i] = old;\n"
            "  args.words[i] = (old ^ 0xa5c39e71u) + args.bias + i;\n}\n";
        const char *sourcePath = getenv("GPU_POINTER_SOURCE");
        if (sourcePath) {
            NSError *writeError = nil;
            require([source writeToFile:@(sourcePath) atomically:YES encoding:NSUTF8StringEncoding
                                 error:&writeError], writeError.description ?: @"source write failed");
        }
        NSError *error = nil;
        id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
        require(library != nil, error.description ?: @"library compilation failed");
        id<MTLFunction> function = [library newFunctionWithName:@"gpu_pointer"];
        require(function != nil, @"kernel missing");
        id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function error:&error];
        require(pipeline != nil, error.description ?: @"pipeline creation failed");
        puts("GPU_POINTER_PIPELINE_PASS buffers=3,7");
        if (pipelineOnly) return 0;

        enum { count = 64, guard = 4, bindingOffset = 256 };
        uint32_t initial[count + 2 * guard];
        for (unsigned i = 0; i < count + 2 * guard; ++i) initial[i] = 0x12345678u + 0x10203u * i;
        id<MTLBuffer> target = [device newBufferWithBytes:initial length:sizeof initial
                                                options:MTLResourceStorageModeShared];
        require(target != nil && target.gpuAddress != 0, @"target GPU address missing");
        struct { uint64_t address; uint32_t bias; uint32_t padding; } args = {
            target.gpuAddress + guard * sizeof(uint32_t), 0x31415926u, 0
        };
        id<MTLBuffer> arguments = [device newBufferWithLength:bindingOffset + sizeof args
                                                    options:MTLResourceStorageModeShared];
        id<MTLBuffer> observed = [device newBufferWithLength:bindingOffset + count * sizeof(uint32_t)
                                                   options:MTLResourceStorageModeShared];
        require(arguments != nil && observed != nil, @"bound buffer allocation failed");
        memset(arguments.contents, 0xa5, arguments.length);
        memcpy((uint8_t *)arguments.contents + bindingOffset, &args, sizeof args);
        memset(observed.contents, 0xcd, observed.length);
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLCommandBuffer> commands = [queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [commands computeCommandEncoder];
        require(encoder != nil, @"compute encoder missing");
        [encoder setComputePipelineState:pipeline];
        [encoder setBuffer:arguments offset:bindingOffset atIndex:3];
        [encoder setBuffer:observed offset:bindingOffset atIndex:7];
        [encoder useResource:target usage:MTLResourceUsageRead | MTLResourceUsageWrite];
        __weak id<MTLBuffer> weakTarget = target;
        target = nil;
        require(weakTarget != nil, @"useResource did not retain the indirect buffer before submission");
        target = weakTarget;
        [encoder dispatchThreadgroups:MTLSizeMake(count, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        [encoder endEncoding];
        [commands commit];
        [commands waitUntilCompleted];
        require(commands.status == MTLCommandBufferStatusCompleted && commands.error == nil,
                commands.error.description ?: @"command did not complete");
        const uint32_t *actual = target.contents;
        const uint32_t *reads = (const uint32_t *)((const uint8_t *)observed.contents + bindingOffset);
        for (unsigned i = 0; i < count; ++i) {
            uint32_t old = initial[i + guard];
            uint32_t expected = (old ^ 0xa5c39e71u) + args.bias + i;
            require(reads[i] == old && actual[i + guard] == expected,
                    [NSString stringWithFormat:@"word %u read=%08x expected=%08x write=%08x expected=%08x",
                     i, reads[i], old, actual[i + guard], expected]);
        }
        for (unsigned i = 0; i < guard; ++i)
            require(actual[i] == initial[i] && actual[count + guard + i] == initial[count + guard + i],
                    @"pointer write crossed guard region");
        for (unsigned i = 0; i < bindingOffset; ++i)
            require(((const uint8_t *)arguments.contents)[i] == 0xa5 &&
                    ((const uint8_t *)observed.contents)[i] == 0xcd,
                    @"bound buffer prefix was overwritten");
        printf("GPU_POINTER_PASS words=%u address=0x%llx guards=%u binding_offset=%u useResource=retained,read,write\n",
               count, (unsigned long long)args.address, guard, bindingOffset);
    }
    return 0;
}
