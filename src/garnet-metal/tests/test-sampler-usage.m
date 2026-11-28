#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const uint8_t kArgumentGuard = 0xa5;
static const uint8_t kUploadGuard = 0x5a;
static const uint8_t kOutputGuard = 0xcd;
static const NSUInteger kArgumentOffset = 256;
static const NSUInteger kArgumentTail = 256;
static const NSUInteger kUploadOffset = 64;
static const NSUInteger kOutputOffset = 256;
static const NSUInteger kMixedThreads = 32;
static const NSUInteger kDepthSize = 2;
static const NSUInteger kDepthBytes = 2 * 2 * sizeof(float);
static const float kColorValue = 0.375f;
static const float kReference = 0.5f;

static void require(BOOL ok, NSString *message) {
    if (!ok) {
        fprintf(stderr, "ARGUMENT_SAMPLER_USAGE_FAIL %s\n", message.UTF8String);
        exit(1);
    }
}

static NSUInteger alignUp(NSUInteger value, NSUInteger alignment) {
    require(alignment != 0, @"argument encoder alignment is zero");
    return (value + alignment - 1) / alignment * alignment;
}

static id<MTLBuffer> newSharedBuffer(id<MTLDevice> device, NSUInteger length,
                                     uint8_t fill, NSString *description) {
    id<MTLBuffer> buffer = [device newBufferWithLength:length
        options:MTLResourceStorageModeShared];
    require(buffer != nil && buffer.contents != NULL,
        [description stringByAppendingString:@" unavailable"]);
    memset(buffer.contents, fill, length);
    return buffer;
}

static void requireGuard(id<MTLBuffer> buffer, NSRange payload, uint8_t guard,
                         NSString *description) {
    require(buffer.contents != NULL && payload.location <= buffer.length &&
        payload.length <= buffer.length - payload.location,
        [description stringByAppendingString:@" range invalid"]);
    const uint8_t *bytes = buffer.contents;
    NSUInteger end = payload.location + payload.length;
    for (NSUInteger i = 0; i < payload.location; ++i)
        if (bytes[i] != guard)
            require(NO, [NSString stringWithFormat:@"%@ prefix guard changed at %lu",
                description, (unsigned long)i]);
    for (NSUInteger i = end; i < buffer.length; ++i)
        if (bytes[i] != guard)
            require(NO, [NSString stringWithFormat:@"%@ suffix guard changed at %lu",
                description, (unsigned long)i]);
}

static void commitAndWait(id<MTLCommandBuffer> command, NSString *operation) {
    require(command != nil, [operation stringByAppendingString:@" command unavailable"]);
    [command commit];
    [command waitUntilCompleted];
    require(command.status == MTLCommandBufferStatusCompleted && command.error == nil,
        command.error.description ?: [operation stringByAppendingString:@" failed"]);
}

static id<MTLSamplerState> newSampler(id<MTLDevice> device,
                                      MTLCompareFunction function,
                                      NSString *description) {
    MTLSamplerDescriptor *descriptor = [MTLSamplerDescriptor new];
    descriptor.normalizedCoordinates = YES;
    descriptor.minFilter = MTLSamplerMinMagFilterNearest;
    descriptor.magFilter = MTLSamplerMinMagFilterNearest;
    descriptor.mipFilter = MTLSamplerMipFilterNotMipmapped;
    descriptor.sAddressMode = MTLSamplerAddressModeClampToEdge;
    descriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
    descriptor.rAddressMode = MTLSamplerAddressModeClampToEdge;
    descriptor.compareFunction = function;
    descriptor.supportArgumentBuffers = YES;
    id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:descriptor];
    require(sampler != nil, [description stringByAppendingString:@" unavailable"]);
    return sampler;
}

static id<MTLComputePipelineState> newComputePipeline(id<MTLDevice> device,
                                                      id<MTLFunction> function,
                                                      NSString *description,
                                                      NSError **error) {
    require(function != nil, [description stringByAppendingString:@" function unavailable"]);
    id<MTLComputePipelineState> pipeline =
        [device newComputePipelineStateWithFunction:function error:error];
    require(pipeline != nil, (*error).description ?: [description
        stringByAppendingString:@" pipeline unavailable"]);
    return pipeline;
}

static void writeAIR(id<MTLFunction> function, NSString *path) {
    SEL selector = NSSelectorFromString(@"bitcodeData");
    NSData *air = [function respondsToSelector:selector] ?
        ((id (*)(id, SEL))objc_msgSend)(function, selector) : nil;
    require(air.length > 0 && [air writeToFile:path atomically:YES],
        [NSString stringWithFormat:@"AIR write failed for %@", function.name]);
}

static float compareResult(MTLCompareFunction function, float reference,
                           float depth) {
    switch (function) {
    case MTLCompareFunctionNever: return 0.0f;
    case MTLCompareFunctionLess: return reference < depth ? 1.0f : 0.0f;
    case MTLCompareFunctionEqual: return reference == depth ? 1.0f : 0.0f;
    case MTLCompareFunctionLessEqual: return reference <= depth ? 1.0f : 0.0f;
    case MTLCompareFunctionGreater: return reference > depth ? 1.0f : 0.0f;
    case MTLCompareFunctionNotEqual: return reference != depth ? 1.0f : 0.0f;
    case MTLCompareFunctionGreaterEqual: return reference >= depth ? 1.0f : 0.0f;
    case MTLCompareFunctionAlways: return 1.0f;
    }
    return -1.0f;
}

static void checkMixedOutput(id<MTLBuffer> output, BOOL reference,
                             BOOL ordinary, BOOL comparison) {
    NSRange payload = NSMakeRange(kOutputOffset, kMixedThreads * 2 * sizeof(float));
    requireGuard(output, payload, kOutputGuard, @"mixed output after GPU");
    const float *values = (const float *)((const uint8_t *)output.contents +
        kOutputOffset);
    static const float depthByCorner[4] = {0.25f, 0.75f, 0.75f, 0.25f};
    for (NSUInteger thread = 0; thread < kMixedThreads; ++thread) {
        NSUInteger function = thread / 4;
        NSUInteger corner = thread % 4;
        float expectedCompare = compareResult((MTLCompareFunction)function,
            kReference, depthByCorner[corner]);
        require(!ordinary || values[thread * 2] == kColorValue,
            [NSString stringWithFormat:@"mixed ordinary sample[%lu]=%.9g expected=%.9g",
                (unsigned long)thread, values[thread * 2], kColorValue]);
        require(!comparison || values[thread * 2 + 1] == expectedCompare,
            [NSString stringWithFormat:@"mixed compare[%lu] function=%lu depth=%.2f "
                "actual=%.9g expected=%.9g", (unsigned long)thread,
                (unsigned long)function, depthByCorner[corner],
                values[thread * 2 + 1], expectedCompare]);
        for (NSUInteger component = 0; component < 2; ++component) {
            if (component == 0 ? ordinary : comparison) continue;
            const uint8_t *bytes = (const uint8_t *)&values[thread * 2 + component];
            for (NSUInteger byte = 0; byte < sizeof(float); ++byte)
                require(bytes[byte] == kOutputGuard, @"unselected sampler output changed");
        }
    }
    printf("ARGUMENT_SAMPLER_USAGE_CASE_PASS case=%s backend=%s "
           "functions=Never,Less,Equal,LessEqual,Greater,NotEqual,GreaterEqual,Always "
           "depth=0.25,0.75 ordinary=%u comparison=%u outputs=%u\n",
        ordinary && comparison ? "argument-mixed" :
            ordinary ? "argument-ordinary" : "argument-compare",
        reference ? "reference" : "bundle", (unsigned)ordinary,
        (unsigned)comparison, (unsigned)(32 * (ordinary + comparison)));
}

static void checkPairOutput(id<MTLBuffer> output, float expectedCompare,
                            const char *caseName, BOOL reference) {
    NSRange payload = NSMakeRange(kOutputOffset, 2 * sizeof(float));
    requireGuard(output, payload, kOutputGuard,
        [NSString stringWithUTF8String:caseName]);
    const float *values = (const float *)((const uint8_t *)output.contents +
        kOutputOffset);
    require(values[0] == kColorValue,
        [NSString stringWithFormat:@"%s ordinary=%.9g expected=%.9g",
            caseName, values[0], kColorValue]);
    require(values[1] == expectedCompare,
        [NSString stringWithFormat:@"%s compare=%.9g expected=%.9g",
            caseName, values[1], expectedCompare]);
    printf("ARGUMENT_SAMPLER_USAGE_CASE_PASS case=%s backend=%s "
           "ordinary=%.9g compare=%.9g\n", caseName,
        reference ? "reference" : "bundle", values[0], values[1]);
}

static id<MTLTexture> newColorTexture(id<MTLDevice> device) {
    MTLTextureDescriptor *descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
            MTLPixelFormatRGBA32Float width:1 height:1 mipmapped:NO];
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
    require(texture != nil, @"sampler color texture unavailable");
    const float color[4] = {kColorValue, 0.625f, 0.875f, 1.0f};
    [texture replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0
        withBytes:color bytesPerRow:sizeof color];
    return texture;
}

static id<MTLTexture> newDepthTexture(id<MTLDevice> device,
                                      id<MTLCommandQueue> queue,
                                      id<MTLBuffer> *uploadOut) {
    MTLTextureDescriptor *descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
            MTLPixelFormatDepth32Float width:kDepthSize height:kDepthSize
            mipmapped:NO];
    descriptor.storageMode = MTLStorageModePrivate;
    descriptor.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
    require(texture != nil, @"sampler Depth32Float texture unavailable");

    id<MTLBuffer> upload = newSharedBuffer(device,
        kUploadOffset + kDepthBytes + 64, kUploadGuard, @"sampler depth upload");
    const float depth[4] = {0.25f, 0.75f, 0.75f, 0.25f};
    memcpy((uint8_t *)upload.contents + kUploadOffset, depth, kDepthBytes);
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
    require(command != nil && blit != nil, @"sampler depth upload encoder unavailable");
    [blit copyFromBuffer:upload sourceOffset:kUploadOffset
        sourceBytesPerRow:kDepthSize * sizeof(float)
        sourceBytesPerImage:kDepthBytes
        sourceSize:MTLSizeMake(kDepthSize, kDepthSize, 1)
        toTexture:texture destinationSlice:0 destinationLevel:0
        destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding];
    commitAndWait(command, @"sampler depth upload");
    requireGuard(upload, NSMakeRange(kUploadOffset, kDepthBytes),
        kUploadGuard, @"sampler depth upload after GPU");
    *uploadOut = upload;
    return texture;
}

static void runMixedCase(id<MTLDevice> device, id<MTLCommandQueue> queue,
                         id<MTLComputePipelineState> pipeline,
                         id<MTLArgumentEncoder> encoder,
                         id<MTLTexture> color, id<MTLTexture> depth,
                         NSArray<id<MTLSamplerState>> *samplers,
                         BOOL reference, BOOL ordinary, BOOL comparison) {
    NSUInteger argumentOffset = alignUp(kArgumentOffset, encoder.alignment);
    id<MTLBuffer> arguments = newSharedBuffer(device,
        argumentOffset + encoder.encodedLength + kArgumentTail,
        kArgumentGuard, @"mixed sampler argument buffer");
    [encoder setArgumentBuffer:arguments offset:argumentOffset];
    [encoder setTexture:color atIndex:0];
    [encoder setTexture:depth atIndex:1];
    for (NSUInteger i = 0; i < samplers.count; ++i)
        [encoder setSamplerState:samplers[i] atIndex:2 + i];
    NSRange argumentRange = NSMakeRange(argumentOffset, encoder.encodedLength);
    requireGuard(arguments, argumentRange, kArgumentGuard,
        @"mixed argument buffer after encode");
    NSData *argumentSnapshot = [NSData dataWithBytes:arguments.contents
        length:arguments.length];

    id<MTLBuffer> output = newSharedBuffer(device,
        kOutputOffset + kMixedThreads * 2 * sizeof(float) + 256,
        kOutputGuard, @"mixed sampler output");
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLComputeCommandEncoder> compute = [command computeCommandEncoder];
    require(command != nil && compute != nil, @"mixed sampler encoder unavailable");
    [compute setComputePipelineState:pipeline];
    [compute setBuffer:arguments offset:argumentOffset atIndex:3];
    [compute setBuffer:output offset:kOutputOffset atIndex:0];
    [compute useResource:color usage:MTLResourceUsageRead];
    [compute useResource:depth usage:MTLResourceUsageRead];
    [compute dispatchThreads:MTLSizeMake(kMixedThreads, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(kMixedThreads, 1, 1)];
    [compute endEncoding];
    commitAndWait(command, @"mixed ordinary and comparison sampling");
    checkMixedOutput(output, reference, ordinary, comparison);
    requireGuard(arguments, argumentRange, kArgumentGuard,
        @"mixed argument buffer after GPU");
    require(memcmp(argumentSnapshot.bytes, arguments.contents,
        arguments.length) == 0, @"GPU modified the mixed argument buffer");
}

static void runDirectCase(id<MTLDevice> device, id<MTLCommandQueue> queue,
                          id<MTLComputePipelineState> pipeline,
                          id<MTLTexture> color, id<MTLTexture> depth,
                          id<MTLSamplerState> sampler, BOOL reference) {
    id<MTLBuffer> output = newSharedBuffer(device,
        kOutputOffset + 2 * sizeof(float) + 256, kOutputGuard,
        @"direct sampler output");
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLComputeCommandEncoder> compute = [command computeCommandEncoder];
    require(command != nil && compute != nil, @"direct sampler encoder unavailable");
    [compute setComputePipelineState:pipeline];
    [compute setTexture:color atIndex:0];
    [compute setTexture:depth atIndex:1];
    [compute setSamplerState:sampler atIndex:3];
    [compute setBuffer:output offset:kOutputOffset atIndex:0];
    [compute dispatchThreads:MTLSizeMake(1, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [compute endEncoding];
    commitAndWait(command, @"direct Never ordinary/compare sampling");
    checkPairOutput(output, 0.0f, "direct-never", reference);
}

static void runConstexprCase(id<MTLDevice> device, id<MTLCommandQueue> queue,
                             id<MTLComputePipelineState> pipeline,
                             id<MTLTexture> color, id<MTLTexture> depth,
                             float expectedCompare, const char *caseName,
                             BOOL reference) {
    id<MTLBuffer> output = newSharedBuffer(device,
        kOutputOffset + 2 * sizeof(float) + 256, kOutputGuard,
        [NSString stringWithUTF8String:caseName]);
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLComputeCommandEncoder> compute = [command computeCommandEncoder];
    require(command != nil && compute != nil,
        [NSString stringWithFormat:@"%s encoder unavailable", caseName]);
    [compute setComputePipelineState:pipeline];
    [compute setTexture:color atIndex:0];
    [compute setTexture:depth atIndex:1];
    [compute setBuffer:output offset:kOutputOffset atIndex:0];
    [compute dispatchThreads:MTLSizeMake(1, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [compute endEncoding];
    commitAndWait(command, [NSString stringWithUTF8String:caseName]);
    checkPairOutput(output, expectedCompare, caseName, reference);
}

static NSString *validSource(void) {
    return @"#include <metal_stdlib>\n"
        "using namespace metal;\n"
        "struct MixedArgs {\n"
        "  texture2d<float> color [[id(0)]];\n"
        "  depth2d<float> depth [[id(1)]];\n"
        "  array<sampler, 8> filters [[id(2)]];\n"
        "};\n"
        "kernel void sampler_mixed(constant MixedArgs &args [[buffer(3)]],\n"
        "    device float *out [[buffer(0)]],\n"
        "    uint i [[thread_position_in_grid]]) {\n"
        "  uint function = i >> 2u;\n"
        "  uint corner = i & 3u;\n"
        "  float2 uv = float2((corner & 1u) ? 0.75f : 0.25f,\n"
        "                     (corner & 2u) ? 0.75f : 0.25f);\n"
        "  sampler state = args.filters[function];\n"
        "  float4 color = args.color.sample(state, float2(0.5f));\n"
        "  float pass = args.depth.sample_compare(state, uv, 0.5f, level(0.0f));\n"
        "  out[2u * i] = color.x;\n"
        "  out[2u * i + 1u] = pass;\n"
        "}\n"
        "kernel void sampler_direct(texture2d<float> color [[texture(0)]],\n"
        "    depth2d<float> depth [[texture(1)]],\n"
        "    sampler state [[sampler(3)]],\n"
        "    device float *out [[buffer(0)]]) {\n"
        "  out[0] = color.sample(state, float2(0.5f)).x;\n"
        "  out[1] = depth.sample_compare(state, float2(0.25f), 0.5f, level(0.0f));\n"
        "}\n"
        "constexpr sampler static_never(coord::normalized,\n"
        "    address::clamp_to_edge, filter::nearest, compare_func::never);\n"
        "kernel void sampler_constexpr_never(texture2d<float> color [[texture(0)]],\n"
        "    depth2d<float> depth [[texture(1)]],\n"
        "    device float *out [[buffer(0)]]) {\n"
        "  out[0] = color.sample(static_never, float2(0.5f)).x;\n"
        "  out[1] = depth.sample_compare(static_never, float2(0.25f), 0.5f,\n"
        "      level(0.0f));\n"
        "}\n"
        "constexpr sampler static_always(coord::normalized,\n"
        "    address::clamp_to_edge, filter::nearest, compare_func::always);\n"
        "kernel void sampler_constexpr_always(texture2d<float> color [[texture(0)]],\n"
        "    depth2d<float> depth [[texture(1)]],\n"
        "    device float *out [[buffer(0)]]) {\n"
        "  out[0] = color.sample(static_always, float2(0.5f)).x;\n"
        "  out[1] = depth.sample_compare(static_always, float2(0.25f), 0.5f,\n"
        "      level(0.0f));\n"
        "}\n";
}

static NSString *invalidSource(void) {
    return @"#include <metal_stdlib>\n"
        "using namespace metal;\n"
        "constexpr sampler invalid(coord::pixel, address::repeat,\n"
        "    filter::nearest);\n"
        "kernel void invalid_sampler(texture2d<float> image [[texture(0)]],\n"
        "    device float *out [[buffer(0)]]) {\n"
        "  out[0] = image.sample(invalid, float2(1.0f)).x;\n"
        "}\n";
}

int main(int argc, const char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        puts("Usage: test-sampler-usage BUNDLE|--reference OUTPUT_DIR [--capture-only] "
             "[--case all|argument-mixed|argument-ordinary|argument-compare|"
             "direct|constexpr-never|constexpr-always]");
        return 0;
    }
    if (argc < 3) return 2;
    BOOL captureOnly = NO;
    const char *selectedCase = "all";
    for (int i = 3; i < argc; ++i) {
        if (!strcmp(argv[i], "--capture-only")) captureOnly = YES;
        else if (!strcmp(argv[i], "--case") && i + 1 < argc)
            selectedCase = argv[++i];
        else return 2;
    }
    BOOL all = !strcmp(selectedCase, "all");
    BOOL ordinary = all || !strcmp(selectedCase, "argument-mixed") ||
        !strcmp(selectedCase, "argument-ordinary");
    BOOL comparison = all || !strcmp(selectedCase, "argument-mixed") ||
        !strcmp(selectedCase, "argument-compare");
    BOOL mixed = ordinary || comparison;
    BOOL direct = all || !strcmp(selectedCase, "direct");
    BOOL never = all || !strcmp(selectedCase, "constexpr-never");
    BOOL always = all || !strcmp(selectedCase, "constexpr-always");
    if (!mixed && !direct && !never && !always) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(120);

    @autoreleasepool {
        NSError *error = nil;
        BOOL reference = !strcmp(argv[1], "--reference");
        NSString *directory = @(argv[2]);
        require([[NSFileManager defaultManager] createDirectoryAtPath:directory
            withIntermediateDirectories:YES attributes:nil error:&error],
            error.description ?: @"output directory unavailable");

        id<MTLDevice> device = nil;
        if (reference) device = MTLCreateSystemDefaultDevice();
        else {
            NSBundle *bundle = [NSBundle bundleWithPath:@(argv[1])];
            require([bundle loadAndReturnError:&error],
                error.description ?: @"bundle load failed");
            device = [[[bundle classNamed:@"GarnetDevice"] alloc] init];
        }
        require(device != nil, @"Metal device unavailable");

        NSString *source = validSource();
        if (mixed && !ordinary)
            source = [source stringByReplacingOccurrencesOfString:@"  out[2u * i] = color.x;\n"
                withString:@""];
        if (mixed && !comparison)
            source = [source stringByReplacingOccurrencesOfString:@"  out[2u * i + 1u] = pass;\n"
                withString:@""];
        require([source writeToFile:[directory stringByAppendingPathComponent:
            @"sampler-usage.metal"] atomically:YES encoding:NSUTF8StringEncoding
            error:&error], @"valid sampler source write failed");
        MTLCompileOptions *options = [MTLCompileOptions new];
        options.languageVersion = MTLLanguageVersion2_1;
        id<MTLLibrary> library = [device newLibraryWithSource:source options:options
            error:&error];
        require(library != nil, error.description ?: @"valid sampler source compilation failed");

        id<MTLFunction> mixedFunction = [library newFunctionWithName:@"sampler_mixed"];
        id<MTLFunction> directFunction = [library newFunctionWithName:@"sampler_direct"];
        id<MTLFunction> neverFunction =
            [library newFunctionWithName:@"sampler_constexpr_never"];
        id<MTLFunction> alwaysFunction =
            [library newFunctionWithName:@"sampler_constexpr_always"];
        require(mixedFunction != nil && directFunction != nil &&
            neverFunction != nil && alwaysFunction != nil,
            @"sampler function unavailable");
        writeAIR(mixedFunction, [directory stringByAppendingPathComponent:
            @"sampler-mixed.air"]);
        writeAIR(directFunction, [directory stringByAppendingPathComponent:
            @"sampler-direct.air"]);
        writeAIR(neverFunction, [directory stringByAppendingPathComponent:
            @"sampler-constexpr-never.air"]);
        writeAIR(alwaysFunction, [directory stringByAppendingPathComponent:
            @"sampler-constexpr-always.air"]);

        NSString *invalid = invalidSource();
        require([invalid writeToFile:[directory stringByAppendingPathComponent:
            @"sampler-usage-invalid.metal"] atomically:YES
            encoding:NSUTF8StringEncoding error:&error],
            @"invalid sampler source write failed");
        NSError *rejectError = nil;
        id<MTLLibrary> invalidLibrary = [device newLibraryWithSource:invalid
            options:options error:&rejectError];
        require(invalidLibrary == nil && rejectError != nil,
            @"constexpr pixel coordinate with repeat addressing was not rejected");
        printf("ARGUMENT_SAMPLER_USAGE_REJECT_PASS case=constexpr-pixel-repeat "
               "backend=%s\n", reference ? "reference" : "bundle");

        if (captureOnly) {
            printf("ARGUMENT_SAMPLER_USAGE_CAPTURE backend=%s air=4 "
                   "gpu_execution=0\n",
                reference ? "reference" : "bundle");
            return 0;
        }

        id<MTLArgumentEncoder> encoder = mixed ?
            [mixedFunction newArgumentEncoderWithBufferIndex:3] : nil;
        require(!mixed || (encoder != nil && encoder.encodedLength > 0),
            @"mixed argument encoder unavailable");
        id<MTLComputePipelineState> mixedPipeline = mixed ?
            newComputePipeline(device, mixedFunction, @"mixed sampler", &error) : nil;
        id<MTLComputePipelineState> directPipeline = direct ?
            newComputePipeline(device, directFunction, @"direct sampler", &error) : nil;
        id<MTLComputePipelineState> neverPipeline = never ?
            newComputePipeline(device, neverFunction, @"constexpr Never", &error) : nil;
        id<MTLComputePipelineState> alwaysPipeline = always ?
            newComputePipeline(device, alwaysFunction, @"constexpr Always", &error) : nil;
        id<MTLCommandQueue> queue = [device newCommandQueue];
        require(queue != nil, @"sampler command queue unavailable");

        NSMutableArray<id<MTLSamplerState>> *samplers = [NSMutableArray arrayWithCapacity:8];
        for (NSUInteger i = 0; i < 8; ++i)
            [samplers addObject:newSampler(device, (MTLCompareFunction)i,
                [NSString stringWithFormat:@"comparison sampler %lu",
                    (unsigned long)i])];
        id<MTLSamplerState> neverSampler =
            newSampler(device, MTLCompareFunctionNever, @"direct Never sampler");

        id<MTLTexture> color = newColorTexture(device);
        id<MTLBuffer> upload = nil;
        id<MTLTexture> depth = newDepthTexture(device, queue, &upload);
        if (mixed) runMixedCase(device, queue, mixedPipeline, encoder, color, depth,
            samplers, reference, ordinary, comparison);
        if (direct) runDirectCase(device, queue, directPipeline, color, depth,
            neverSampler, reference);
        if (never) runConstexprCase(device, queue, neverPipeline, color, depth,
            0.0f, "constexpr-never", reference);
        if (always) runConstexprCase(device, queue, alwaysPipeline, color, depth,
            1.0f, "constexpr-always", reference);
        require(upload.length > 0, @"depth upload was released early");

        printf("ARGUMENT_SAMPLER_USAGE_PASS backend=%s msl=2.1 case=%s "
               "mixed=%u direct=%u constexpr_never=%u constexpr_always=%u "
               "reject=compile guards=argument,output\n",
            reference ? "reference" : "bundle", selectedCase,
            (unsigned)mixed, (unsigned)direct, (unsigned)never, (unsigned)always);
    }
    return 0;
}
