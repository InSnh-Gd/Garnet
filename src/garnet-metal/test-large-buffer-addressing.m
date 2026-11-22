// Live 64-bit addressing coverage for a small Shared backing allocation. Run
// this on the target GPU; it does not claim to allocate or access above 4 GiB.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <dispatch/dispatch.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum {
    kDataWords = 65536,
    kDataBytes = kDataWords * (int)sizeof(uint32_t),
    kDataBindingOffset = 256,
    kDataTailBytes = 256,
    kDataBufferBytes = kDataBindingOffset + kDataBytes + kDataTailBytes,

    kOutputWords = 32,
    kOutputPayloadBytes = kOutputWords * (int)sizeof(uint32_t),
    kOutputBindingOffset = 512,
    kOutputTailBytes = 256,
    kOutputBufferBytes = kOutputBindingOffset + kOutputPayloadBytes + kOutputTailBytes,

    kControlStorageBytes = 256,
    kControlBindingOffset = 768,
    kControlTailBytes = 256,
    kControlBufferBytes = kControlBindingOffset + kControlStorageBytes + kControlTailBytes,

    kGuardPayloadBytes = 1024,
    kPrefixGuardBindingOffset = 384,
    kPrefixGuardTailBytes = 256,
    kPrefixGuardBufferBytes =
        kPrefixGuardBindingOffset + kGuardPayloadBytes + kPrefixGuardTailBytes,
    kSuffixGuardBindingOffset = 640,
    kSuffixGuardTailBytes = 256,
    kSuffixGuardBufferBytes =
        kSuffixGuardBindingOffset + kGuardPayloadBytes + kSuffixGuardTailBytes,

    kOutputPrefixGuardWord = 0,
    kOutputSuffixGuardWord = 1,
    kOutputRoundTagWord = 2,
    kOutputDataTokenWord = 3,
    kOutputCaseBaseWord = 8,

    kControlConstantBaseRound0 = 28,
    kControlConstantBaseRound1 = 32,
};

static const uint32_t kPrefixGuardPattern = UINT32_C(0x5a5a5a5a);
static const uint32_t kSuffixGuardPattern = UINT32_C(0xa5a5a5a5);

typedef enum {
    AddressingCaseRuntimeUlong = 0,
    AddressingCaseConstantUintPointer,
    AddressingCaseHelperPointer,
    AddressingCaseNullablePhi,
    AddressingCasePointerIntegerRoundTrip,
    AddressingCaseAtomicAdd,
    AddressingCasePackedUnalignedI32,
    AddressingCaseLocalPointerTable,
    AddressingCaseCount,
} AddressingCase;

// Each little-endian uint64_t maps to an MSL uint2 with the same size/alignment.
typedef struct {
    uint64_t rmwIndex[4];
    uint64_t helperIndex;
    uint64_t atomicIndex;
    uint64_t packedByteOffset;
    uint64_t nullableIndex;
    uint64_t loopStride;
    uint32_t constantIndex;
    uint32_t constantSelector;
    uint32_t branchSelector;
    uint32_t loopCount;
    uint32_t roundTag;
    uint32_t flags;
} ControlWide;

_Static_assert(sizeof(ControlWide) == 96, "ControlWide layout changed");

@interface LargeBufferAddressingCompletion : NSObject {
@public
    dispatch_semaphore_t done;
    _Atomic unsigned callbacks;
    MTLCommandBufferStatus status;
    NSError *error;
}
@end
@implementation LargeBufferAddressingCompletion
@end

@interface LargeBufferAddressingResources : NSObject
@property(nonatomic, strong) id<MTLBuffer> dataBuffer;
@property(nonatomic, strong) id<MTLBuffer> outputBuffer;
@property(nonatomic, strong) id<MTLBuffer> controlBuffer;
@property(nonatomic, strong) id<MTLBuffer> prefixGuardBuffer;
@property(nonatomic, strong) id<MTLBuffer> suffixGuardBuffer;
@end
@implementation LargeBufferAddressingResources
@end

static LargeBufferAddressingResources *makeResources(id<MTLDevice> device) {
    LargeBufferAddressingResources *resources = [LargeBufferAddressingResources new];
    resources.dataBuffer = [device newBufferWithLength:kDataBufferBytes
                                              options:MTLResourceStorageModeShared];
    resources.outputBuffer = [device newBufferWithLength:kOutputBufferBytes
                                                options:MTLResourceStorageModeShared];
    resources.controlBuffer = [device newBufferWithLength:kControlBufferBytes
                                                 options:MTLResourceStorageModeShared];
    resources.prefixGuardBuffer = [device newBufferWithLength:kPrefixGuardBufferBytes
                                                     options:MTLResourceStorageModeShared];
    resources.suffixGuardBuffer = [device newBufferWithLength:kSuffixGuardBufferBytes
                                                     options:MTLResourceStorageModeShared];
    return resources;
}

static void failf(const char *format, ...) __attribute__((noreturn, format(printf, 1, 2)));
static void failf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    fputs("LARGE_BUFFER_ADDRESSING_FAIL ", stderr);
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    va_end(args);
    fflush(stderr);
    _exit(1);
}

static void require(BOOL condition, const char *message) {
    if (!condition) failf("%s", message);
}

static const char *caseName(AddressingCase kind) {
    switch (kind) {
        case AddressingCaseRuntimeUlong: return "runtime_ulong";
        case AddressingCaseConstantUintPointer: return "constant_uint_pointer";
        case AddressingCaseHelperPointer: return "helper_pointer";
        case AddressingCaseNullablePhi: return "nullable_phi";
        case AddressingCasePointerIntegerRoundTrip: return "ptr_int_roundtrip";
        case AddressingCaseAtomicAdd: return "atomic_add";
        case AddressingCasePackedUnalignedI32: return "packed_unaligned_i32";
        case AddressingCaseLocalPointerTable: return "local_pointer_table";
        default: return "unknown";
    }
}

static const char *functionName(AddressingCase kind) {
    switch (kind) {
        case AddressingCaseRuntimeUlong: return "runtime_ulong";
        case AddressingCaseConstantUintPointer: return "constant_uint_pointer";
        case AddressingCaseHelperPointer: return "helper_pointer";
        case AddressingCaseNullablePhi: return "nullable_phi";
        case AddressingCasePointerIntegerRoundTrip: return "ptr_int_roundtrip";
        case AddressingCaseAtomicAdd: return "atomic_add";
        case AddressingCasePackedUnalignedI32: return "packed_unaligned_i32";
        case AddressingCaseLocalPointerTable: return "local_pointer_table";
        default: return "unknown";
    }
}

static uint32_t load32(const uint8_t *base, NSUInteger byteOffset) {
    uint32_t value;
    memcpy(&value, base + byteOffset, sizeof value);
    return value;
}

static void store32(uint8_t *base, NSUInteger byteOffset, uint32_t value) {
    memcpy(base + byteOffset, &value, sizeof value);
}

static uint32_t loadWord(const uint8_t *base, uint64_t wordIndex) {
    return load32(base, (NSUInteger)wordIndex * sizeof(uint32_t));
}

static void storeWord(uint8_t *base, uint64_t wordIndex, uint32_t value) {
    store32(base, (NSUInteger)wordIndex * sizeof(uint32_t), value);
}

static uint32_t initialWord(AddressingCase kind, unsigned round, NSUInteger wordIndex) {
    uint32_t value = (uint32_t)wordIndex +
        UINT32_C(0x9e3779b9) * ((uint32_t)kind + 1u) +
        UINT32_C(0x85ebca6b) * (uint32_t)(round + 1u);
    value ^= value >> 16;
    value *= UINT32_C(0x7feb352d);
    value ^= value >> 15;
    value *= UINT32_C(0x846ca68b);
    value ^= value >> 16;
    return value;
}

static uint32_t mixRuntimeWord(uint32_t oldValue, uint32_t salt) {
    return (oldValue ^ UINT32_C(0xa5c39e71)) + salt + UINT32_C(0x31415926);
}

static uint32_t mixHelperWord(uint32_t oldValue, uint32_t salt) {
    return (oldValue ^ UINT32_C(0xa5a5f00d)) + salt;
}

static uint32_t packedValue(const uint8_t *base, uint64_t byteOffset) {
    const uint8_t *bytes = base + (NSUInteger)byteOffset;
    return (uint32_t)bytes[0] |
        ((uint32_t)bytes[1] << 8) |
        ((uint32_t)bytes[2] << 16) |
        ((uint32_t)bytes[3] << 24);
}

static void requireImage(const uint8_t *actual, const uint8_t *expected, NSUInteger length,
                         const char *caseLabel, const char *imageLabel, unsigned round) {
    for (NSUInteger i = 0; i < length; ++i) {
        if (actual[i] != expected[i])
            failf("case=%s round=%u image=%s first_bad_offset=%lu actual=0x%02x expected=0x%02x",
                  caseLabel, round, imageLabel, (unsigned long)i, actual[i], expected[i]);
    }
}

static void requirePattern(const uint8_t *bytes, NSUInteger length, uint8_t expected,
                           const char *caseLabel, const char *imageLabel, unsigned round) {
    for (NSUInteger i = 0; i < length; ++i) {
        if (bytes[i] != expected)
            failf("case=%s round=%u image=%s first_bad_offset=%lu actual=0x%02x expected=0x%02x",
                  caseLabel, round, imageLabel, (unsigned long)i, bytes[i], expected);
    }
}

static LargeBufferAddressingCompletion *observe(id<MTLCommandBuffer> command,
                                                const char *caseLabel, unsigned round) {
    if (!command) failf("case=%s round=%u command buffer creation", caseLabel, round);
    LargeBufferAddressingCompletion *completion = [LargeBufferAddressingCompletion new];
    completion->done = dispatch_semaphore_create(0);
    if (!completion->done)
        failf("case=%s round=%u completion semaphore creation", caseLabel, round);
    [command addCompletedHandler:^(id<MTLCommandBuffer> completed) {
        completion->status = completed.status;
        completion->error = completed.error;
        atomic_fetch_add(&completion->callbacks, 1);
        dispatch_semaphore_signal(completion->done);
    }];
    return completion;
}

static void awaitCommand(id<MTLCommandBuffer> command,
                         LargeBufferAddressingCompletion *completion,
                         const char *caseLabel, unsigned round) {
    if (dispatch_semaphore_wait(completion->done,
                                dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_SEC)))
        failf("case=%s round=%u bounded completion timeout", caseLabel, round);
    [command waitUntilCompleted];
    usleep(20000);
    if (atomic_load(&completion->callbacks) != 1)
        failf("case=%s round=%u completion callback count=%u", caseLabel, round,
              atomic_load(&completion->callbacks));
    NSError *executionError = completion->error ?: command.error;
    if (command.status != MTLCommandBufferStatusCompleted ||
        completion->status != MTLCommandBufferStatusCompleted || executionError)
        failf("case=%s round=%u command status=%lu observed=%lu error=%s",
              caseLabel, round, (unsigned long)command.status,
              (unsigned long)completion->status,
              executionError.localizedDescription.UTF8String ?: "none");
}

static id<MTLComputePipelineState> makePipeline(id<MTLDevice> device, id<MTLLibrary> library,
                                                 AddressingCase kind) {
    NSString *name = [NSString stringWithUTF8String:functionName(kind)];
    id<MTLFunction> function = [library newFunctionWithName:name];
    if (!function)
        failf("kernel function missing: %s", functionName(kind));
    NSError *error = nil;
    id<MTLComputePipelineState> pipeline =
        [device newComputePipelineStateWithFunction:function error:&error];
    if (!pipeline)
        failf("pipeline %s: %s", functionName(kind),
              error.localizedDescription.UTF8String ?: "creation failed");
    return pipeline;
}

static NSString *addressingSource(void) {
    return
        @"#include <metal_stdlib>\n"
         "using namespace metal;\n"
         "\n"
         "struct ControlWide {\n"
         "    uint2 rmwIndex[4];\n"
         "    uint2 helperIndex;\n"
         "    uint2 atomicIndex;\n"
         "    uint2 packedByteOffset;\n"
         "    uint2 nullableIndex;\n"
         "    uint2 loopStride;\n"
         "    uint constantIndex;\n"
         "    uint constantSelector;\n"
         "    uint branchSelector;\n"
         "    uint loopCount;\n"
         "    uint roundTag;\n"
         "    uint flags;\n"
         "};\n"
         "\n"
         "#define OUT_PREFIX_GUARD 0u\n"
         "#define OUT_SUFFIX_GUARD 1u\n"
         "#define OUT_ROUND_TAG 2u\n"
         "#define OUT_DATA_TOKEN 3u\n"
         "#define OUT_CASE_BASE 8u\n"
         "\n"
         "struct PackedI32 {\n"
         "    uchar bytes[4];\n"
         "};\n"
         "\n"
         "static uint mix_runtime_word(uint old_value, uint salt) {\n"
         "    return (old_value ^ 0xa5c39e71u) + salt + 0x31415926u;\n"
         "}\n"
         "\n"
         "// MSL 2.1 buffer fields use uint2; size_t keeps both words in runtime arithmetic.\n"
         "static size_t runtime64(uint2 words) {\n"
         "    return (size_t(words.y) << 32) | size_t(words.x);\n"
         "}\n"
         "\n"
         "// A normal helper with a device pointer and a runtime 64-bit index. The\n"
         "// compiler may inline it; this test does not claim AIR helper preservation.\n"
         "static uint helper_read_modify_readback(device uint *base, size_t index, uint salt) {\n"
         "    const uint old_value = base[index];\n"
         "    const uint next_value = (old_value ^ 0xa5a5f00du) + salt;\n"
         "    base[index] = next_value;\n"
         "    return base[index];\n"
         "}\n"
         "\n"
         "#define COMMON_ARGS \\\n"
         "    device uint *data [[buffer(0)]], \\\n"
         "    device uint *out [[buffer(1)]], \\\n"
         "    constant ControlWide &control [[buffer(2)]], \\\n"
         "    device const uint *prefixGuard [[buffer(3)]], \\\n"
         "    device const uint *suffixGuard [[buffer(4)]]\n"
         "\n"
         "#define WRITE_COMMON \\\n"
         "    out[OUT_PREFIX_GUARD] = prefixGuard[0]; \\\n"
         "    out[OUT_SUFFIX_GUARD] = suffixGuard[0]; \\\n"
         "    out[OUT_ROUND_TAG] = control.roundTag; \\\n"
         "    out[OUT_DATA_TOKEN] = data[0]\n"
         "\n"
         "kernel void runtime_ulong(COMMON_ARGS) {\n"
         "    WRITE_COMMON;\n"
         "    const size_t index0 = runtime64(control.rmwIndex[0]);\n"
         "    const size_t index1 = runtime64(control.rmwIndex[1]);\n"
         "    const size_t index2 = runtime64(control.rmwIndex[2]);\n"
         "    const size_t index3 = runtime64(control.rmwIndex[3]);\n"
         "    data[index0] = mix_runtime_word(data[index0], control.roundTag + 0x01020304u);\n"
         "    out[OUT_CASE_BASE + 0u] = data[index0];\n"
         "    data[index1] = mix_runtime_word(data[index1], control.roundTag + 0x10203040u);\n"
         "    out[OUT_CASE_BASE + 1u] = data[index1];\n"
         "    data[index2] = mix_runtime_word(data[index2], control.roundTag + 0x31415926u);\n"
         "    out[OUT_CASE_BASE + 2u] = data[index2];\n"
         "    data[index3] = mix_runtime_word(data[index3], control.roundTag + 0x27182818u);\n"
         "    out[OUT_CASE_BASE + 3u] = data[index3];\n"
         "}\n"
         "\n"
         "kernel void constant_uint_pointer(COMMON_ARGS,\n"
         "                                  constant uint *controlWords [[buffer(5)]]) {\n"
         "    WRITE_COMMON;\n"
         "    constant uint *candidate = controlWords + control.constantIndex;\n"
         "    const uint selector = control.constantSelector & 3u;\n"
         "    out[OUT_CASE_BASE] = candidate[selector];\n"
         "}\n"
         "\n"
         "kernel void helper_pointer(COMMON_ARGS) {\n"
         "    WRITE_COMMON;\n"
         "    out[OUT_CASE_BASE] = helper_read_modify_readback(\n"
         "        data, runtime64(control.helperIndex), control.roundTag + 0x13579bdfu);\n"
         "}\n"
         "\n"
         "kernel void nullable_phi(COMMON_ARGS) {\n"
         "    WRITE_COMMON;\n"
         "    device uint *legal = data + runtime64(control.nullableIndex);\n"
         "    uint direct = 0u;\n"
         "    if (legal != nullptr) direct = legal[0];\n"
         "\n"
         "    device uint *nullable =\n"
         "        ((control.branchSelector & 1u) != 0u) ? legal : nullptr;\n"
         "    uint merged = 0u;\n"
         "    if (nullable != nullptr) merged = nullable[0];\n"
         "\n"
         "    device uint *branchPointer = nullptr;\n"
         "    if ((control.branchSelector & 2u) != 0u) branchPointer = legal;\n"
         "    uint branchValue = 0u;\n"
         "    if (branchPointer != nullptr) branchValue = branchPointer[0];\n"
         "\n"
         "    device uint *loopPointer = legal;\n"
         "    uint loopSum = 0u;\n"
         "    for (uint step = 0u; step < control.loopCount; ++step) {\n"
         "        if (loopPointer != nullptr) loopSum += loopPointer[0];\n"
         "        if (step + 1u < control.loopCount) {\n"
         "            loopPointer += runtime64(control.loopStride);\n"
         "        } else {\n"
         "            loopPointer = nullptr;\n"
         "        }\n"
         "    }\n"
         "    out[OUT_CASE_BASE + 0u] = direct;\n"
         "    out[OUT_CASE_BASE + 1u] = merged;\n"
         "    out[OUT_CASE_BASE + 2u] = branchValue;\n"
         "    out[OUT_CASE_BASE + 3u] = loopSum;\n"
         "    out[OUT_CASE_BASE + 4u] = control.loopCount;\n"
         "}\n"
         "\n"
         "kernel void ptr_int_roundtrip(COMMON_ARGS) {\n"
         "    WRITE_COMMON;\n"
         "    device uint *at = data + runtime64(control.nullableIndex);\n"
         "    uintptr_t address = reinterpret_cast<uintptr_t>(at);\n"
         "    address += sizeof(uint);\n"
         "    device uint *restored = reinterpret_cast<device uint *>(address);\n"
         "    uint value = 0u;\n"
         "    if (restored != nullptr) value = restored[0];\n"
         "    out[OUT_CASE_BASE] = value;\n"
         "    uintptr_t alternate = reinterpret_cast<uintptr_t>(data + runtime64(control.helperIndex)) + size_t(2) * sizeof(uint);\n"
         "    uintptr_t selected = (control.branchSelector & 1u) ? address : alternate;\n"
         "    selected -= sizeof(uint);\n"
         "    device uint *back = reinterpret_cast<device uint *>(selected);\n"
         "    out[OUT_CASE_BASE + 1u] = back[0];\n"
         "}\n"
         "\n"
         "kernel void atomic_add(COMMON_ARGS) {\n"
         "    WRITE_COMMON;\n"
         "    device atomic_uint *cell =\n"
         "        reinterpret_cast<device atomic_uint *>(data + runtime64(control.atomicIndex));\n"
         "    const uint oldValue = atomic_fetch_add_explicit(cell, 1u, memory_order_relaxed);\n"
         "    const uint newValue = atomic_load_explicit(cell, memory_order_relaxed);\n"
         "    out[OUT_CASE_BASE + 0u] = oldValue;\n"
         "    out[OUT_CASE_BASE + 1u] = newValue;\n"
         "}\n"
         "\n"
         "kernel void packed_unaligned_i32(COMMON_ARGS) {\n"
         "    WRITE_COMMON;\n"
         "    device uchar *bytes = reinterpret_cast<device uchar *>(data);\n"
         "    device PackedI32 *packed =\n"
         "        reinterpret_cast<device PackedI32 *>(bytes + runtime64(control.packedByteOffset));\n"
         "    const uchar b0 = packed->bytes[0];\n"
         "    const uchar b1 = packed->bytes[1];\n"
         "    const uchar b2 = packed->bytes[2];\n"
         "    const uchar b3 = packed->bytes[3];\n"
         "    out[OUT_CASE_BASE] = uint(b0) | (uint(b1) << 8) |\n"
         "        (uint(b2) << 16) | (uint(b3) << 24);\n"
         "}\n"
         "\n"
         "kernel void local_pointer_table(COMMON_ARGS) {\n"
         "    WRITE_COMMON;\n"
         "    device uint *table[2][2] = {\n"
         "        {data + runtime64(control.rmwIndex[0]), data + runtime64(control.rmwIndex[1])},\n"
         "        {data + runtime64(control.rmwIndex[2]), data + runtime64(control.rmwIndex[3])}\n"
         "    };\n"
         "    uint i = control.branchSelector & 1u;\n"
         "    uint j = control.constantSelector & 1u;\n"
         "    table[i][j] = data + runtime64(control.helperIndex);\n"
         "    out[OUT_CASE_BASE] = table[i][j][0];\n"
         "    out[OUT_CASE_BASE + 1u] = table[0][0][0];\n"
         "}\n";
}

static id<MTLCommandBuffer> encodeRound(LargeBufferAddressingResources *resources,
                                        id<MTLComputePipelineState> pipeline,
                                        id<MTLCommandQueue> queue,
                                        const char *caseLabel, unsigned round) {
    id<MTLBuffer> data = resources.dataBuffer;
    id<MTLBuffer> output = resources.outputBuffer;
    id<MTLBuffer> control = resources.controlBuffer;
    id<MTLBuffer> prefixGuard = resources.prefixGuardBuffer;
    id<MTLBuffer> suffixGuard = resources.suffixGuardBuffer;

    id<MTLCommandBuffer> command = [queue commandBuffer];
    if (!command) failf("case=%s round=%u command buffer creation", caseLabel, round);
    id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
    if (!encoder) failf("case=%s round=%u compute encoder creation", caseLabel, round);
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:data offset:kDataBindingOffset atIndex:0];
    [encoder setBuffer:output offset:kOutputBindingOffset atIndex:1];
    [encoder setBuffer:control offset:kControlBindingOffset atIndex:2];
    [encoder setBuffer:prefixGuard offset:kPrefixGuardBindingOffset atIndex:3];
    [encoder setBuffer:suffixGuard offset:kSuffixGuardBindingOffset atIndex:4];
    [encoder setBuffer:control offset:kControlBindingOffset atIndex:5];
    [encoder dispatchThreadgroups:MTLSizeMake(1, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [encoder endEncoding];
    return command;
}

static void configureControl(ControlWide *control, AddressingCase kind, unsigned round) {
    static const uint64_t runtimeIndices[2][4] = {
        { 1, 8191, 32768, 65534 },
        { 2, 12389, 40009, 65533 },
    };

    memset(control, 0, sizeof *control);
    for (unsigned lane = 0; lane < 4; ++lane)
        control->rmwIndex[lane] = runtimeIndices[round][lane];
    control->constantIndex =
        round == 0 ? kControlConstantBaseRound0 : kControlConstantBaseRound1;
    control->constantSelector = round == 0 ? 1u : 3u;
    control->roundTag = round == 0 ? UINT32_C(0x4c425231) : UINT32_C(0x4c425232);
    control->flags = ((uint32_t)kind << 8) | (uint32_t)(round + 1u);

    switch (kind) {
        case AddressingCaseRuntimeUlong:
            break;
        case AddressingCaseConstantUintPointer:
            break;
        case AddressingCaseHelperPointer:
            control->helperIndex = round == 0 ? 97 : 54321;
            break;
        case AddressingCaseNullablePhi:
            control->nullableIndex = round == 0 ? 4096 : 30003;
            control->loopStride = round == 0 ? 31 : 97;
            control->loopCount = round == 0 ? 2 : 3;
            control->branchSelector = round == 0 ? 0u : 3u;
            break;
        case AddressingCasePointerIntegerRoundTrip:
            control->nullableIndex = round == 0 ? 4096 : 30003;
            control->helperIndex = round == 0 ? 97 : 54321;
            control->branchSelector = round;
            break;
        case AddressingCaseAtomicAdd:
            control->atomicIndex = round == 0 ? 7 : 65528;
            break;
        case AddressingCasePackedUnalignedI32:
            control->packedByteOffset = round == 0 ? 4101 : 200003;
            break;
        case AddressingCaseLocalPointerTable:
            control->helperIndex = round == 0 ? 97 : 54321;
            control->branchSelector = round;
            control->constantSelector = round;
            break;
        default:
            failf("unknown case index=%d", (int)kind);
    }
}

static void validateControl(const ControlWide *control, AddressingCase kind) {
    if (kind == AddressingCaseRuntimeUlong) {
        for (unsigned i = 0; i < 4; ++i) {
            require(control->rmwIndex[i] > 0 &&
                        control->rmwIndex[i] < kDataWords,
                    "runtime ulong index outside the data window");
            for (unsigned j = 0; j < i; ++j)
                require(control->rmwIndex[i] != control->rmwIndex[j],
                        "runtime ulong indexes overlap");
        }
    } else if (kind == AddressingCaseHelperPointer) {
        require(control->helperIndex > 0 &&
                    control->helperIndex < kDataWords,
                "helper index outside the data window");
    } else if (kind == AddressingCaseAtomicAdd) {
        require(control->atomicIndex > 0 &&
                    control->atomicIndex < kDataWords,
                "atomic index outside the data window");
    } else if (kind == AddressingCasePackedUnalignedI32) {
        require(control->packedByteOffset > 0 &&
                    control->packedByteOffset <= kDataBytes - sizeof(uint32_t) &&
                    (control->packedByteOffset & 3u) != 0,
                "packed byte offset is invalid or aligned");
    } else if (kind == AddressingCaseNullablePhi ||
               kind == AddressingCasePointerIntegerRoundTrip) {
        require(control->nullableIndex > 0 &&
                    control->nullableIndex + 1u < kDataWords,
                "nullable/round-trip index outside the data window");
        if (kind == AddressingCaseNullablePhi) {
            require(control->loopCount > 0 &&
                        control->nullableIndex +
                            control->loopStride * (control->loopCount - 1u) <
                            kDataWords,
                    "nullable loop leaves the data window");
        }
    }
}

static void runCaseRound(LargeBufferAddressingResources *resources,
                         id<MTLCommandQueue> queue,
                         id<MTLComputePipelineState> pipeline,
                         AddressingCase kind, unsigned round) {
    static const uint32_t runtimeSalts[4] = {
        UINT32_C(0x01020304), UINT32_C(0x10203040),
        UINT32_C(0x31415926), UINT32_C(0x27182818),
    };
    static const uint32_t candidateValues[2][4] = {
        { UINT32_C(0x11112222), UINT32_C(0x33334444),
          UINT32_C(0x55556666), UINT32_C(0x77778888) },
        { UINT32_C(0x89abcdef), UINT32_C(0xfedcba98),
          UINT32_C(0x0badf00d), UINT32_C(0x76543210) },
    };
    const char *label = caseName(kind);
    uint8_t expectedData[kDataBufferBytes];
    uint8_t expectedOutput[kOutputBufferBytes];
    uint8_t expectedControl[kControlBufferBytes];

    @autoreleasepool {
        id<MTLBuffer> data = resources.dataBuffer;
        id<MTLBuffer> output = resources.outputBuffer;
        id<MTLBuffer> control = resources.controlBuffer;
        id<MTLBuffer> prefixGuard = resources.prefixGuardBuffer;
        id<MTLBuffer> suffixGuard = resources.suffixGuardBuffer;
        require(data && output && control && prefixGuard && suffixGuard,
                "bound buffer allocation");
        require(data.contents && output.contents && control.contents &&
                    prefixGuard.contents && suffixGuard.contents,
                "Shared buffer CPU mapping unavailable");
        require(data.length == kDataBufferBytes &&
                    output.length == kOutputBufferBytes &&
                    control.length == kControlBufferBytes &&
                    prefixGuard.length == kPrefixGuardBufferBytes &&
                    suffixGuard.length == kSuffixGuardBufferBytes,
                "buffer length changed");

        memset(data.contents, 0xd3, data.length);
        memset(output.contents, 0xcd, output.length);
        memset(control.contents, 0x6d, control.length);
        memset(prefixGuard.contents, 0x5a, prefixGuard.length);
        memset(suffixGuard.contents, 0xa5, suffixGuard.length);

        uint8_t *dataWindow = (uint8_t *)data.contents + kDataBindingOffset;
        for (NSUInteger i = 0; i < kDataWords; ++i) {
            uint32_t value = initialWord(kind, round, i);
            memcpy(dataWindow + i * sizeof value, &value, sizeof value);
        }

        ControlWide configured;
        configureControl(&configured, kind, round);
        validateControl(&configured, kind);
        memcpy((uint8_t *)control.contents + kControlBindingOffset,
               &configured, sizeof configured);
        for (unsigned i = 0; i < 4; ++i) {
            storeWord((uint8_t *)control.contents + kControlBindingOffset,
                      configured.constantIndex + i, candidateValues[round][i]);
        }

        memcpy(expectedData, data.contents, sizeof expectedData);
        memcpy(expectedOutput, output.contents, sizeof expectedOutput);
        memcpy(expectedControl, control.contents, sizeof expectedControl);

        uint8_t *expectedDataWindow = expectedData + kDataBindingOffset;
        uint8_t *expectedOutputWindow = expectedOutput + kOutputBindingOffset;
        storeWord(expectedOutputWindow, kOutputPrefixGuardWord, kPrefixGuardPattern);
        storeWord(expectedOutputWindow, kOutputSuffixGuardWord, kSuffixGuardPattern);
        storeWord(expectedOutputWindow, kOutputRoundTagWord, configured.roundTag);
        storeWord(expectedOutputWindow, kOutputDataTokenWord, loadWord(expectedDataWindow, 0));

        switch (kind) {
            case AddressingCaseRuntimeUlong:
                for (unsigned i = 0; i < 4; ++i) {
                    uint64_t index = configured.rmwIndex[i];
                    uint32_t oldValue = loadWord(expectedDataWindow, index);
                    uint32_t value =
                        mixRuntimeWord(oldValue, configured.roundTag + runtimeSalts[i]);
                    storeWord(expectedDataWindow, index, value);
                    storeWord(expectedOutputWindow, kOutputCaseBaseWord + i, value);
                }
                break;
            case AddressingCaseConstantUintPointer: {
                uint32_t selector = configured.constantSelector & 3u;
                storeWord(expectedOutputWindow, kOutputCaseBaseWord,
                          candidateValues[round][selector]);
                break;
            }
            case AddressingCaseHelperPointer: {
                uint32_t oldValue = loadWord(expectedDataWindow, configured.helperIndex);
                uint32_t value =
                    mixHelperWord(oldValue, configured.roundTag + UINT32_C(0x13579bdf));
                storeWord(expectedDataWindow, configured.helperIndex, value);
                storeWord(expectedOutputWindow, kOutputCaseBaseWord, value);
                break;
            }
            case AddressingCaseNullablePhi: {
                uint32_t direct = loadWord(expectedDataWindow, configured.nullableIndex);
                uint32_t merged = (configured.branchSelector & 1u)
                    ? direct : 0u;
                uint32_t branchValue = (configured.branchSelector & 2u)
                    ? direct : 0u;
                uint32_t loopSum = 0;
                for (uint32_t step = 0; step < configured.loopCount; ++step) {
                    uint64_t index =
                        configured.nullableIndex + configured.loopStride * step;
                    loopSum += loadWord(expectedDataWindow, index);
                }
                storeWord(expectedOutputWindow, kOutputCaseBaseWord + 0u, direct);
                storeWord(expectedOutputWindow, kOutputCaseBaseWord + 1u, merged);
                storeWord(expectedOutputWindow, kOutputCaseBaseWord + 2u, branchValue);
                storeWord(expectedOutputWindow, kOutputCaseBaseWord + 3u, loopSum);
                storeWord(expectedOutputWindow, kOutputCaseBaseWord + 4u,
                          configured.loopCount);
                break;
            }
            case AddressingCasePointerIntegerRoundTrip: {
                uint32_t value = loadWord(expectedDataWindow, configured.nullableIndex + 1u);
                storeWord(expectedOutputWindow, kOutputCaseBaseWord, value);
                uint64_t selectedIndex = configured.branchSelector & 1u
                    ? configured.nullableIndex : configured.helperIndex + 1u;
                storeWord(expectedOutputWindow, kOutputCaseBaseWord + 1u,
                          loadWord(expectedDataWindow, selectedIndex));
                break;
            }
            case AddressingCaseAtomicAdd: {
                uint32_t oldValue = loadWord(expectedDataWindow, configured.atomicIndex);
                uint32_t newValue = oldValue + 1u;
                storeWord(expectedDataWindow, configured.atomicIndex, newValue);
                storeWord(expectedOutputWindow, kOutputCaseBaseWord + 0u, oldValue);
                storeWord(expectedOutputWindow, kOutputCaseBaseWord + 1u, newValue);
                break;
            }
            case AddressingCasePackedUnalignedI32:
                storeWord(expectedOutputWindow, kOutputCaseBaseWord,
                          packedValue(expectedDataWindow, configured.packedByteOffset));
                break;
            case AddressingCaseLocalPointerTable:
                storeWord(expectedOutputWindow, kOutputCaseBaseWord,
                          loadWord(expectedDataWindow, configured.helperIndex));
                storeWord(expectedOutputWindow, kOutputCaseBaseWord + 1u,
                          loadWord(expectedDataWindow, round == 0
                              ? configured.helperIndex : configured.rmwIndex[0]));
                break;
            default:
                failf("unknown case index=%d", (int)kind);
        }
    }

    // Metal may release bound resources at completion. Keep the oracle in separate
    // readback buffers so the application can release every shader buffer early.
    LargeBufferAddressingResources *readback = makeResources(queue.device);
    require(readback.dataBuffer && readback.outputBuffer && readback.controlBuffer &&
                readback.prefixGuardBuffer && readback.suffixGuardBuffer,
            "readback buffer allocation");
    id<MTLCommandBuffer> command = nil;
    @autoreleasepool {
        command = encodeRound(resources, pipeline, queue, label, round);
        id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
        require(blit != nil, "readback blit encoder creation");
        [blit copyFromBuffer:resources.dataBuffer sourceOffset:0
                   toBuffer:readback.dataBuffer destinationOffset:0 size:kDataBufferBytes];
        [blit copyFromBuffer:resources.outputBuffer sourceOffset:0
                   toBuffer:readback.outputBuffer destinationOffset:0 size:kOutputBufferBytes];
        [blit copyFromBuffer:resources.controlBuffer sourceOffset:0
                   toBuffer:readback.controlBuffer destinationOffset:0 size:kControlBufferBytes];
        [blit copyFromBuffer:resources.prefixGuardBuffer sourceOffset:0
                   toBuffer:readback.prefixGuardBuffer destinationOffset:0 size:kPrefixGuardBufferBytes];
        [blit copyFromBuffer:resources.suffixGuardBuffer sourceOffset:0
                   toBuffer:readback.suffixGuardBuffer destinationOffset:0 size:kSuffixGuardBufferBytes];
        [blit endEncoding];
    }

    __weak id<MTLBuffer> weakData = resources.dataBuffer;
    __weak id<MTLBuffer> weakOutput = resources.outputBuffer;
    __weak id<MTLBuffer> weakControl = resources.controlBuffer;
    __weak id<MTLBuffer> weakPrefixGuard = resources.prefixGuardBuffer;
    __weak id<MTLBuffer> weakSuffixGuard = resources.suffixGuardBuffer;
    LargeBufferAddressingCompletion *completion = observe(command, label, round);
    resources.dataBuffer = nil;
    resources.outputBuffer = nil;
    resources.controlBuffer = nil;
    resources.prefixGuardBuffer = nil;
    resources.suffixGuardBuffer = nil;

    require(weakData && weakOutput && weakControl && weakPrefixGuard && weakSuffixGuard,
            "setBuffer did not retain every bound buffer before submission");
    [command commit];
    awaitCommand(command, completion, label, round);

    resources.dataBuffer = readback.dataBuffer;
    resources.outputBuffer = readback.outputBuffer;
    resources.controlBuffer = readback.controlBuffer;
    resources.prefixGuardBuffer = readback.prefixGuardBuffer;
    resources.suffixGuardBuffer = readback.suffixGuardBuffer;

    id<MTLBuffer> data = resources.dataBuffer;
    id<MTLBuffer> output = resources.outputBuffer;
    id<MTLBuffer> control = resources.controlBuffer;
    id<MTLBuffer> prefixGuard = resources.prefixGuardBuffer;
    id<MTLBuffer> suffixGuard = resources.suffixGuardBuffer;
    requireImage(data.contents, expectedData, sizeof expectedData, label, "data", round);
    requireImage(output.contents, expectedOutput, sizeof expectedOutput, label, "output", round);
    requireImage(control.contents, expectedControl, sizeof expectedControl,
                 label, "control", round);
    requirePattern(prefixGuard.contents, sizeof(uint8_t) * kPrefixGuardBufferBytes,
                   0x5a, label, "prefix_guard", round);
    requirePattern(suffixGuard.contents, sizeof(uint8_t) * kSuffixGuardBufferBytes,
                   0xa5, label, "suffix_guard", round);

    printf("LARGE_BUFFER_ADDRESSING_ROUND_PASS case=%s round=%u dispatch=1 invocation=1 "
           "data_bytes=%d output_words=%d binding_offsets=data:%d,output:%d,control:%d,"
           "prefix:%d,suffix:%d full_image_oracle=1 guards=1 weak_retention=1\n",
           label, round, kDataBytes, kOutputWords, kDataBindingOffset,
           kOutputBindingOffset, kControlBindingOffset, kPrefixGuardBindingOffset,
           kSuffixGuardBindingOffset);
}

int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc == 2 && strcmp(argv[1], "--help") == 0) {
            puts("Usage: test-large-buffer-addressing BUNDLE|--reference\n"
                 "Runs eight two-round single-invocation GPU cases on a 256 KiB Shared window.");
            return 0;
        }
        if (argc != 2) {
            fputs("Usage: test-large-buffer-addressing BUNDLE|--reference\n", stderr);
            return 2;
        }
        setvbuf(stdout, NULL, _IONBF, 0);

        id<MTLDevice> device = nil;
        if (strcmp(argv[1], "--reference") == 0) {
            device = MTLCreateSystemDefaultDevice();
        } else {
            NSError *bundleError = nil;
            NSBundle *bundle = [NSBundle bundleWithPath:@(argv[1])];
            if (!bundle || ![bundle loadAndReturnError:&bundleError])
                failf("bundle load: %s",
                      bundleError.localizedDescription.UTF8String ?: "load failed");
            Class deviceClass = [bundle classNamed:@"GarnetDevice"];
            if (deviceClass == Nil) failf("Metal device class missing from bundle");
            device = [[deviceClass alloc] init];
        }
        require(device != nil, "Metal device unavailable");

        NSString *source = addressingSource();
        const char *sourcePath = getenv("LARGE_BUFFER_ADDRESSING_SOURCE");
        if (sourcePath) {
            NSError *writeError = nil;
            if (![source writeToFile:@(sourcePath) atomically:YES
                            encoding:NSUTF8StringEncoding error:&writeError])
                failf("MSL source write: %s",
                      writeError.localizedDescription.UTF8String ?: "write failed");
        }

        MTLCompileOptions *compileOptions = [MTLCompileOptions new];
        compileOptions.languageVersion = MTLLanguageVersion2_1;
        NSError *compileError = nil;
        id<MTLLibrary> library =
            [device newLibraryWithSource:source options:compileOptions error:&compileError];
        if (!library)
            failf("MSL 2.1 compilation: %s",
                  compileError.localizedDescription.UTF8String ?: "compilation failed");

        NSMutableArray *pipelines = [NSMutableArray arrayWithCapacity:AddressingCaseCount];
        for (NSUInteger i = 0; i < AddressingCaseCount; ++i) {
            AddressingCase kind = (AddressingCase)i;
            [pipelines addObject:makePipeline(device, library, kind)];
        }
        id<MTLCommandQueue> queue = [device newCommandQueue];
        require(queue != nil, "command queue creation");

        LargeBufferAddressingResources *resources = makeResources(device);
        require(resources.dataBuffer && resources.outputBuffer &&
                    resources.controlBuffer && resources.prefixGuardBuffer &&
                    resources.suffixGuardBuffer,
                "Shared buffer allocation");

        for (NSUInteger i = 0; i < AddressingCaseCount; ++i) {
            AddressingCase kind = (AddressingCase)i;
            for (unsigned round = 0; round < 2; ++round)
                runCaseRound(resources, queue, pipelines[i], kind, round);
            printf("LARGE_BUFFER_ADDRESSING_CASE_PASS case=%s rounds=2 dispatches=2 "
                   "invocations=2\n", caseName(kind));
        }

        printf("LARGE_BUFFER_ADDRESSING_PASS cases=%d rounds=%d dispatches=%d "
               "invocations=%d data_bytes=%d small_backing=1 actual_over_4g=0 "
               "msl_version=2.1\n", AddressingCaseCount, AddressingCaseCount * 2,
               AddressingCaseCount * 2, AddressingCaseCount * 2, kDataBytes);
    }
    _exit(0);
}
