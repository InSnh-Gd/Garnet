#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RDNA4_ARGUMENT_TEST_PREFIX "ARGUMENT_DEPTH_MIPS"
#include "test-argument-render-support.h"

typedef struct { float scale; uint32_t image, sampler; } DepthControl;

static NSString *depthSource(BOOL arrayed, BOOL levelZero, BOOL mixed) {
    NSMutableArray<NSString *> *samples = [NSMutableArray array];
    for (unsigned c = 0; c < 4; ++c) [samples addObject:[NSString stringWithFormat:
        @"args.images[image].sample_compare(args.filters[filter], position.xy * control.scale, %@reference + %.9ef%@, int2(1, -1))",
        arrayed ? @"layer, " : @"", (double)c / 8.0, levelZero ? @", level(0.0f)" : @""]];
    if (mixed) samples[0] = [NSString stringWithFormat:
        @"args.images[image].sample(args.filters[filter], position.xy * control.scale, %@int2(1, -1))",
        arrayed ? @"layer, " : @""];
    return [NSString stringWithFormat:
        @"#include <metal_stdlib>\nusing namespace metal;\n"
         "struct Args { array<%@<float>, 2> images [[id(0)]]; array<sampler, 2> filters [[id(2)]]; };\n"
         "struct Control { float scale; uint image; uint sampler; };\n"
         "vertex float4 depth_vertex(uint vid [[vertex_id]]) { float2 p = float2(float((vid << 1u) & 2u), float(vid & 2u)); return float4(p * 2.0f - 1.0f, 0, 1); }\n"
         "fragment float4 depth_fragment(float4 position [[position]], constant Args &args [[buffer(3)]], constant Control &control [[buffer(1)]]) { "
         "uint image = (uint(position.x) ^ control.image) & 1u; uint filter = (uint(position.y) ^ control.sampler) & 1u; "
         "%@"
         "float reference = (float((uint(position.x) - 2u) * 8u + uint(position.y) - 2u) + 0.25f) / 32.0f; "
         "return float4(%@); }\n", arrayed ? @"depth2d_array" : @"depth2d",
        arrayed ? @"uint layer = 1u + ((uint(position.x) ^ uint(position.y) ^ control.image) & 1u); " : @"",
        [samples componentsJoinedByString:@", "]];
}

static float depthValue(unsigned image, NSUInteger layer, NSUInteger mip, NSUInteger x, NSUInteger y) {
    return ((float)(12 * image + 4 * layer + mip + 1) + (float)((x + 2 * y) & 1u) * 0.5f) / 32.0f;
}

static id<MTLTexture> depthImage(id<MTLDevice> device, id<MTLCommandQueue> queue,
    NSUInteger width, NSUInteger height, unsigned image, BOOL arrayed) {
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
        MTLPixelFormatDepth32Float width:width height:height mipmapped:YES];
    descriptor.storageMode = MTLStorageModePrivate;
    descriptor.usage = MTLTextureUsageShaderRead;
    if (arrayed) { descriptor.textureType = MTLTextureType2DArray; descriptor.arrayLength = 3; }
    id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
    require(texture != nil, @"depth mip texture");
    NSUInteger length = 0;
    for (NSUInteger mip = 0; mip < texture.mipmapLevelCount; ++mip)
        length += alignUp(MAX((NSUInteger)1, width >> mip) * MAX((NSUInteger)1, height >> mip) * sizeof(float), 64);
    length *= texture.arrayLength;
    id<MTLBuffer> upload = newSharedBuffer(device, 64 + length + 64, 0x3c, @"depth mip upload");
    NSUInteger offset = 64;
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
    require(command && blit, @"depth mip upload encoder");
    for (NSUInteger layer = 0; layer < texture.arrayLength; ++layer)
        for (NSUInteger mip = 0; mip < texture.mipmapLevelCount; ++mip) {
            NSUInteger w = MAX((NSUInteger)1, width >> mip), h = MAX((NSUInteger)1, height >> mip);
            float *values = (float *)((uint8_t *)upload.contents + offset);
            for (NSUInteger y = 0; y < h; ++y) for (NSUInteger x = 0; x < w; ++x)
                values[y * w + x] = depthValue(image, layer, mip, x, y);
            [blit copyFromBuffer:upload sourceOffset:offset sourceBytesPerRow:w * sizeof(float)
                sourceBytesPerImage:w * h * sizeof(float) sourceSize:MTLSizeMake(w, h, 1)
                toTexture:texture destinationSlice:layer destinationLevel:mip destinationOrigin:MTLOriginMake(0, 0, 0)];
            offset += alignUp(w * h * sizeof(float), 64);
        }
    require(offset == 64 + length, @"depth mip upload span");
    NSData *snapshot = [NSData dataWithBytes:upload.contents length:upload.length];
    [blit endEncoding]; [command commit]; [command waitUntilCompleted];
    require(command.status == MTLCommandBufferStatusCompleted && !command.error,
        command.error.description ?: @"depth mip upload completion");
    require(!memcmp(upload.contents, snapshot.bytes, upload.length), @"depth upload bytes and guards unchanged");
    return texture;
}

static id<MTLSamplerState> depthSampler(id<MTLDevice> device, BOOL narrow) {
    MTLSamplerDescriptor *descriptor = [MTLSamplerDescriptor new];
    descriptor.normalizedCoordinates = YES;
    descriptor.minFilter = descriptor.magFilter = MTLSamplerMinMagFilterNearest;
    descriptor.mipFilter = MTLSamplerMipFilterNearest;
    descriptor.sAddressMode = descriptor.tAddressMode = descriptor.rAddressMode = MTLSamplerAddressModeClampToEdge;
    descriptor.compareFunction = narrow ? MTLCompareFunctionGreater : MTLCompareFunctionLess;
    descriptor.lodMinClamp = narrow ? 1 : 0;
    descriptor.lodMaxClamp = narrow ? 2 : 4;
    descriptor.supportArgumentBuffers = YES;
    id<MTLSamplerState> result = [device newSamplerStateWithDescriptor:descriptor];
    require(result != nil, @"depth mip sampler");
    return result;
}

static float expectedDepth(DepthControl control, BOOL arrayed, BOOL levelZero, BOOL mixed,
    const NSUInteger widths[2], const NSUInteger heights[2], unsigned x, unsigned y, unsigned channel) {
    unsigned image = (x ^ control.image) & 1u, sampler = (y ^ control.sampler) & 1u;
    NSUInteger layer = arrayed ? 1u + ((x ^ y ^ control.image) & 1u) : 0;
    double lod = levelZero ? 0 : log2((double)MAX(widths[image], heights[image]) * control.scale);
    lod = fmin(fmax(lod, sampler ? 1 : 0), sampler ? 2 : 4);
    NSUInteger mip = MIN((NSUInteger)floor(log2((double)MAX(widths[image], heights[image]))), (NSUInteger)floor(lod + 0.5));
    NSUInteger w = MAX((NSUInteger)1, widths[image] >> mip), h = MAX((NSUInteger)1, heights[image] >> mip);
    long px = (long)floorf(((float)x + 0.5f) * control.scale * (float)w) + 1;
    long py = (long)floorf(((float)y + 0.5f) * control.scale * (float)h) - 1;
    px = MIN(MAX(px, 0), (long)w - 1); py = MIN(MAX(py, 0), (long)h - 1);
    float depth = depthValue(image, layer, mip, (NSUInteger)px, (NSUInteger)py);
    if (mixed && channel == 0) return depth;
    float reference = ((float)((x - kViewportOrigin) * 8u + y - kViewportOrigin) + 0.25f) / 32.0f + (float)channel / 8.0f;
    return sampler ? reference > depth : reference < depth;
}

static void drawDepth(id<MTLDevice> device, id<MTLCommandQueue> queue, id<MTLRenderPipelineState> pipeline,
    id<MTLBuffer> arguments, NSUInteger argumentOffset, NSUInteger argumentLength, NSData *argumentSnapshot,
    id<MTLBuffer> controls, id<MTLTexture> __strong *images, const NSUInteger widths[2], const NSUInteger heights[2],
    const char *shape, BOOL arrayed, BOOL levelZero, BOOL mixed, DepthControl control) {
    memcpy((uint8_t *)controls.contents + kControlOffset, &control, sizeof control);
    NSData *snapshot = [NSData dataWithBytes:controls.contents length:controls.length];
    id<MTLTexture> target = newTarget(device);
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(kTargetGuard, kTargetGuard, kTargetGuard, kTargetGuard);
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLRenderCommandEncoder> draw = [command renderCommandEncoderWithDescriptor:pass];
    require(command && draw, @"depth mip draw encoder");
    [draw setRenderPipelineState:pipeline];
    [draw setViewport:(MTLViewport){kViewportOrigin, kViewportOrigin, kViewportSize, kViewportSize, 0, 1}];
    [draw setScissorRect:(MTLScissorRect){kViewportOrigin, kViewportOrigin, kViewportSize, kViewportSize}];
    [draw setFragmentBuffer:arguments offset:argumentOffset atIndex:3];
    [draw setFragmentBuffer:controls offset:kControlOffset atIndex:1];
    for (unsigned i = 0; i < 2; ++i) [draw useResource:images[i] usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
    [draw drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [draw endEncoding]; [command commit]; [command waitUntilCompleted];
    require(command.status == MTLCommandBufferStatusCompleted && !command.error,
        command.error.description ?: @"depth mip draw completion");
    requireGuard(arguments, argumentOffset, argumentLength, kArgumentGuard, @"depth mip argument guards");
    requireGuard(controls, kControlOffset, sizeof control, kControlGuard, @"depth mip control guards");
    require(!memcmp(arguments.contents, argumentSnapshot.bytes, arguments.length), @"depth mip arguments unchanged");
    require(!memcmp(controls.contents, snapshot.bytes, controls.length), @"depth mip controls unchanged");
    float pixels[kTargetSize * kTargetSize * 4];
    [target getBytes:pixels bytesPerRow:kTargetSize * 4 * sizeof(float)
        fromRegion:MTLRegionMake2D(0, 0, kTargetSize, kTargetSize) mipmapLevel:0];
    for (unsigned y = 0; y < kTargetSize; ++y) for (unsigned x = 0; x < kTargetSize; ++x)
        for (unsigned c = 0; c < 4; ++c) {
            BOOL inside = x >= kViewportOrigin && x < kViewportOrigin + kViewportSize &&
                y >= kViewportOrigin && y < kViewportOrigin + kViewportSize;
            float wanted = inside ? expectedDepth(control, arrayed, levelZero, mixed, widths, heights, x, y, c) : kTargetGuard;
            float actual = pixels[4 * (y * kTargetSize + x) + c];
            require(actual == wanted, [NSString stringWithFormat:
                @"shape=%s level0=%d scale=%g image=%u sampler=%u pixel=%u,%u channel=%u actual=%g expected=%g",
                shape, levelZero, control.scale, control.image, control.sampler, x, y, c, actual, wanted]);
        }
    printf("ARGUMENT_DEPTH_MIPS_DRAW_PASS shape=%s level0=%d scale=%g image=%u sampler=%u guards=exact gpu_execution=1\n",
        shape, levelZero, control.scale, control.image, control.sampler);
}

int main(int argc, const char **argv) {
    if (argc != 5 || (strcmp(argv[3], "pot") && strcmp(argv[3], "npot") &&
        strcmp(argv[3], "array-pot") && strcmp(argv[3], "array-npot")) ||
        (strcmp(argv[4], "implicit") && strcmp(argv[4], "level0") && strcmp(argv[4], "mixed"))) return 2;
    setvbuf(stdout, NULL, _IONBF, 0); alarm(110);
    @autoreleasepool {
        BOOL reference = !strcmp(argv[1], "--reference"), arrayed = !strncmp(argv[3], "array-", 6);
        BOOL npot = strstr(argv[3], "npot") != NULL, levelZero = !strcmp(argv[4], "level0");
        BOOL mixed = !strcmp(argv[4], "mixed");
        NSError *error = nil;
        id<MTLDevice> device;
        if (reference) device = MTLCreateSystemDefaultDevice();
        else {
            NSBundle *bundle = [NSBundle bundleWithPath:@(argv[1])];
            require([bundle loadAndReturnError:&error], error.description ?: @"depth mip bundle load");
            device = [[[bundle classNamed:@"GarnetDevice"] alloc] init];
        }
        require(device != nil, @"depth mip device");
        NSString *directory = @(argv[2]), *source = depthSource(arrayed, levelZero, mixed);
        require([[NSFileManager defaultManager] createDirectoryAtPath:directory withIntermediateDirectories:YES
            attributes:nil error:&error], @"depth mip output directory");
        require([source writeToFile:[directory stringByAppendingPathComponent:@"source.metal"] atomically:YES
            encoding:NSUTF8StringEncoding error:&error], @"depth mip source capture");
        MTLCompileOptions *options = [MTLCompileOptions new]; options.languageVersion = MTLLanguageVersion2_1;
        id<MTLLibrary> library = [device newLibraryWithSource:source options:options error:&error];
        require(library != nil, error.description ?: @"depth mip source compile");
        id<MTLFunction> vertex = [library newFunctionWithName:@"depth_vertex"];
        id<MTLFunction> fragment = [library newFunctionWithName:@"depth_fragment"];
        require(vertex && fragment, @"depth mip functions");
        writeAIR(vertex, directory); writeAIR(fragment, directory);
        id<MTLRenderPipelineState> pipeline = newPipeline(device, vertex, fragment, @"depth mip pipeline", &error);
        id<MTLCommandQueue> queue = [device newCommandQueue]; require(queue != nil, @"depth mip queue");
        id<MTLArgumentEncoder> encoder = [fragment newArgumentEncoderWithBufferIndex:3];
        require(encoder && encoder.encodedLength && encoder.alignment, @"depth mip argument encoder");
        NSUInteger offset = alignUp(kArgumentOffset, encoder.alignment);
        id<MTLBuffer> arguments = newSharedBuffer(device, offset + encoder.encodedLength + kArgumentTail,
            kArgumentGuard, @"depth mip arguments");
        NSUInteger widths[] = {npot ? 13 : 8, npot ? 23 : 16}, heights[] = {npot ? 7 : 8, npot ? 11 : 16};
        id<MTLTexture> images[] = {depthImage(device, queue, widths[0], heights[0], 0, arrayed),
            depthImage(device, queue, widths[1], heights[1], 1, arrayed)};
        id<MTLSamplerState> samplers[] = {depthSampler(device, NO), depthSampler(device, YES)};
        [encoder setArgumentBuffer:arguments offset:offset];
        [encoder setTextures:images withRange:NSMakeRange(0, 2)];
        [encoder setSamplerStates:samplers withRange:NSMakeRange(2, 2)];
        NSData *snapshot = [NSData dataWithBytes:arguments.contents length:arguments.length];
        id<MTLBuffer> controls = newSharedBuffer(device, kControlOffset + sizeof(DepthControl) + kControlTail,
            kControlGuard, @"depth mip controls");
        const float scales[] = {0.15f, 0.3f};
        for (unsigned s = 0; s < 2; ++s) for (uint32_t image = 0; image < 2; ++image)
            for (uint32_t sampler = 0; sampler < 2; ++sampler) {
                @autoreleasepool {
                    drawDepth(device, queue, pipeline, arguments, offset, encoder.encodedLength, snapshot,
                        controls, images, widths, heights, argv[3], arrayed, levelZero, mixed,
                        (DepthControl){scales[s], image, sampler});
                }
            }
        if (mixed) printf("ARGUMENT_DEPTH_MIXED_TOKEN ordinary=1 comparison=3 shared_dynamic_token=1\n");
        printf("ARGUMENT_DEPTH_MIPS_PASS backend=%s shape=%s mode=%s draws=8 msl=2.1 cpu_oracle=1 compare=less,greater sampler_clamps=0:4,1:2 offset=1:-1 layers=%s guards=target,argument,control,upload gpu_execution=1\n",
            reference ? "reference" : "bundle", argv[3], argv[4], arrayed ? "1:2" : "0");
    }
    return 0;
}
