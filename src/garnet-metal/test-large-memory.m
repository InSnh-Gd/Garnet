// Live large-buffer acceptance test. Build with test-large-memory.sh and run
// with an explicitly selected bundle on real hardware.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <errno.h>
#include <dispatch/dispatch.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

enum {
    kDefaultIterations = 1,
    kMaxIterations = 16,
    kNoCopyImportCount = 65,
};

static const uint64_t kGuardBytes = UINT64_C(64) << 10;
static const uint64_t kMaxChunkBytes = (UINT64_C(32) << 20) - (UINT64_C(64) << 10);
static const uint64_t kNoCopyBytes = UINT64_C(64) << 20;
static const uint64_t kLargePayloadBytes =
    (UINT64_C(8) << 30) - 2 * kGuardBytes;
static const size_t kHostGuardBytes = 64 * 1024;
static const uint64_t kRetainedCacheAllowance = UINT64_C(512) << 20;
static const uint64_t kRetainedCacheGrowth = UINT64_C(64) << 20;
static const uint8_t kInitialFillByte = 0x3c;
static const uint8_t kPrefixGuardByte = 0x5a;
static const uint8_t kSuffixGuardByte = 0xa5;
static const unsigned kReadOnlyImport = 32;

typedef enum {
    MemoryModeSmall,
    MemoryModePrivate,
    MemoryModeShared,
    MemoryModeManaged,
    MemoryModeNoCopyTotal,
} MemoryMode;

typedef struct {
    MemoryMode mode;
    const char *name;
    uint64_t logicalBytes;
    MTLResourceOptions options;
    BOOL large;
    BOOL noCopy;
} MemoryModeSpec;

typedef struct {
    uint64_t byteBase;
    uint64_t wordCount;
    uint32_t salt;
    uint32_t padding;
} ChunkArgs;

typedef struct {
    uint64_t offset0;
    uint64_t offset1;
    uint64_t offset2;
    uint64_t offset3;
} ProbeArgs;

_Static_assert(sizeof(ChunkArgs) == 24, "shader ChunkArgs layout changed");
_Static_assert(sizeof(ProbeArgs) == 32, "shader ProbeArgs layout changed");

typedef struct {
    void *mapping;
    size_t mappingBytes;
    uint8_t *data;
    size_t dataBytes;
} GuardedMapping;

typedef struct {
    GuardedMapping mapping;
    void *originalPointer;
    NSUInteger originalLength;
    unsigned callbackCount;
    BOOL wrongLength;
} NoCopyRecord;

typedef struct {
    NoCopyRecord records[kNoCopyImportCount];
    uint64_t mappedBytes;
    uint64_t unmappedBytes;
    uint64_t outstandingBytes;
    uint64_t peakMappedBytes;
    unsigned callbacks;
    unsigned unknownCallbacks;
    unsigned unmapFailures;
} NoCopyAccounting;

typedef struct {
    uint64_t hostMappedBytes;
    uint64_t readbackChunks;
    uint64_t finalChunkBytes;
    uint64_t probeOffsets[4];
} BufferRunStats;

typedef struct {
    uint64_t mappedBytes;
    uint64_t unmappedBytes;
    uint64_t peakMappedBytes;
    uint64_t cumulativeImportBytes;
    unsigned callbacks;
    unsigned negativeCallbacks;
    unsigned readOnlyImports;
} NoCopyRunStats;

static void fail(NSString *message) __attribute__((noreturn));
static void fail(NSString *message) {
    fprintf(stderr, "LARGE_MEMORY_FAIL %s\n", message.UTF8String);
    fflush(stderr);
    exit(1);
}

static void require(BOOL condition, NSString *message) {
    if (!condition) fail(message);
}

static uint64_t currentAllocatedBytes(id<MTLDevice> device) {
    return (uint64_t)[device currentAllocatedSize];
}

static NSString *storageName(MTLResourceOptions options) {
    if (options == MTLResourceStorageModePrivate) return @"private";
    if (options == MTLResourceStorageModeShared) return @"shared";
    if (options == MTLResourceStorageModeManaged) return @"managed";
    return @"unknown";
}

static BOOL modeSpecForName(const char *name, MemoryModeSpec *spec) {
    if (!strcmp(name, "small")) {
        *spec = (MemoryModeSpec){ MemoryModeSmall, "small", (UINT64_C(75) << 20) + 4,
            MTLResourceStorageModeShared, NO, NO };
        return YES;
    }
    if (!strcmp(name, "private")) {
        *spec = (MemoryModeSpec){ MemoryModePrivate, "private", kLargePayloadBytes,
            MTLResourceStorageModePrivate, YES, NO };
        return YES;
    }
    if (!strcmp(name, "shared")) {
        *spec = (MemoryModeSpec){ MemoryModeShared, "shared", kLargePayloadBytes,
            MTLResourceStorageModeShared, YES, NO };
        return YES;
    }
    if (!strcmp(name, "managed")) {
        *spec = (MemoryModeSpec){ MemoryModeManaged, "managed", kLargePayloadBytes,
            MTLResourceStorageModeManaged, YES, NO };
        return YES;
    }
    if (!strcmp(name, "nocopy-total")) {
        *spec = (MemoryModeSpec){ MemoryModeNoCopyTotal, "nocopy-total",
            kNoCopyBytes * kNoCopyImportCount, MTLResourceStorageModeShared, YES, YES };
        return YES;
    }
    return NO;
}

static BOOL parseIterations(const char *text, unsigned *iterations) {
    if (!text || !text[0]) return NO;
    unsigned value = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (*p < '0' || *p > '9') return NO;
        unsigned digit = (unsigned)(*p - '0');
        if (value > (UINT_MAX - digit) / 10) return NO;
        value = value * 10 + digit;
    }
    if (value < 1 || value > kMaxIterations) return NO;
    *iterations = value;
    return YES;
}

static uint32_t patternValue(uint64_t wordIndex) {
    return (uint32_t)(UINT64_C(0x9e3779b9) * wordIndex) + UINT32_C(0x7f4a7c15);
}

static uint32_t transformWordValue(uint32_t value, uint64_t wordIndex) {
    return (value ^ UINT32_C(0xa5a55a5a)) + UINT32_C(0x31415926) +
        (uint32_t)(wordIndex & UINT64_C(0xffff));
}

static uint32_t transformedValue(uint64_t wordIndex) {
    return transformWordValue(patternValue(wordIndex), wordIndex);
}

static uint32_t expectedAfterFullTransform(uint64_t wordIndex,
                                           const uint64_t *probeWords,
                                           unsigned probeWordCount) {
    uint32_t value = patternValue(wordIndex);
    value = transformWordValue(value, wordIndex);
    for (unsigned i = 0; i < probeWordCount; ++i) {
        if (probeWords[i] == wordIndex) {
            value = transformWordValue(value, wordIndex);
            break;
        }
    }
    return value;
}

static BOOL guardedMappingCreate(size_t dataBytes, GuardedMapping *mapping) {
    memset(mapping, 0, sizeof *mapping);
    if (!dataBytes || dataBytes > SIZE_MAX - 4 * kHostGuardBytes) return NO;
    size_t total = dataBytes + 4 * kHostGuardBytes;
    void *raw = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (raw == MAP_FAILED) return NO;
    uintptr_t aligned = ((uintptr_t)raw + 2 * kHostGuardBytes - 1) &
        ~((uintptr_t)kHostGuardBytes - 1);
    size_t prefix = aligned - (uintptr_t)raw;
    if (prefix < kHostGuardBytes || total - prefix < dataBytes + kHostGuardBytes) {
        (void)munmap(raw, total);
        return NO;
    }
    uint8_t *data = (uint8_t *)aligned;
    if (mprotect(data - kHostGuardBytes, kHostGuardBytes, PROT_NONE) != 0 ||
        mprotect(data + dataBytes, kHostGuardBytes, PROT_NONE) != 0) {
        (void)munmap(raw, total);
        return NO;
    }
    mapping->mapping = raw;
    mapping->mappingBytes = total;
    mapping->data = data;
    mapping->dataBytes = dataBytes;
    return YES;
}

static BOOL guardedMappingRelease(GuardedMapping *mapping, uint64_t *unmappedBytes) {
    if (!mapping->mapping) return YES;
    if (munmap(mapping->mapping, mapping->mappingBytes) != 0) return NO;
    *unmappedBytes += (uint64_t)mapping->mappingBytes;
    memset(mapping, 0, sizeof *mapping);
    return YES;
}

static NSString *largeMemoryShaderSource(void) {
    return @"#include <metal_stdlib>\n"
        "using namespace metal;\n"
        "struct ChunkArgs { ulong byte_base; ulong word_count; uint salt; uint padding; };\n"
        "struct ProbeArgs { ulong offset0; ulong offset1; ulong offset2; ulong offset3; };\n"
        "kernel void initialize_words(device uint *data [[buffer(0)]],\n"
        "                             constant ChunkArgs &args [[buffer(1)]],\n"
        "                             uint i [[thread_position_in_grid]]) {\n"
        "    if (i < args.word_count) {\n"
        "        ulong at = (args.byte_base >> 2) + ulong(i);\n"
        "        data[at] = (uint)(0x9e3779b9UL * at) + 0x7f4a7c15u;\n"
        "    }\n"
        "}\n"
        "kernel void transform_words(device uint *data [[buffer(0)]],\n"
        "                            constant ChunkArgs &args [[buffer(1)]],\n"
        "                            uint i [[thread_position_in_grid]]) {\n"
        "    if (i < args.word_count) {\n"
        "        ulong at = (args.byte_base >> 2) + ulong(i);\n"
        "        uint old = data[at];\n"
        "        data[at] = (old ^ 0xa5a55a5au) + args.salt + (uint)(at & 0xffffUL);\n"
        "    }\n"
        "}\n"
        "kernel void probe_read_words(device const uint *data [[buffer(0)]],\n"
        "                             device uint *output [[buffer(1)]],\n"
        "                             constant ProbeArgs &args [[buffer(2)]],\n"
        "                             uint i [[thread_position_in_grid]]) {\n"
        "    if (i == 0) {\n"
        "        output[0] = data[args.offset0 >> 2];\n"
        "        output[1] = data[args.offset1 >> 2];\n"
        "        output[2] = data[args.offset2 >> 2];\n"
        "        output[3] = data[args.offset3 >> 2];\n"
        "    }\n"
        "}\n"
        "kernel void probe_rmw_words(device uint *data [[buffer(0)]],\n"
        "                            device uint *output [[buffer(1)]],\n"
        "                            constant ProbeArgs &args [[buffer(2)]],\n"
        "                            uint i [[thread_position_in_grid]]) {\n"
        "    if (i == 0) {\n"
        "        ulong offsets[4] = { args.offset0, args.offset1, args.offset2, args.offset3 };\n"
        "        for (uint j = 0; j < 4; ++j) {\n"
        "            ulong at = offsets[j] >> 2;\n"
        "            uint old = data[at];\n"
        "            data[at] = (old ^ 0xa5a55a5au) + 0x31415926u +\n"
        "                (uint)(at & 0xffffUL);\n"
        "            output[j * 2] = old;\n"
        "            output[j * 2 + 1] = data[at];\n"
        "        }\n"
        "    }\n"
        "}\n";
}

static id<MTLComputePipelineState> makePipeline(id<MTLDevice> device, id<MTLLibrary> library,
                                                NSString *name, NSError **error) {
    id<MTLFunction> function = [library newFunctionWithName:name];
    require(function != nil, [NSString stringWithFormat:@"missing shader function %@", name]);
    id<MTLComputePipelineState> pipeline =
        [device newComputePipelineStateWithFunction:function error:error];
    require(pipeline != nil, [NSString stringWithFormat:@"pipeline %@ failed: %@", name,
        *error ? *error : @"no NSError"]);
    return pipeline;
}

static void completeCommand(id<MTLCommandBuffer> command, NSString *operation) {
    require(command != nil, [operation stringByAppendingString:@" command buffer unavailable"]);
    dispatch_semaphore_t completed = dispatch_semaphore_create(0);
    __block MTLCommandBufferStatus status = (MTLCommandBufferStatus)0;
    __block NSError *completionError = nil;
    __block unsigned completionHandlerCalls = 0;
    [command addCompletedHandler:^(id<MTLCommandBuffer> finished) {
        completionHandlerCalls++;
        status = finished.status;
        completionError = finished.error;
        dispatch_semaphore_signal(completed);
    }];
    [command commit];
    const uint64_t timeoutNanoseconds = UINT64_C(90) * NSEC_PER_SEC;
    long waitResult = dispatch_semaphore_wait(completed,
        dispatch_time(DISPATCH_TIME_NOW, (int64_t)timeoutNanoseconds));
    require(waitResult == 0,
        [NSString stringWithFormat:@"%@ timed out after 90 seconds", operation]);
    require(completionHandlerCalls == 1,
        [NSString stringWithFormat:@"%@ completed handler ran %u times", operation,
            completionHandlerCalls]);
    require(status == MTLCommandBufferStatusCompleted && completionError == nil,
        [NSString stringWithFormat:@"%@ status=%lu error=%@", operation,
            (unsigned long)status, completionError ?: @"none"]);
}

static uint64_t nextChunkBytes(uint64_t remaining) {
    return remaining < kMaxChunkBytes ? remaining : kMaxChunkBytes;
}

static void dispatchWords(id<MTLComputeCommandEncoder> encoder, uint64_t words) {
    require(words > 0 && words <= UINT32_MAX, @"word dispatch is outside the Metal grid range");
    [encoder dispatchThreads:MTLSizeMake((NSUInteger)words, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
}

static void initializeGuardsCPU(id<MTLBuffer> buffer, uint64_t dataOffset,
                                uint64_t logicalBytes, MTLResourceOptions options) {
    uint8_t *base = buffer.contents;
    require(base != NULL, @"CPU-visible buffer has no contents");
    memset(base, kPrefixGuardByte, (size_t)dataOffset);
    memset(base + dataOffset + logicalBytes, kSuffixGuardByte, (size_t)kGuardBytes);
    if (options == MTLResourceStorageModeManaged) {
        [buffer didModifyRange:NSMakeRange(0, (NSUInteger)dataOffset)];
        [buffer didModifyRange:NSMakeRange((NSUInteger)(dataOffset + logicalBytes),
                                          (NSUInteger)kGuardBytes)];
    }
}

static void initializeLogicalCPU(id<MTLBuffer> buffer, uint64_t dataOffset,
                                 uint64_t logicalBytes, MTLResourceOptions options) {
    uint8_t *base = buffer.contents;
    require(base != NULL, @"CPU-visible buffer has no contents");
    for (uint64_t offset = 0; offset < logicalBytes;) {
        uint64_t size = nextChunkBytes(logicalBytes - offset);
        require(size % sizeof(uint32_t) == 0, @"logical chunk is not word aligned");
        uint32_t *words = (uint32_t *)(base + dataOffset + offset);
        uint64_t firstWord = offset / sizeof(uint32_t);
        uint64_t wordCount = size / sizeof(uint32_t);
        for (uint64_t i = 0; i < wordCount; ++i)
            words[i] = patternValue(firstWord + i);
        if (options == MTLResourceStorageModeManaged)
            [buffer didModifyRange:NSMakeRange((NSUInteger)(dataOffset + offset),
                                              (NSUInteger)size)];
        offset += size;
    }
}

static void fillPrivateInitial(id<MTLBuffer> buffer, uint64_t dataOffset,
                               uint64_t logicalBytes, id<MTLCommandQueue> queue) {
    @autoreleasepool {
        id<MTLCommandBuffer> command = [queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
        require(command != nil && blit != nil, @"private initialization encoder unavailable");
        [blit fillBuffer:buffer range:NSMakeRange(0, (NSUInteger)dataOffset)
                   value:kPrefixGuardByte];
        for (uint64_t offset = 0; offset < logicalBytes;) {
            uint64_t size = nextChunkBytes(logicalBytes - offset);
            [blit fillBuffer:buffer
                       range:NSMakeRange((NSUInteger)(dataOffset + offset), (NSUInteger)size)
                       value:kInitialFillByte];
            offset += size;
        }
        [blit fillBuffer:buffer
                   range:NSMakeRange((NSUInteger)(dataOffset + logicalBytes),
                                     (NSUInteger)kGuardBytes)
                   value:kSuffixGuardByte];
        [blit endEncoding];
        completeCommand(command, @"private initial fill");
    }
}

static void fillLogicalGPU(id<MTLBuffer> buffer, uint64_t dataOffset,
                           uint64_t logicalBytes, id<MTLCommandQueue> queue) {
    @autoreleasepool {
        id<MTLCommandBuffer> command = [queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
        require(command != nil && blit != nil, @"GPU fill encoder unavailable");
        for (uint64_t offset = 0; offset < logicalBytes;) {
            uint64_t size = nextChunkBytes(logicalBytes - offset);
            [blit fillBuffer:buffer
                       range:NSMakeRange((NSUInteger)(dataOffset + offset), (NSUInteger)size)
                       value:kInitialFillByte];
            offset += size;
        }
        [blit endEncoding];
        completeCommand(command, @"GPU logical fill");
    }
}

static void initializeLogicalGPU(id<MTLBuffer> buffer, uint64_t dataOffset,
                                 uint64_t logicalBytes, id<MTLCommandQueue> queue,
                                 id<MTLComputePipelineState> pipeline) {
    @autoreleasepool {
        id<MTLCommandBuffer> command = [queue commandBuffer];
        id<MTLComputeCommandEncoder> compute = [command computeCommandEncoder];
        require(command != nil && compute != nil, @"private pattern encoder unavailable");
        [compute setComputePipelineState:pipeline];
        [compute setBuffer:buffer offset:(NSUInteger)dataOffset atIndex:0];
        for (uint64_t offset = 0; offset < logicalBytes;) {
            uint64_t size = nextChunkBytes(logicalBytes - offset);
            require(size % sizeof(uint32_t) == 0, @"private logical chunk is not word aligned");
            ChunkArgs args = {
                .byteBase = offset,
                .wordCount = size / sizeof(uint32_t),
                .salt = 0,
                .padding = 0,
            };
            [compute setBytes:&args length:sizeof args atIndex:1];
            dispatchWords(compute, args.wordCount);
            offset += size;
        }
        [compute endEncoding];
        completeCommand(command, @"private pattern initialization");
    }
}

static void transformLogical(id<MTLBuffer> buffer, uint64_t dataOffset,
                             uint64_t logicalBytes, id<MTLCommandQueue> queue,
                             id<MTLComputePipelineState> pipeline) {
    @autoreleasepool {
        id<MTLCommandBuffer> command = [queue commandBuffer];
        id<MTLComputeCommandEncoder> compute = [command computeCommandEncoder];
        require(command != nil && compute != nil, @"transform encoder unavailable");
        [compute setComputePipelineState:pipeline];
        [compute setBuffer:buffer offset:(NSUInteger)dataOffset atIndex:0];
        for (uint64_t offset = 0; offset < logicalBytes;) {
            uint64_t size = nextChunkBytes(logicalBytes - offset);
            require(size % sizeof(uint32_t) == 0, @"transform chunk is not word aligned");
            ChunkArgs args = {
                .byteBase = offset,
                .wordCount = size / sizeof(uint32_t),
                .salt = UINT32_C(0x31415926),
                .padding = 0,
            };
            [compute setBytes:&args length:sizeof args atIndex:1];
            dispatchWords(compute, args.wordCount);
            offset += size;
        }
        [compute endEncoding];
        completeCommand(command, @"full-range transform");
    }
}

static void probeReadWords(id<MTLCommandQueue> queue,
                           id<MTLComputePipelineState> pipeline, id<MTLBuffer> input,
                           NSUInteger bindingOffset, id<MTLBuffer> output,
                           const uint64_t offsets[4], uint32_t values[4]) {
    require(input.gpuAddress != 0, @"probe input has no GPU address");
    require(output.contents != NULL && output.length >= 4 * sizeof(uint32_t),
            @"probe output is not CPU visible");
    require(bindingOffset <= input.length, @"probe binding offset is outside the input buffer");
    for (unsigned i = 0; i < 4; ++i)
        require((offsets[i] & 3) == 0 &&
                offsets[i] <= input.length - bindingOffset - sizeof(uint32_t),
                @"probe offset is outside the input buffer");
    @autoreleasepool {
        memset(output.contents, 0xee, 4 * sizeof(uint32_t));
        id<MTLCommandBuffer> command = [queue commandBuffer];
        id<MTLComputeCommandEncoder> compute = [command computeCommandEncoder];
        require(command != nil && compute != nil, @"probe encoder unavailable");
        ProbeArgs args = { offsets[0], offsets[1], offsets[2], offsets[3] };
        [compute setComputePipelineState:pipeline];
        [compute setBuffer:input offset:bindingOffset atIndex:0];
        [compute setBuffer:output offset:0 atIndex:1];
        [compute setBytes:&args length:sizeof args atIndex:2];
        [compute dispatchThreads:MTLSizeMake(1, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        [compute endEncoding];
        completeCommand(command, @"single-invocation read probe");
        memcpy(values, output.contents, 4 * sizeof(uint32_t));
    }
}

static void probeRMWWords(id<MTLCommandQueue> queue,
                          id<MTLComputePipelineState> pipeline, id<MTLBuffer> input,
                          NSUInteger bindingOffset, id<MTLBuffer> output,
                          const uint64_t offsets[4], uint32_t values[8]) {
    require(input.gpuAddress != 0, @"RMW probe input has no GPU address");
    require(output.contents != NULL && output.length >= 8 * sizeof(uint32_t),
            @"RMW probe output is not CPU visible");
    require(bindingOffset <= input.length, @"RMW probe binding offset is outside the input buffer");
    for (unsigned i = 0; i < 4; ++i)
        require((offsets[i] & 3) == 0 &&
                offsets[i] <= input.length - bindingOffset - sizeof(uint32_t),
                @"RMW probe offset is outside the input buffer");
    @autoreleasepool {
        memset(output.contents, 0xee, 8 * sizeof(uint32_t));
        id<MTLCommandBuffer> command = [queue commandBuffer];
        id<MTLComputeCommandEncoder> compute = [command computeCommandEncoder];
        require(command != nil && compute != nil, @"RMW probe encoder unavailable");
        ProbeArgs args = { offsets[0], offsets[1], offsets[2], offsets[3] };
        [compute setComputePipelineState:pipeline];
        [compute setBuffer:input offset:bindingOffset atIndex:0];
        [compute setBuffer:output offset:0 atIndex:1];
        [compute setBytes:&args length:sizeof args atIndex:2];
        [compute dispatchThreads:MTLSizeMake(1, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        [compute endEncoding];
        completeCommand(command, @"single-invocation RMW probe");
        memcpy(values, output.contents, 8 * sizeof(uint32_t));
    }
}

static void verifyExpectedWords(const uint32_t *actual, uint64_t firstWord,
                                uint64_t wordCount, const uint64_t *probeWords,
                                unsigned probeWordCount, NSString *where) {
    for (uint64_t i = 0; i < wordCount; ++i) {
        uint64_t wordIndex = firstWord + i;
        uint32_t expected = expectedAfterFullTransform(wordIndex, probeWords,
                                                       probeWordCount);
        if (actual[i] != expected) {
            fail([NSString stringWithFormat:@"%@ word=%llu got=0x%08x expected=0x%08x",
                where, (unsigned long long)wordIndex, actual[i], expected]);
        }
    }
}

static void verifyLogicalByCopy(id<MTLBuffer> buffer, uint64_t dataOffset,
                                uint64_t logicalBytes, id<MTLBuffer> staging,
                                id<MTLCommandQueue> queue, BOOL cpuVisible,
                                const uint64_t *probeWords, unsigned probeWordCount,
                                BufferRunStats *stats) {
    require(staging.contents != NULL && staging.length >= kMaxChunkBytes,
            @"staging buffer is too small or not CPU visible");
    uint64_t chunks = 0;
    for (uint64_t offset = 0; offset < logicalBytes;) {
        uint64_t size = nextChunkBytes(logicalBytes - offset);
        @autoreleasepool {
            id<MTLCommandBuffer> command = [queue commandBuffer];
            id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
            require(command != nil && blit != nil, @"readback blit encoder unavailable");
            [blit copyFromBuffer:buffer sourceOffset:(NSUInteger)(dataOffset + offset)
                        toBuffer:staging destinationOffset:0 size:(NSUInteger)size];
            [blit endEncoding];
            completeCommand(command, @"logical readback");
        }
        uint32_t *actual = staging.contents;
        verifyExpectedWords(actual, offset / sizeof(uint32_t), size / sizeof(uint32_t),
                            probeWords, probeWordCount, @"readback");
        if (cpuVisible) {
            uint8_t *direct = (uint8_t *)buffer.contents + dataOffset + offset;
            if (memcmp(direct, staging.contents, (size_t)size) != 0) {
                fail([NSString stringWithFormat:
                    @"CPU visibility differs at logical offset %llu",
                    (unsigned long long)offset]);
            }
        }
        if (offset + size == logicalBytes) {
            uint64_t lastWord = logicalBytes / sizeof(uint32_t) - 1;
            uint32_t expected = expectedAfterFullTransform(lastWord, probeWords,
                                                           probeWordCount);
            uint32_t actualLast = ((uint32_t *)staging.contents)[size / sizeof(uint32_t) - 1];
            require(actualLast == expected, @"final logical word mismatch");
            if (cpuVisible) {
                uint32_t directLast = ((uint32_t *)((uint8_t *)buffer.contents +
                    dataOffset + logicalBytes - sizeof(uint32_t)))[0];
                require(directLast == expected, @"final CPU-visible word mismatch");
            }
            stats->finalChunkBytes = size;
        }
        chunks++;
        offset += size;
    }
    require(stats->finalChunkBytes < kMaxChunkBytes,
            @"final readback chunk was not a short chunk");
    stats->readbackChunks = chunks;
}

static void verifyGuardsCPU(id<MTLBuffer> buffer, uint64_t dataOffset,
                            uint64_t logicalBytes) {
    uint8_t *base = buffer.contents;
    require(base != NULL, @"guard verification needs CPU-visible storage");
    for (uint64_t i = 0; i < dataOffset; ++i)
        require(base[i] == kPrefixGuardByte, @"prefix guard changed");
    for (uint64_t i = 0; i < kGuardBytes; ++i)
        require(base[dataOffset + logicalBytes + i] == kSuffixGuardByte,
                @"suffix guard changed");
}

static void verifyGuardsGPU(id<MTLBuffer> buffer, uint64_t dataOffset,
                            uint64_t logicalBytes, id<MTLBuffer> staging,
                            id<MTLCommandQueue> queue) {
    require(staging.length >= 2 * kGuardBytes, @"guard staging buffer is too small");
    @autoreleasepool {
        id<MTLCommandBuffer> command = [queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
        require(command != nil && blit != nil, @"guard readback encoder unavailable");
        [blit copyFromBuffer:buffer sourceOffset:0 toBuffer:staging destinationOffset:0
                        size:(NSUInteger)kGuardBytes];
        [blit copyFromBuffer:buffer sourceOffset:(NSUInteger)(dataOffset + logicalBytes)
                    toBuffer:staging destinationOffset:(NSUInteger)kGuardBytes
                        size:(NSUInteger)kGuardBytes];
        [blit endEncoding];
        completeCommand(command, @"guard readback");
    }
    const uint8_t *prefix = staging.contents;
    const uint8_t *suffix = prefix + kGuardBytes;
    for (uint64_t i = 0; i < kGuardBytes; ++i)
        require(prefix[i] == kPrefixGuardByte && suffix[i] == kSuffixGuardByte,
                @"GPU-visible guard sentinel changed");
}

static void verifyGuards(id<MTLBuffer> buffer, uint64_t dataOffset,
                         uint64_t logicalBytes, id<MTLBuffer> staging,
                         id<MTLCommandQueue> queue) {
    if (buffer.contents) verifyGuardsCPU(buffer, dataOffset, logicalBytes);
    else verifyGuardsGPU(buffer, dataOffset, logicalBytes, staging, queue);
}

static void verifySmallSeamOperations(id<MTLBuffer> buffer, uint64_t dataOffset,
                                      uint64_t logicalBytes, id<MTLBuffer> staging,
                                      id<MTLCommandQueue> queue) {
    const uint64_t seam = (UINT64_C(64) << 20) - dataOffset;
    require(logicalBytes >= seam + sizeof(uint32_t), @"small mode does not cross 64 MiB");
    @autoreleasepool {
        id<MTLCommandBuffer> command = [queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
        require(command != nil && blit != nil, @"seam copy encoder unavailable");
        [blit copyFromBuffer:buffer sourceOffset:(NSUInteger)(dataOffset + seam - 4)
                    toBuffer:staging destinationOffset:0 size:8];
        [blit endEncoding];
        completeCommand(command, @"cross-seam blit copy");
    }
    uint32_t *copied = staging.contents;
    uint64_t seamWord = seam / sizeof(uint32_t);
    require(copied[0] == transformedValue(seamWord - 1) &&
            copied[1] == transformedValue(seamWord),
            @"cross-seam blit copy oracle mismatch");
    @autoreleasepool {
        id<MTLCommandBuffer> command = [queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
        require(command != nil && blit != nil, @"seam fill encoder unavailable");
        [blit fillBuffer:buffer range:NSMakeRange((NSUInteger)(dataOffset + seam - 4), 8)
                   value:0x6d];
        [blit endEncoding];
        completeCommand(command, @"cross-seam blit fill");
    }
    const uint8_t *direct = (const uint8_t *)buffer.contents + dataOffset + seam - 4;
    for (unsigned i = 0; i < 8; ++i)
        require(direct[i] == 0x6d, @"cross-seam fill did not cover both sides");
}

static BufferRunStats runBufferModeIteration(id<MTLDevice> device,
                                             const MemoryModeSpec *spec,
                                             unsigned iteration,
                                             id<MTLComputePipelineState> initPipeline,
                                             id<MTLComputePipelineState> transformPipeline,
                                             id<MTLComputePipelineState> probeRMWPipeline) {
    BufferRunStats stats;
    memset(&stats, 0, sizeof stats);
    uint64_t dataOffset = kGuardBytes;
    uint64_t totalBytes = dataOffset + spec->logicalBytes + kGuardBytes;
    uint64_t maxBufferLength = (uint64_t)[device maxBufferLength];
    require(maxBufferLength >= totalBytes,
        [NSString stringWithFormat:@"maxBufferLength %llu < requested buffer %llu",
            (unsigned long long)maxBufferLength, (unsigned long long)totalBytes]);
    uint64_t before = currentAllocatedBytes(device);
    @autoreleasepool {
        id<MTLCommandQueue> queue = [device newCommandQueue];
        require(queue != nil, @"command queue unavailable");
        id<MTLBuffer> buffer = [device newBufferWithLength:(NSUInteger)totalBytes
                                                   options:spec->options];
        require(buffer != nil, [NSString stringWithFormat:@"%@ buffer allocation failed",
            storageName(spec->options)]);
        require(buffer.length == totalBytes, @"buffer allocation lost logical backing");
        require(buffer.storageMode == (MTLStorageMode)(spec->options >> 4),
                @"buffer allocation changed the storage mode");
        require(buffer.gpuAddress != 0, @"64-bit GPU address unavailable");
        require(buffer.gpuAddress <= UINT64_MAX - totalBytes,
                @"buffer GPU address range overflows 64 bits");
        uint64_t afterAllocation = currentAllocatedBytes(device);
        require(afterAllocation >= before && afterAllocation - before >= totalBytes,
                @"currentAllocatedSize omitted owned buffer backing");
        uint64_t hostMappedBytes = buffer.contents ? totalBytes : 0;
        stats.hostMappedBytes = hostMappedBytes;
        id<MTLBuffer> staging = [device newBufferWithLength:(NSUInteger)kMaxChunkBytes
                                                    options:MTLResourceStorageModeShared];
        require(staging != nil, @"readback staging allocation failed");
        id<MTLBuffer> probeOutput = [device newBufferWithLength:64
                                                        options:MTLResourceStorageModeShared];
        require(probeOutput != nil, @"probe output allocation failed");
        uint64_t afterSetup = currentAllocatedBytes(device);

        if (spec->options == MTLResourceStorageModePrivate) {
            fillPrivateInitial(buffer, dataOffset, spec->logicalBytes, queue);
            verifyGuards(buffer, dataOffset, spec->logicalBytes, staging, queue);
        } else {
            initializeGuardsCPU(buffer, dataOffset, spec->logicalBytes, spec->options);
            verifyGuards(buffer, dataOffset, spec->logicalBytes, staging, queue);
            if (spec->large)
                fillLogicalGPU(buffer, dataOffset, spec->logicalBytes, queue);
        }

        if (spec->options == MTLResourceStorageModePrivate) {
            initializeLogicalGPU(buffer, dataOffset, spec->logicalBytes, queue, initPipeline);
            verifyGuards(buffer, dataOffset, spec->logicalBytes, staging, queue);
        } else {
            initializeLogicalCPU(buffer, dataOffset, spec->logicalBytes, spec->options);
        }

        uint64_t mutatedWords[4] = {0, 0, 0, 0};
        unsigned mutatedWordCount = 0;
        if (spec->large) {
            uint64_t probeLogicalOffsets[4] = {
                (UINT64_C(4) << 30) - dataOffset - 4,
                (UINT64_C(4) << 30) - dataOffset,
                spec->logicalBytes - 4,
                0,
            };
            for (unsigned i = 0; i < 4; ++i)
                stats.probeOffsets[i] = dataOffset + probeLogicalOffsets[i];
            uint32_t values[8] = {0, 0, 0, 0, 0, 0, 0, 0};
            probeRMWWords(queue, probeRMWPipeline, buffer, (NSUInteger)dataOffset,
                          probeOutput, probeLogicalOffsets, values);
            for (unsigned i = 0; i < 4; ++i) {
                uint64_t wordIndex = probeLogicalOffsets[i] / sizeof(uint32_t);
                uint32_t oldExpected = patternValue(wordIndex);
                uint32_t newExpected = transformedValue(wordIndex);
                if (values[i * 2] != oldExpected ||
                    values[i * 2 + 1] != newExpected) {
                    fail([NSString stringWithFormat:
                        @"RMW probe %u got old=0x%08x new=0x%08x "
                        "expected old=0x%08x new=0x%08x", i, values[i * 2],
                        values[i * 2 + 1], oldExpected, newExpected]);
                }
                mutatedWords[mutatedWordCount++] = wordIndex;
            }
        }
        transformLogical(buffer, dataOffset, spec->logicalBytes, queue, transformPipeline);
        verifyLogicalByCopy(buffer, dataOffset, spec->logicalBytes, staging, queue,
                            buffer.contents != NULL, mutatedWords, mutatedWordCount, &stats);
        if (!spec->large)
            verifySmallSeamOperations(buffer, dataOffset, spec->logicalBytes, staging, queue);
        verifyGuards(buffer, dataOffset, spec->logicalBytes, staging, queue);

        printf("LARGE_MEMORY_ITERATION_PASS mode=%s iteration=%u storage=%s payload_bytes=%llu "
               "guard_prefix_bytes=%llu guard_suffix_bytes=%llu buffer_bytes=%llu "
               "host_mapped_bytes=%llu max_buffer_length=%llu "
               "gpu_address=0x%llx current_allocated_before=%llu "
               "current_allocated_after_allocation=%llu current_allocated_after_setup=%llu "
               "staging_bytes=%llu readback_chunks=%llu "
               "final_chunk_bytes=%llu runtime_ulong=1 probe_invocations=%u "
               "gpu_execution=1\n",
            spec->name, iteration, storageName(spec->options).UTF8String,
            (unsigned long long)spec->logicalBytes, (unsigned long long)dataOffset,
            (unsigned long long)kGuardBytes, (unsigned long long)totalBytes,
            (unsigned long long)hostMappedBytes, (unsigned long long)maxBufferLength,
            (unsigned long long)buffer.gpuAddress, (unsigned long long)before,
            (unsigned long long)afterAllocation, (unsigned long long)afterSetup,
            (unsigned long long)kMaxChunkBytes, (unsigned long long)stats.readbackChunks,
            (unsigned long long)stats.finalChunkBytes, spec->large ? 1u : 0u);
        if (spec->large) {
            printf("LARGE_MEMORY_PROBE_PASS mode=%s offsets=%llu,%llu,%llu,%llu "
                   "invocations=1 reads=4 writes=4 same_invocation_rmw=1 runtime_ulong=1 "
                   "storage_modes=private,shared,managed\n",
                spec->name, (unsigned long long)stats.probeOffsets[0],
                (unsigned long long)stats.probeOffsets[1],
                (unsigned long long)stats.probeOffsets[2],
                (unsigned long long)stats.probeOffsets[3]);
        }
        buffer = nil;
        staging = nil;
        probeOutput = nil;
        queue = nil;
    }
    return stats;
}

static NoCopyRunStats runNoCopyIteration(id<MTLDevice> device,
                                         unsigned iteration,
                                         id<MTLComputePipelineState> probeReadPipeline) {
    NoCopyRunStats stats;
    memset(&stats, 0, sizeof stats);
    uint64_t before = currentAllocatedBytes(device);
    @autoreleasepool {
        id<MTLCommandQueue> queue = [device newCommandQueue];
        require(queue != nil, @"NoCopy command queue unavailable");
        id<MTLBuffer> output = [device newBufferWithLength:64
                                                   options:MTLResourceStorageModeShared];
        require(output != nil && output.contents != NULL, @"NoCopy output unavailable");
        dispatch_semaphore_t callbacksDone = dispatch_semaphore_create(0);
        require(callbacksDone != NULL, @"NoCopy callback semaphore unavailable");
        NSLock *accountingLock = [[NSLock alloc] init];
        require(accountingLock != nil, @"NoCopy callback accounting lock unavailable");
        NSMutableArray *imports = [NSMutableArray arrayWithCapacity:kNoCopyImportCount];
        require(imports != nil, @"NoCopy import holder unavailable");
        __block NoCopyAccounting accounting;
        memset(&accounting, 0, sizeof accounting);

        for (unsigned importIndex = 0; importIndex < kNoCopyImportCount; ++importIndex) {
            GuardedMapping mapping;
            require(guardedMappingCreate((size_t)kNoCopyBytes, &mapping),
                    @"NoCopy guarded host mapping failed");
            accounting.mappedBytes += (uint64_t)mapping.mappingBytes;
            accounting.outstandingBytes += (uint64_t)mapping.mappingBytes;
            if (accounting.outstandingBytes > accounting.peakMappedBytes)
                accounting.peakMappedBytes = accounting.outstandingBytes;
            NoCopyRecord *record = &accounting.records[importIndex];
            record->mapping = mapping;
            record->originalPointer = mapping.data;
            record->originalLength = (NSUInteger)kNoCopyBytes;

            uint64_t offsets[4] = {
                0,
                kNoCopyBytes / 2,
                kNoCopyBytes - 4,
                16 * 1024,
            };
            uint64_t baseWord = (uint64_t)importIndex * (kNoCopyBytes / sizeof(uint32_t));
            uint32_t *hostWords = (uint32_t *)mapping.data;
            for (unsigned i = 0; i < 4; ++i) {
                uint64_t word = offsets[i] / sizeof(uint32_t);
                hostWords[word] = patternValue(baseWord + word);
            }
            BOOL readOnly = importIndex == kReadOnlyImport;
            if (readOnly) {
                require(mprotect(mapping.data, (size_t)kNoCopyBytes, PROT_READ) == 0,
                        @"NoCopy PROT_READ transition failed");
                stats.readOnlyImports++;
            }

            id<MTLBuffer> imported = [device newBufferWithBytesNoCopy:mapping.data
                length:(NSUInteger)kNoCopyBytes options:MTLResourceStorageModeShared
                deallocator:^(void *pointer, NSUInteger length) {
                    [accountingLock lock];
                    unsigned which = kNoCopyImportCount;
                    for (unsigned i = 0; i < kNoCopyImportCount; ++i) {
                        if (accounting.records[i].originalPointer == pointer) {
                            which = i;
                            break;
                        }
                    }
                    if (which == kNoCopyImportCount) {
                        accounting.unknownCallbacks++;
                    } else {
                        NoCopyRecord *released = &accounting.records[which];
                        released->callbackCount++;
                        if (length != released->originalLength) released->wrongLength = YES;
                        accounting.callbacks++;
                        if (released->callbackCount == 1) {
                            uint64_t mappingBytes = (uint64_t)released->mapping.mappingBytes;
                            if (guardedMappingRelease(&released->mapping,
                                                      &accounting.unmappedBytes))
                                accounting.outstandingBytes -= mappingBytes;
                            else
                                accounting.unmapFailures++;
                        }
                    }
                    [accountingLock unlock];
                    dispatch_semaphore_signal(callbacksDone);
                }];
            if (!imported) {
                uint64_t ignored = 0;
                (void)guardedMappingRelease(&record->mapping, &ignored);
                fail([NSString stringWithFormat:@"NoCopy import %u was refused", importIndex]);
            }
            require(imported.contents == mapping.data, @"NoCopy import changed the host pointer");
            require(imported.length == kNoCopyBytes, @"NoCopy import changed the host length");
            require(imported.storageMode == MTLStorageModeShared,
                    @"NoCopy import changed the storage mode");
            require(imported.gpuAddress != 0, @"NoCopy import has no GPU address");
            require(imported.gpuAddress <= UINT64_MAX - kNoCopyBytes,
                    @"NoCopy GPU address range overflows 64 bits");
            [imports addObject:imported];
            stats.cumulativeImportBytes += kNoCopyBytes;
        }

        require(imports.count == kNoCopyImportCount,
                @"NoCopy did not retain every live import");
        require(accounting.callbacks == 0,
                @"NoCopy callback ran while the import set was live");
        require(stats.cumulativeImportBytes > (UINT64_C(4) << 30),
                @"NoCopy logical import total did not exceed 4 GiB");
        require(accounting.outstandingBytes > (UINT64_C(4) << 30),
                @"NoCopy mapped host total did not exceed 4 GiB");
        printf("LARGE_MEMORY_NOCOPY_RESIDENT_PASS iteration=%u live_imports=%lu "
               "live_import_bytes=%llu live_mapped_bytes=%llu older_limits_exceeded=1\n",
            iteration, (unsigned long)imports.count,
            (unsigned long long)stats.cumulativeImportBytes,
            (unsigned long long)accounting.outstandingBytes);

        for (unsigned importIndex = 0; importIndex < kNoCopyImportCount; ++importIndex) {
            id<MTLBuffer> imported = imports[importIndex];
            uint64_t offsets[4] = {
                0,
                kNoCopyBytes / 2,
                kNoCopyBytes - 4,
                16 * 1024,
            };
            uint64_t baseWord = (uint64_t)importIndex * (kNoCopyBytes / sizeof(uint32_t));
            uint32_t expected[4];
            for (unsigned i = 0; i < 4; ++i) {
                uint64_t word = offsets[i] / sizeof(uint32_t);
                expected[i] = patternValue(baseWord + word);
            }
            uint32_t values[4] = {0, 0, 0, 0};
            probeReadWords(queue, probeReadPipeline, imported, 0, output, offsets, values);
            for (unsigned i = 0; i < 4; ++i) {
                if (values[i] != expected[i]) {
                    fail([NSString stringWithFormat:
                        @"NoCopy import %u read probe %u got=0x%08x expected=0x%08x",
                        importIndex, i, values[i], expected[i]]);
                }
            }
        }
        require(accounting.callbacks == 0,
                @"NoCopy callback ran before the retained import set was released");

        int hostPageSize = getpagesize();
        require(hostPageSize > 0, @"host page size is unavailable");
        size_t negativeDataBytes = (size_t)kNoCopyBytes + (size_t)hostPageSize;
        GuardedMapping negative;
        require(guardedMappingCreate(negativeDataBytes, &negative),
                @"NoCopy negative guarded mapping failed");
        accounting.mappedBytes += (uint64_t)negative.mappingBytes;
        accounting.outstandingBytes += (uint64_t)negative.mappingBytes;
        if (accounting.outstandingBytes > accounting.peakMappedBytes)
            accounting.peakMappedBytes = accounting.outstandingBytes;
        __block unsigned negativeCallbacks = 0;
        id<MTLBuffer> rejected = [device newBufferWithBytesNoCopy:negative.data
            length:(NSUInteger)negativeDataBytes
            options:MTLResourceStorageModeShared
            deallocator:^(void *pointer, NSUInteger length) {
                (void)pointer;
                (void)length;
                negativeCallbacks++;
            }];
        require(rejected == nil, @"64 MiB + page NoCopy request was accepted");
        require(negativeCallbacks == 0, @"refused NoCopy request invoked its deallocator");
        uint64_t negativeBytes = (uint64_t)negative.mappingBytes;
        require(guardedMappingRelease(&negative, &accounting.unmappedBytes),
                @"NoCopy negative mapping unmap failed");
        accounting.outstandingBytes -= negativeBytes;
        require(accounting.outstandingBytes > (UINT64_C(4) << 30),
                @"NoCopy imports were released before the simultaneous-total check");

        output = nil;
        queue = nil;
        [imports removeAllObjects];
        imports = nil;

        dispatch_time_t callbackDeadline = dispatch_time(DISPATCH_TIME_NOW,
            (int64_t)UINT64_C(5) * NSEC_PER_SEC);
        BOOL callbacksDrained = YES;
        for (unsigned i = 0; i < kNoCopyImportCount; ++i) {
            if (dispatch_semaphore_wait(callbacksDone, callbackDeadline) != 0) {
                callbacksDrained = NO;
                break;
            }
        }
        require(callbacksDrained,
            @"NoCopy callbacks did not reach the import count within 5 seconds");

        require(accounting.callbacks == kNoCopyImportCount,
            @"NoCopy callback count did not equal the import count");
        require(accounting.unknownCallbacks == 0, @"NoCopy callback received another pointer");
        require(accounting.unmapFailures == 0, @"NoCopy callback could not unmap its range");
        require(accounting.mappedBytes == accounting.unmappedBytes,
            @"NoCopy host mappings remain after release");
        require(accounting.outstandingBytes == 0,
            @"NoCopy outstanding host-byte counter is nonzero");
        require(negativeCallbacks == 0, @"refused NoCopy callback count changed");
        for (unsigned i = 0; i < kNoCopyImportCount; ++i) {
            NoCopyRecord *record = &accounting.records[i];
            if (record->callbackCount != 1 || record->originalPointer == NULL) {
                fail([NSString stringWithFormat:
                    @"NoCopy import %u callback count is not one", i]);
            }
            if (record->wrongLength != NO) {
                fail([NSString stringWithFormat:
                    @"NoCopy import %u callback reported the wrong length", i]);
            }
        }
        require(accounting.peakMappedBytes > (UINT64_C(4) << 30) &&
                accounting.mappedBytes > stats.cumulativeImportBytes,
            @"NoCopy host mapping accounting omitted a range");

        stats.mappedBytes = accounting.mappedBytes;
        stats.unmappedBytes = accounting.unmappedBytes;
        stats.peakMappedBytes = accounting.peakMappedBytes;
        stats.callbacks = accounting.callbacks;
        stats.negativeCallbacks = negativeCallbacks;
        printf("LARGE_MEMORY_NOCOPY_ITERATION_PASS mode=nocopy-total iteration=%u imports=%u "
               "cumulative_import_bytes=%llu readonly_imports=%u callbacks=%u "
               "negative_request=rejected negative_callbacks=%u host_mapped_bytes=%llu "
               "host_unmapped_bytes=%llu peak_host_mapped_bytes=%llu "
               "outstanding_host_bytes=0 gpu_execution=1 "
               "current_allocated_before=%llu\n",
            iteration, kNoCopyImportCount, (unsigned long long)stats.cumulativeImportBytes,
            stats.readOnlyImports, stats.callbacks, stats.negativeCallbacks,
            (unsigned long long)stats.mappedBytes, (unsigned long long)stats.unmappedBytes,
            (unsigned long long)stats.peakMappedBytes,
            (unsigned long long)before);
    }
    return stats;
}

static void checkRetainedAllocation(id<MTLDevice> device, uint64_t baseline,
                                    uint64_t firstAfter, unsigned iteration) {
    uint64_t after = currentAllocatedBytes(device);
    require(baseline <= UINT64_MAX - kRetainedCacheAllowance &&
            after <= baseline + kRetainedCacheAllowance,
        [NSString stringWithFormat:@"retained allocation %llu exceeds bound %llu above baseline",
            (unsigned long long)after, (unsigned long long)kRetainedCacheAllowance]);
    if (iteration > 1) {
        require(firstAfter <= UINT64_MAX - kRetainedCacheGrowth &&
                after <= firstAfter + kRetainedCacheGrowth,
            [NSString stringWithFormat:@"retained allocation grew from %llu to %llu after cycle one",
                (unsigned long long)firstAfter, (unsigned long long)after]);
    }
    printf("LARGE_MEMORY_RETAINED mode_iteration=%u current_allocated_after_release=%llu "
           "baseline=%llu cache_bound_bytes=%llu cache_growth_bound_bytes=%llu\n",
        iteration, (unsigned long long)after, (unsigned long long)baseline,
        (unsigned long long)kRetainedCacheAllowance,
        (unsigned long long)kRetainedCacheGrowth);
}

int main(int argc, const char **argv) {
    @autoreleasepool {
        setvbuf(stdout, NULL, _IONBF, 0);
        if (argc == 2 && !strcmp(argv[1], "--help")) {
            puts("Usage: test-large-memory BUNDLE MODE [iterations]\n"
                 "Modes: small, private, shared, managed, nocopy-total\n"
                 "small checks both Shared and Managed storage below 128 MiB.\n"
                 "iterations: 1..16 (default 1). Runs real Metal allocations and GPU work.");
            return 0;
        }
        if (argc != 3 && argc != 4) return 2;
        MemoryModeSpec spec;
        if (!argv[1][0] || !modeSpecForName(argv[2], &spec)) return 2;
        unsigned iterations = kDefaultIterations;
        if (argc == 4 && !parseIterations(argv[3], &iterations)) return 2;

        const char *mock = getenv("RADV_DARWIN_MOCK");
        require(!mock || !mock[0] || !strcmp(mock, "0"),
                @"RADV_DARWIN_MOCK is not allowed for the live large-memory test");

        NSError *error = nil;
        NSBundle *bundle = [NSBundle bundleWithPath:@(argv[1])];
        require(bundle != nil, @"bundle path is invalid");
        require([bundle loadAndReturnError:&error],
                [NSString stringWithFormat:@"bundle load failed: %@", error ?: @"no NSError"]);
        Class deviceClass = [bundle classNamed:@"GarnetDevice"];
        require(deviceClass != Nil, @"GarnetDevice class is missing");
        id<MTLDevice> device = [[deviceClass alloc] init];
        require(device != nil, @"Metal device initialization failed");
        require(device.name.length > 0, @"Metal device has no name");

        id<MTLLibrary> library = [device newLibraryWithSource:largeMemoryShaderSource()
                                                       options:nil error:&error];
        require(library != nil,
            [NSString stringWithFormat:@"shader compilation failed: %@", error ?: @"no NSError"]);
        id<MTLComputePipelineState> initPipeline =
            makePipeline(device, library, @"initialize_words", &error);
        id<MTLComputePipelineState> transformPipeline =
            makePipeline(device, library, @"transform_words", &error);
        id<MTLComputePipelineState> probeReadPipeline =
            makePipeline(device, library, @"probe_read_words", &error);
        id<MTLComputePipelineState> probeRMWPipeline =
            makePipeline(device, library, @"probe_rmw_words", &error);
        library = nil;

        uint64_t maxBufferLength = (uint64_t)[device maxBufferLength];
        uint64_t baseline = currentAllocatedBytes(device);
        uint64_t firstAfter = 0;
        printf("LARGE_MEMORY_DEVICE mode=%s device=%s max_buffer_length=%llu "
               "working_set=%llu baseline_allocated=%llu iterations=%u\n",
            spec.name, device.name.UTF8String, (unsigned long long)maxBufferLength,
            (unsigned long long)[device recommendedMaxWorkingSetSize],
            (unsigned long long)baseline, iterations);

        NoCopyRunStats noCopy;
        memset(&noCopy, 0, sizeof noCopy);
        BufferRunStats finalBuffer;
        memset(&finalBuffer, 0, sizeof finalBuffer);
        for (unsigned iteration = 1; iteration <= iterations; ++iteration) {
            if (spec.noCopy)
                noCopy = runNoCopyIteration(device, iteration, probeReadPipeline);
            else {
                finalBuffer = runBufferModeIteration(device, &spec, iteration,
                    initPipeline, transformPipeline, probeRMWPipeline);
                if (spec.mode == MemoryModeSmall) {
                    MemoryModeSpec managed = spec;
                    managed.options = MTLResourceStorageModeManaged;
                    finalBuffer = runBufferModeIteration(device, &managed, iteration,
                        initPipeline, transformPipeline, probeRMWPipeline);
                }
            }
            @autoreleasepool {
                uint64_t after = currentAllocatedBytes(device);
                if (iteration == 1) firstAfter = after;
            }
            checkRetainedAllocation(device, baseline, firstAfter, iteration);
        }
        if (spec.noCopy) {
            printf("LARGE_MEMORY_PASS mode=%s imports=%u imported_logical_bytes=%llu "
                   "readonly_imports=%u callbacks=%u negative_callbacks=%u "
                   "host_mapped_bytes=%llu host_unmapped_bytes=%llu outstanding_host_bytes=0 "
                   "peak_host_mapped_bytes=%llu simultaneous_over_4g=1 "
                   "read_probe_invocations_per_iteration=%u gpu_execution=1 iterations=%u\n",
                spec.name, kNoCopyImportCount, (unsigned long long)spec.logicalBytes,
                noCopy.readOnlyImports, noCopy.callbacks, noCopy.negativeCallbacks,
                (unsigned long long)noCopy.mappedBytes,
                (unsigned long long)noCopy.unmappedBytes,
                (unsigned long long)noCopy.peakMappedBytes,
                kNoCopyImportCount, iterations);
        } else {
            uint64_t bufferBytes = kGuardBytes + spec.logicalBytes + kGuardBytes;
            printf("LARGE_MEMORY_PASS mode=%s payload_bytes=%llu buffer_bytes=%llu "
                   "guard_bytes_each=%llu host_mapped_bytes=%llu readback_chunks=%llu "
                   "final_chunk_bytes=%llu rmw_probe_invocations=%u gpu_execution=1 "
                   "iterations=%u\n",
                spec.name, (unsigned long long)spec.logicalBytes,
                (unsigned long long)bufferBytes, (unsigned long long)kGuardBytes,
                (unsigned long long)finalBuffer.hostMappedBytes,
                (unsigned long long)finalBuffer.readbackChunks,
                (unsigned long long)finalBuffer.finalChunkBytes,
                spec.large ? 1u : 0u, iterations);
        }
        device = nil;
        bundle = nil;
    }
    return 0;
}
