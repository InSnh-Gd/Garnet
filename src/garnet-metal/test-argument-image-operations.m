#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// MSL 2.1 image operations. A PASS is printed only after GPU completion and exact oracles.
static const NSUInteger kGuard = 256;
static const uint8_t kCanary = 0xa5;

typedef struct {
    const char *name;
    MTLTextureType type;
    BOOL arrayed;
    BOOL cube;
    BOOL depth;
    BOOL multisample;
} Shape;

static const Shape kShapes[] = {
    {"1d", MTLTextureType1D, NO, NO, NO, NO},
    {"1d_array", MTLTextureType1DArray, YES, NO, NO, NO},
    {"2d", MTLTextureType2D, NO, NO, NO, NO},
    {"2d_array", MTLTextureType2DArray, YES, NO, NO, NO},
    {"3d", MTLTextureType3D, NO, NO, NO, NO},
    {"cube", MTLTextureTypeCube, NO, YES, NO, NO},
    {"cube_array", MTLTextureTypeCubeArray, YES, YES, NO, NO},
    {"2d", MTLTextureType2D, NO, NO, YES, NO},
    {"2d_array", MTLTextureType2DArray, YES, NO, YES, NO},
    {"cube", MTLTextureTypeCube, NO, YES, YES, NO},
    {"cube_array", MTLTextureTypeCubeArray, YES, YES, YES, NO},
    {"2d_ms", MTLTextureType2DMultisample, NO, NO, NO, YES},
    {"2d_ms", MTLTextureType2DMultisample, NO, NO, YES, YES},
};

static void check(BOOL ok, NSString *message) {
    if (!ok) {
        fprintf(stderr, "ARGUMENT_IMAGE_OPERATIONS_FAIL %s\n", message.UTF8String);
        exit(1);
    }
}

static void finish(id<MTLCommandBuffer> command, NSString *label) {
    check(command != nil, [label stringByAppendingString:@" command unavailable"]);
    [command commit];
    [command waitUntilCompleted];
    check(command.status == MTLCommandBufferStatusCompleted && command.error == nil,
        command.error.description ?: [label stringByAppendingString:@" GPU failed"]);
}

static id<MTLBuffer> buffer(id<MTLDevice> device, NSUInteger payload) {
    id<MTLBuffer> result = [device newBufferWithLength:kGuard + payload + kGuard
        options:MTLResourceStorageModeShared];
    check(result != nil && result.contents != NULL, @"guarded shared buffer unavailable");
    memset(result.contents, kCanary, result.length);
    return result;
}

static void guards(id<MTLBuffer> value, NSUInteger payload, NSString *label) {
    const uint8_t *bytes = value.contents;
    for (NSUInteger i = 0; i < value.length; ++i) {
        if (i < kGuard || i >= kGuard + payload)
            check(bytes[i] == kCanary, [NSString stringWithFormat:@"%@ guard changed at %lu",
                label, (unsigned long)i]);
    }
}

static float texel(const Shape *shape, unsigned kind, NSUInteger resource,
                   NSUInteger mip, NSUInteger slice, NSUInteger x, NSUInteger y,
                   NSUInteger z, NSUInteger sample) {
    if (shape->depth)
        return (float)(1 + resource * 64 + mip * 16 + slice * 4 + y * 2 + x + sample * 8) / 256.0f;
    float value = (float)(1 + resource * 10000 + mip * 1000 + slice * 100 + z * 16 + y * 4 + x + sample * 64);
    return kind == 2 ? -value : value;
}

static void storeTexel(void *destination, unsigned kind, float value) {
    if (kind == 0) *(float *)destination = value;
    else if (kind == 1) *(uint32_t *)destination = (uint32_t)value;
    else *(int32_t *)destination = (int32_t)value;
}

static float loadTexel(const void *source, unsigned kind) {
    if (kind == 0) return *(const float *)source;
    if (kind == 1) return (float)*(const uint32_t *)source;
    return (float)*(const int32_t *)source;
}

static NSUInteger slices(const Shape *shape) {
    return (shape->cube ? 6u : 1u) * (shape->arrayed ? 2u : 1u);
}

static BOOL oneDimensional(const Shape *shape) {
    return shape->type == MTLTextureType1D || shape->type == MTLTextureType1DArray;
}

static id<MTLTexture> texture(id<MTLDevice> device, const Shape *shape, unsigned kind,
                              BOOL writable) {
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor new];
    descriptor.textureType = shape->type;
    descriptor.pixelFormat = shape->depth ? MTLPixelFormatDepth32Float :
        (kind == 0 ? MTLPixelFormatR32Float : kind == 1 ? MTLPixelFormatR32Uint : MTLPixelFormatR32Sint);
    descriptor.width = 4;
    descriptor.height = oneDimensional(shape) ? 1 : 4;
    descriptor.depth = shape->type == MTLTextureType3D ? 4 : 1;
    descriptor.arrayLength = shape->arrayed ? 2 : 1;
    descriptor.mipmapLevelCount = oneDimensional(shape) || shape->multisample || writable ? 1 : 2;
    descriptor.sampleCount = shape->multisample ? 4 : 1;
    descriptor.storageMode = MTLStorageModePrivate;
    descriptor.usage = MTLTextureUsageShaderRead |
        (writable ? MTLTextureUsageShaderWrite : 0) |
        (shape->multisample ? MTLTextureUsageRenderTarget : 0);
    id<MTLTexture> result = [device newTextureWithDescriptor:descriptor];
    check(result != nil, [NSString stringWithFormat:@"%s kind=%u texture unavailable",
        shape->name, kind]);
    return result;
}

static void upload(id<MTLDevice> device, id<MTLCommandQueue> queue, id<MTLTexture> image,
                   const Shape *shape, unsigned kind, NSUInteger resource, BOOL writable) {
    // Private depth and color textures share the same staged upload; no replaceRegion on depth.
    for (NSUInteger mip = 0; mip < image.mipmapLevelCount; ++mip) {
        NSUInteger width = image.width >> mip;
        NSUInteger height = image.height >> mip;
        NSUInteger depth = image.depth >> mip;
        if (height == 0) height = 1;
        if (depth == 0) depth = 1;
        NSUInteger rowBytes = 256;
        NSUInteger imageBytes = rowBytes * height;
        NSUInteger length = imageBytes * depth * slices(shape);
        id<MTLBuffer> staging = buffer(device, length);
        for (NSUInteger slice = 0; slice < slices(shape); ++slice)
            for (NSUInteger z = 0; z < depth; ++z)
                for (NSUInteger y = 0; y < height; ++y)
                    for (NSUInteger x = 0; x < width; ++x) {
                        float value = writable ? (kind == 2 ? -9.0f : 9.0f) :
                            texel(shape, kind, resource, mip, slice, x, y, z, 0);
                        storeTexel((uint8_t *)staging.contents + kGuard + slice * imageBytes * depth +
                            z * imageBytes + y * rowBytes + x * 4, kind, value);
                    }
        id<MTLCommandBuffer> command = [queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
        check(blit != nil, @"staged upload encoder unavailable");
        for (NSUInteger slice = 0; slice < slices(shape); ++slice)
            [blit copyFromBuffer:staging sourceOffset:kGuard + slice * imageBytes * depth
                sourceBytesPerRow:rowBytes sourceBytesPerImage:imageBytes
                sourceSize:MTLSizeMake(width, height, depth) toTexture:image
                destinationSlice:slice destinationLevel:mip destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit endEncoding];
        finish(command, @"staged image upload");
        guards(staging, length, @"upload");
    }
}

static void verifyWrites(id<MTLDevice> device, id<MTLCommandQueue> queue,
                         id<MTLTexture> image, const Shape *shape, unsigned kind,
                         BOOL selected) {
    NSUInteger rowBytes = 256;
    NSUInteger imageBytes = rowBytes * image.height;
    NSUInteger length = imageBytes * image.depth * slices(shape);
    id<MTLBuffer> readback = buffer(device, length);
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
    check(blit != nil, @"write readback encoder unavailable");
    for (NSUInteger slice = 0; slice < slices(shape); ++slice)
        [blit copyFromTexture:image sourceSlice:slice sourceLevel:0
            sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(image.width, image.height, image.depth)
            toBuffer:readback destinationOffset:kGuard + slice * imageBytes * image.depth
            destinationBytesPerRow:rowBytes destinationBytesPerImage:imageBytes];
    [blit endEncoding];
    finish(command, @"selected write readback");
    guards(readback, length, @"write readback");
    NSUInteger targetSlice = shape->cube ? (shape->arrayed ? 8 : 2) : (shape->arrayed ? 1 : 0);
    for (NSUInteger slice = 0; slice < slices(shape); ++slice)
        for (NSUInteger z = 0; z < image.depth; ++z)
            for (NSUInteger y = 0; y < image.height; ++y)
                for (NSUInteger x = 0; x < image.width; ++x) {
                    BOOL target = selected && slice == targetSlice && x == 2 &&
                        y == (oneDimensional(shape) ? 0u : 1u) &&
                        z == (shape->type == MTLTextureType3D ? 2u : 0u);
                    float expected = target ? (kind == 2 ? -77.0f : 77.0f) : (kind == 2 ? -9.0f : 9.0f);
                    float actual = loadTexel((uint8_t *)readback.contents + kGuard +
                        slice * imageBytes * image.depth + z * imageBytes + y * rowBytes + x * 4, kind);
                    check(actual == expected, [NSString stringWithFormat:
                        @"selected=%d %s write side effect slice=%lu xyz=%lu,%lu,%lu expected=%g actual=%g",
                        selected, shape->name, (unsigned long)slice, (unsigned long)x,
                        (unsigned long)y, (unsigned long)z, expected, actual]);
                }
}

static NSString *shader(const Shape *shape, unsigned kind, BOOL writable, uint32_t operations,
                        BOOL directRead) {
    NSString *scalar = shape->depth || kind == 0 ? @"float" : kind == 1 ? @"uint" : @"int";
    NSString *type = [NSString stringWithFormat:@"%s%s<%@%@>",
        shape->depth ? "depth" : "texture", shape->name, scalar,
        shape->multisample ? @", access::read" : @""];
    NSString *layer = shape->arrayed ? @", 1u" : @"";
    NSString *coord = oneDimensional(shape) ? @"2u" :
        shape->type == MTLTextureType3D ? @"uint3(2,1,2)" : @"uint2(2,1)";
    NSString *read = [NSString stringWithFormat:@"%@.read(%@%@%@, %uu)",
        directRead ? @"direct" : @"a.src[s]", coord, shape->cube ? @", 2u" : @"", layer,
        shape->multisample ? 2 : 0];
    NSString *sample = @"0.0f";
    NSString *gradient = @"0.0f";
    NSString *compare = @"0.0f";
    NSString *levelOne = @"0.0f";
    NSString *offset = @"0.0f";
    if (!shape->multisample) {
        NSString *uv = oneDimensional(shape) ? @"0.625f" :
            shape->cube ? @"float3(0.25f,1.0f,-0.25f)" :
            shape->type == MTLTextureType3D ? @"float3(0.625f,0.375f,0.625f)" :
            @"float2(0.625f,0.375f)";
        sample = [NSString stringWithFormat:@"a.src[s].sample(a.filter, %@%@%@)%@",
            uv, layer, oneDimensional(shape) ? @"" : @", level(0.0f)", shape->depth ? @"" : @".x"];
        if (!oneDimensional(shape))
            levelOne = [NSString stringWithFormat:@"a.src[s].sample(a.filter, %@%@, level(1.0f))%@",
                uv, layer, shape->depth ? @"" : @".x"];
        if (!shape->cube && !oneDimensional(shape) && kind == 0) {
            NSString *derivatives = shape->type == MTLTextureType3D ?
                @"gradient3d(float3(0.001f,0.002f,0.003f),float3(0.003f,0.001f,0.002f))" :
                @"gradient2d(float2(0.001f,0.002f),float2(0.002f,0.001f))";
            gradient = [NSString stringWithFormat:@"a.src[s].sample(a.filter, %@%@, %@)%@",
                uv, layer, derivatives, shape->depth ? @"" : @".x"];
        }
        if (!shape->cube && !oneDimensional(shape) && shape->type != MTLTextureType3D)
            offset = [NSString stringWithFormat:@"a.src[s].sample(a.filter, %@%@, level(0.0f), int2(1,-1))%@",
                uv, layer, shape->depth ? @"" : @".x"];
        if (shape->cube && kind == 0)
            gradient = [NSString stringWithFormat:@"a.src[s].sample(a.filter, %@%@, "
                "gradientcube(float3(0.001f,0.002f,0.003f),float3(0.003f,0.001f,0.002f)))%@",
                uv, layer, shape->depth ? @"" : @".x"];
        if (shape->depth)
            compare = [NSString stringWithFormat:@"a.src[s].sample_compare(a.filter, %@%@, 0.25f, level(0.0f))", uv, layer];
    }
    NSString *writeField = writable ? [NSString stringWithFormat:
        @"array<texture%s<%@, access::write>, 2> dst [[id(2)]];", shape->name, scalar] : @"";
    NSString *write = writable && (operations & (1u << 12)) ? [NSString stringWithFormat:
        @"a.dst[s].write(%@4(%@), %@%@%@, 0u);", scalar, kind == 2 ? @"-77" : @"77", coord,
        shape->cube ? @", 2u" : @"", layer] : @"";
    NSArray<NSString *> *values = @[
         [read stringByAppendingString:shape->depth ? @"" : @".x"], sample, gradient, compare,
         @"a.src[s].get_width()",
         oneDimensional(shape) ? @"1u" : @"a.src[s].get_height()",
         shape->arrayed ? @"a.src[s].get_array_size()" : @"1u",
         shape->multisample ? @"a.src[s].get_num_samples()" : @"a.src[s].get_num_mip_levels()",
         shape->type == MTLTextureType3D ? @"a.src[s].get_depth()" : @"1u", levelOne,
         oneDimensional(shape) || shape->multisample ? @"0u" : @"a.src[s].get_width(1u)", offset];
    NSMutableString *body = [NSMutableString string];
    for (NSUInteger i = 0; i < values.count; ++i)
        if (operations & (1u << i))
            [body appendFormat:@"out[%lu] = float(%@); ", (unsigned long)i, values[i]];
    return [NSString stringWithFormat:
        @"#include <metal_stdlib>\nusing namespace metal;\n"
         "struct Args { array<%@, 2> src [[id(0)]]; %@ %@ };\n"
         "kernel void image_ops(constant Args &a [[buffer(3)]], device float *out [[buffer(0)]], "
         "constant uint &select [[buffer(1)]]%@) { uint s = select & 1u; %@ %@ }\n",
         type, writeField, shape->multisample ? @"" : @"sampler filter [[id(4)]];",
         directRead ? [NSString stringWithFormat:@", %@ direct [[texture(0)]]", type] : @"",
         body, write];
}

static void populateMultisample(id<MTLDevice> device, id<MTLCommandQueue> queue,
                                id<MTLTexture> image, const Shape *shape,
                                unsigned kind, NSUInteger resource) {
    NSString *scalar = kind == 0 || shape->depth ? @"float" : kind == 1 ? @"uint" : @"int";
    NSString *value = shape->depth ?
        @"float v = (1.0f + float(c.x)*64.0f + c.z*2.0f + c.y + c.w*8.0f)/256.0f;" :
        [NSString stringWithFormat:@"%@ v = %@(%s(1u + c.x*10000u + c.z*4u + c.y + c.w*64u));",
            scalar, scalar, kind == 2 ? "-int" : "uint"];
    NSString *result = [NSString stringWithFormat:
        @"struct Result { %@; uint mask [[sample_mask]]; };",
        shape->depth ? @"float depth [[depth(any)]]" :
            [NSString stringWithFormat:@"%@4 color [[color(0)]]", scalar]];
    NSString *ret = shape->depth ? @"result.depth=v;" :
        [NSString stringWithFormat:@"result.color=%@4(v,0,0,1);", scalar];
    NSString *source = [NSString stringWithFormat:
        @"#include <metal_stdlib>\nusing namespace metal;\n%@\n"
         "vertex float4 full(uint v [[vertex_id]]) { float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)}; return float4(p[v],0,1); }\n"
         "fragment Result fill(constant uint4 &c [[buffer(0)]]) { %@ Result result; %@ result.mask=1u<<c.w; return result; }\n",
         result, value, ret];
    NSError *error = nil;
    MTLCompileOptions *options = [MTLCompileOptions new];
    options.languageVersion = MTLLanguageVersion2_1;
    id<MTLLibrary> library = [device newLibraryWithSource:source options:options error:&error];
    check(library != nil, error.description ?: @"MS population shader failed");
    MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = [library newFunctionWithName:@"full"];
    descriptor.fragmentFunction = [library newFunctionWithName:@"fill"];
    descriptor.rasterSampleCount = 4;
    if (shape->depth) descriptor.depthAttachmentPixelFormat = image.pixelFormat;
    else descriptor.colorAttachments[0].pixelFormat = image.pixelFormat;
    id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
    check(pipeline != nil, error.description ?: @"MS population pipeline failed");
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    if (shape->depth) {
        pass.depthAttachment.texture = image;
        pass.depthAttachment.loadAction = MTLLoadActionClear;
        pass.depthAttachment.storeAction = MTLStoreActionStore;
        pass.depthAttachment.clearDepth = 1.0;
    } else {
        pass.colorAttachments[0].texture = image;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    }
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
    check(encoder != nil, @"MS render encoder unavailable");
    [encoder setRenderPipelineState:pipeline];
    if (shape->depth) {
        MTLDepthStencilDescriptor *depth = [MTLDepthStencilDescriptor new];
        depth.depthCompareFunction = MTLCompareFunctionAlways;
        depth.depthWriteEnabled = YES;
        id<MTLDepthStencilState> state = [device newDepthStencilStateWithDescriptor:depth];
        check(state != nil, @"MS depth state unavailable");
        [encoder setDepthStencilState:state];
    }
    // Fixed coverage keeps the per-sample oracle independent of sample-position support.
    for (NSUInteger y = 0; y < image.height; ++y)
        for (NSUInteger x = 0; x < image.width; ++x)
            for (uint32_t sample = 0; sample < 4; ++sample) {
                uint32_t control[] = {(uint32_t)resource, (uint32_t)x, (uint32_t)y, sample};
                [encoder setScissorRect:(MTLScissorRect){x, y, 1, 1}];
                [encoder setFragmentBytes:control length:sizeof(control) atIndex:0];
                [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            }
    [encoder endEncoding];
    finish(command, @"per-sample population");
}

static NSString *caseLabel(const Shape *shape, unsigned kind) {
    return [NSString stringWithFormat:@"%s-%s-%u", shape->depth ? "depth" : "color", shape->name, kind];
}

static void runCase(id<MTLDevice> device, id<MTLCommandQueue> queue, const Shape *shape,
                    unsigned kind, NSString *directory, BOOL compileOnly, uint32_t operations,
                    BOOL directRead) {
    BOOL writable = !shape->depth && !shape->multisample;
    if (operations != 0x1fffu) {
        if (operations == (1u << 12)) check(writable, @"shape has no write operation");
        if (operations == (1u << 3)) check(shape->depth && !shape->multisample,
            @"shape has no comparison operation");
        if (operations == (1u << 2)) check(!oneDimensional(shape) && !shape->multisample && kind == 0,
            @"shape has no gradient operation");
        if (operations == (1u << 1)) check(!shape->multisample, @"shape has no sample operation");
        if (operations == (1u << 9)) check(!oneDimensional(shape) && !shape->multisample,
            @"shape has no level operation");
        if (operations == (1u << 11)) check(!oneDimensional(shape) && !shape->multisample &&
            !shape->cube && shape->type != MTLTextureType3D, @"shape has no offset operation");
    }
    NSString *label = caseLabel(shape, kind);
    printf("ARGUMENT_IMAGE_OPERATIONS_CASE name=%s mode=%s\n", label.UTF8String,
        compileOnly ? "compile-only" : "execute");
    fflush(stdout);
    NSString *source = shader(shape, kind, writable, operations, directRead);
    NSError *error = nil;
    check([source writeToFile:[directory stringByAppendingPathComponent:[label stringByAppendingString:@".metal"]]
        atomically:YES encoding:NSUTF8StringEncoding error:&error], @"MSL source write failed");
    MTLCompileOptions *options = [MTLCompileOptions new];
    options.languageVersion = MTLLanguageVersion2_1;
    id<MTLLibrary> library = [device newLibraryWithSource:source options:options error:&error];
    check(library != nil, [NSString stringWithFormat:@"%@ %@", label, error.description ?: @"MSL compile failed"]);
    id<MTLFunction> function = [library newFunctionWithName:@"image_ops"];
    check(function != nil, @"image operation function unavailable");
    SEL selector = NSSelectorFromString(@"bitcodeData");
    check(!compileOnly || [function respondsToSelector:selector], @"AIR capture selector unavailable");
    if ([function respondsToSelector:selector]) {
        NSData *air = ((id (*)(id, SEL))objc_msgSend)(function, selector);
        check(air.length != 0 && [air writeToFile:[directory stringByAppendingPathComponent:
            [label stringByAppendingString:@".air"]] atomically:YES], @"AIR capture failed");
    }
    if (compileOnly) return;
    check(!shape->multisample || [device supportsTextureSampleCount:4], @"4-sample device unavailable");
    id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function error:&error];
    check(pipeline != nil, error.description ?: @"image compute pipeline unavailable");
    BOOL hasArguments = !directRead || operations != 1u;
    id<MTLArgumentEncoder> arguments = hasArguments ? [function newArgumentEncoderWithBufferIndex:3] : nil;
    // Argument ids can have gaps; AIR packs the two depth textures and sampler into 24 bytes.
    NSUInteger argumentBytes = shape->multisample ? 16u : writable ? 40u : 24u;
    check(!hasArguments || (arguments != nil && arguments.encodedLength >= argumentBytes && arguments.alignment != 0 && arguments.alignment <= kGuard &&
        kGuard % arguments.alignment == 0), @"image argument layout/alignment invalid");
    MTLSamplerDescriptor *samplerDescriptor = [MTLSamplerDescriptor new];
    samplerDescriptor.normalizedCoordinates = YES;
    samplerDescriptor.minFilter = MTLSamplerMinMagFilterNearest;
    samplerDescriptor.magFilter = MTLSamplerMinMagFilterNearest;
    samplerDescriptor.mipFilter = MTLSamplerMipFilterNearest;
    samplerDescriptor.sAddressMode = MTLSamplerAddressModeClampToEdge;
    samplerDescriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
    samplerDescriptor.rAddressMode = MTLSamplerAddressModeClampToEdge;
    samplerDescriptor.compareFunction = shape->depth ? MTLCompareFunctionLess : MTLCompareFunctionNever;
    samplerDescriptor.supportArgumentBuffers = YES;
    id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:samplerDescriptor];
    check(sampler != nil, @"image sampler unavailable");
    id<MTLTexture> inputs[2];
    id<MTLTexture> outputs[2] = {nil, nil};
    for (NSUInteger r = 0; r < 2; ++r) {
        inputs[r] = texture(device, shape, kind, NO);
        if (shape->multisample) populateMultisample(device, queue, inputs[r], shape, kind, r);
        else upload(device, queue, inputs[r], shape, kind, r, NO);
        if (writable) outputs[r] = texture(device, shape, kind, YES);
    }
    id<MTLBuffer> encoded = hasArguments ? buffer(device, arguments.encodedLength) : nil;
    [arguments setArgumentBuffer:encoded offset:kGuard];
    for (NSUInteger r = 0; r < 2; ++r) {
        [arguments setTexture:inputs[r] atIndex:r];
        if (writable) [arguments setTexture:outputs[r] atIndex:2 + r];
    }
    if (!shape->multisample) [arguments setSamplerState:sampler atIndex:4];
    for (uint32_t selected = 0; selected < 2; ++selected) {
        if (writable)
            for (NSUInteger r = 0; r < 2; ++r) upload(device, queue, outputs[r], shape, kind, r, YES);
        id<MTLBuffer> observation = buffer(device, 12 * sizeof(float));
        id<MTLBuffer> selection = buffer(device, sizeof(selected));
        memcpy((uint8_t *)selection.contents + kGuard, &selected, sizeof(selected));
        id<MTLCommandBuffer> command = [queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
        check(encoder != nil, @"image compute encoder unavailable");
        [encoder setComputePipelineState:pipeline];
        if (hasArguments) [encoder setBuffer:encoded offset:kGuard atIndex:3];
        [encoder setBuffer:observation offset:kGuard atIndex:0];
        [encoder setBuffer:selection offset:kGuard atIndex:1];
        if (directRead) [encoder setTexture:inputs[selected] atIndex:0];
        for (NSUInteger r = 0; r < 2; ++r) {
            [encoder useResource:inputs[r] usage:MTLResourceUsageRead];
            if (writable) [encoder useResource:outputs[r] usage:MTLResourceUsageWrite];
        }
        [encoder dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        [encoder endEncoding];
        finish(command, label);
        NSUInteger slice = shape->cube ? (shape->arrayed ? 8 : 2) : (shape->arrayed ? 1 : 0);
        NSUInteger y = oneDimensional(shape) ? 0 : 1;
        NSUInteger z = shape->type == MTLTextureType3D ? 2 : 0;
        float read = texel(shape, kind, selected, 0, slice, 2, y, z, shape->multisample ? 2 : 0);
        // +Y cube direction (0.25,1,-0.25) maps to texel (2,1) in the 4x4 face.
        float sampled = shape->multisample ? 0 : texel(shape, kind, selected, 0, slice, 2, y, z, 0);
        float expected[12] = {read, sampled,
            !oneDimensional(shape) && !shape->multisample && kind == 0 ? sampled : 0,
            shape->depth && !shape->multisample ? (0.25f < sampled ? 1.0f : 0.0f) : 0,
            4, oneDimensional(shape) ? 1 : 4, shape->arrayed ? 2 : 1,
            shape->multisample ? 4 : oneDimensional(shape) ? 0 : 2,
            shape->type == MTLTextureType3D ? 4 : 1,
            oneDimensional(shape) || shape->multisample ? 0 : texel(shape, kind, selected, 1,
                slice, 1, 0, shape->type == MTLTextureType3D ? 1 : 0, 0),
            oneDimensional(shape) || shape->multisample ? 0 : 2,
            shape->cube || oneDimensional(shape) || shape->multisample || shape->type == MTLTextureType3D ?
                0 : texel(shape, kind, selected, 0, slice, 3, 0, 0, 0)};
        const float *actual = (const float *)((uint8_t *)observation.contents + kGuard);
        for (NSUInteger i = 0; i < 12; ++i) {
            if (!(operations & (1u << i))) {
                const uint8_t *bytes = (const uint8_t *)&actual[i];
                for (NSUInteger b = 0; b < sizeof(float); ++b)
                    check(bytes[b] == kCanary, @"unselected image output changed");
                continue;
            }
            check(actual[i] == expected[i], [NSString stringWithFormat:
                @"%@ selected=%u output=%lu expected=%g actual=%g", label, selected,
                (unsigned long)i, expected[i], actual[i]]);
        }
        guards(observation, 12 * sizeof(float), @"output");
        guards(selection, sizeof(selected), @"selection");
        if (hasArguments) guards(encoded, arguments.encodedLength, @"argument");
        check(memcmp((uint8_t *)selection.contents + kGuard, &selected, sizeof(selected)) == 0,
            @"selection payload changed");
        if (writable)
            for (NSUInteger r = 0; r < 2; ++r) verifyWrites(device, queue, outputs[r], shape, kind,
                r == selected && (operations & (1u << 12)));
    }
    printf("ARGUMENT_IMAGE_CASE_PASS shape=%s depth=%d kind=%u selections=2 nonzero_coordinates=1 sample=%u write_guards=%d direct_read=%d gpu_execution=1\n",
        shape->name, shape->depth, kind, shape->multisample ? 2 : 0, writable, directRead);
}

#include "test-cube-read-grid.inc"
#include "test-null-image-guards.inc"

int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc == 2 && strcmp(argv[1], "--list") == 0) {
            for (NSUInteger s = 0; s < sizeof(kShapes) / sizeof(kShapes[0]); ++s)
                for (unsigned kind = 0; kind < (kShapes[s].depth ? 1u : 3u); ++kind)
                    puts(caseLabel(&kShapes[s], kind).UTF8String);
            return 0;
        }
        check(argc >= 4 && (strcmp(argv[1], "--bundle") == 0 || strcmp(argv[1], "--reference") == 0),
            @"usage: test-argument-image-operations --bundle BUNDLE OUTPUT | --reference - OUTPUT [--case NAME] [--operation all|read|read-grid|sample|gradient|compare|query|level|offset|write] [--direct-ms-read|--null-guard|--null-sampling never|less|always] [--compile-only]; --list");
        NSString *selectedCase = nil;
        NSString *operation = @"all";
        uint32_t operations = 0x1fffu;
        BOOL compileOnly = NO;
        BOOL directRead = NO;
        BOOL nullGuard = NO;
        BOOL nullSampling = NO;
        MTLCompareFunction nullCompare = MTLCompareFunctionLess;
        for (int i = 4; i < argc; ++i) {
            if (strcmp(argv[i], "--compile-only") == 0 && !compileOnly) compileOnly = YES;
            else if (strcmp(argv[i], "--direct-ms-read") == 0 && !directRead) directRead = YES;
            else if (strcmp(argv[i], "--null-guard") == 0 && !nullGuard) nullGuard = YES;
            else if (strcmp(argv[i], "--null-sampling") == 0 && !nullSampling && i+1<argc) {
                NSDictionary *functions = @{@"never": @(MTLCompareFunctionNever),
                    @"less": @(MTLCompareFunctionLess), @"always": @(MTLCompareFunctionAlways)};
                NSNumber *compare = functions[@(argv[++i])];
                check(compare != nil, @"null sampling comparison must be never, less or always");
                nullCompare = compare.unsignedIntegerValue;
                nullSampling = YES;
            }
            else if (strcmp(argv[i], "--case") == 0 && !selectedCase && i + 1 < argc)
                selectedCase = @(argv[++i]);
            else if (strcmp(argv[i], "--operation") == 0 && i + 1 < argc) {
                operation = @(argv[++i]);
                NSDictionary *masks = @{@"all": @0x1fff, @"read": @1, @"sample": @2,
                    @"gradient": @4, @"compare": @8, @"query": @0x5f0,
                    @"level": @0x200, @"offset": @0x800, @"write": @0x1000, @"read-grid": @1};
                check(masks[operation] != nil, @"unknown image operation");
                operations = [masks[operation] unsignedIntValue];
            }
            else check(NO, @"invalid or repeated image operation option");
        }
        check(!directRead || (([selectedCase hasPrefix:@"color-2d_ms-"] ||
            [selectedCase isEqual:@"depth-2d_ms-0"]) &&
            ([operation isEqual:@"all"] || [operation isEqual:@"read"])),
            @"direct MS read requires one multisample case and all/read operations");
        check(!(nullGuard && nullSampling), @"null guard and null sampling cannot be combined");
        check(!(nullGuard || nullSampling) || (selectedCase && !directRead && [operation isEqual:@"all"]),
            @"null guard requires one case without another operation");
        check(!nullSampling || [@[@"depth-2d-0", @"depth-2d_array-0", @"depth-cube-0", @"depth-cube_array-0"] containsObject:selectedCase],
            @"null sampling requires one non-MS depth case");
        BOOL reference = strcmp(argv[1], "--reference") == 0;
        NSError *error = nil;
        id<MTLDevice> device = nil;
        if (reference) device = MTLCreateSystemDefaultDevice();
        else {
            NSBundle *bundle = [NSBundle bundleWithPath:@(argv[2])];
            check([bundle loadAndReturnError:&error], error.description ?: @"bundle load failed");
            device = [[[bundle classNamed:@"GarnetDevice"] alloc] init];
        }
        check(device != nil, @"image device unavailable");
        NSString *directory = @(argv[3]);
        check([[NSFileManager defaultManager] createDirectoryAtPath:directory withIntermediateDirectories:YES
            attributes:nil error:&error], error.description ?: @"output directory unavailable");
        id<MTLCommandQueue> queue = compileOnly ? nil : [device newCommandQueue];
        check(compileOnly || queue != nil, @"image command queue unavailable");
        unsigned cases = 0;
        for (NSUInteger s = 0; s < sizeof(kShapes) / sizeof(kShapes[0]); ++s)
            for (unsigned kind = 0; kind < (kShapes[s].depth ? 1u : 3u); ++kind) {
                if (selectedCase && ![selectedCase isEqual:caseLabel(&kShapes[s], kind)]) continue;
                @autoreleasepool {
                    if (nullGuard || nullSampling)
                        runNullImageGuards(device, queue, &kShapes[s], kind, directory, compileOnly,
                            nullSampling, nullCompare);
                    else if ([operation isEqual:@"read-grid"])
                        runCubeReadGrid(device, queue, &kShapes[s], kind, directory, compileOnly);
                    else runCase(device, queue, &kShapes[s], kind, directory, compileOnly, operations,
                        directRead);
                }
                ++cases;
            }
        check(cases != 0, @"unknown image operation case");
        if (compileOnly) {
            printf("ARGUMENT_IMAGE_OPERATIONS_CAPTURE backend=%s cases=%u msl=2.1 gpu_execution=0\n",
                reference ? "reference" : "bundle", cases);
            return 0;
        }
        if (nullGuard || nullSampling)
            printf("ARGUMENT_IMAGE_OPERATIONS_PASS backend=%s cases=%u dispatches=%u msl=2.1 "
                "operation=%s null_members=0 gpu_execution=1\n",
                reference ? "reference" : "bundle", cases, 8 * cases,
                nullSampling ? "null-sampling" : "null-guard");
        else if ([operation isEqual:@"read-grid"])
            printf("ARGUMENT_IMAGE_OPERATIONS_PASS backend=%s cases=%u msl=2.1 operation=read-grid "
                "guards=argument,output,selection,upload gpu_execution=1\n",
                reference ? "reference" : "bundle", cases);
        else
            printf("ARGUMENT_IMAGE_OPERATIONS_PASS backend=%s cases=%u selections=%u msl=2.1 operation=%s "
                "private_depth_staging=1 typed_ms=3 depth_ms=1 cube_gradient=float3 "
                "guards=argument,output,selection,upload,all_unselected_texels gpu_execution=1\n",
                reference ? "reference" : "bundle", cases, 2 * cases, operation.UTF8String);
    }
    return 0;
}
