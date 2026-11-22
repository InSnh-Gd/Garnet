#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const uint8_t kArgumentGuard = 0xa5;
static const uint8_t kOutputGuard = 0xcd;
static const NSUInteger kArgumentOffset = 256;
static const NSUInteger kArgumentTail = 256;
static const NSUInteger kOutputOffset = 64;
static const NSUInteger kOutputTail = 256;
enum { kFloatOutputCount = 60, kIntOutputCount = 8 };
static const NSUInteger kCubeSize = 4;

static void require(BOOL ok, NSString *message) {
    if (!ok) {
        fprintf(stderr, "ARGUMENT_GATHER_CUBE_FAIL %s\n", message.UTF8String);
        exit(1);
    }
}

static void commitAndWait(id<MTLCommandBuffer> command, NSString *operation) {
    require(command != nil, [operation stringByAppendingString:@" command buffer unavailable"]);
    [command commit];
    [command waitUntilCompleted];
    require(command.status == MTLCommandBufferStatusCompleted && command.error == nil,
        command.error.description ?: [operation stringByAppendingString:@" failed"]);
}

static NSUInteger alignUp(NSUInteger value, NSUInteger alignment) {
    require(alignment != 0, @"argument encoder alignment is zero");
    return (value + alignment - 1) / alignment * alignment;
}

static id<MTLBuffer> newSharedBuffer(id<MTLDevice> device, NSUInteger length,
                                     uint8_t fill, NSString *description) {
    id<MTLBuffer> buffer =
        [device newBufferWithLength:length options:MTLResourceStorageModeShared];
    require(buffer != nil && buffer.contents != NULL,
        [description stringByAppendingString:@" unavailable"]);
    memset(buffer.contents, fill, length);
    return buffer;
}

static void requireGuard(id<MTLBuffer> buffer, NSUInteger offset, NSUInteger length,
                         uint8_t guard, NSString *description) {
    require(buffer.contents != NULL && offset <= buffer.length &&
        length <= buffer.length - offset,
        [description stringByAppendingString:@" range invalid"]);
    const uint8_t *bytes = buffer.contents;
    for (NSUInteger i = 0; i < offset; ++i) {
        require(bytes[i] == guard,
            [NSString stringWithFormat:@"%@ prefix changed at %lu", description,
                (unsigned long)i]);
    }
    for (NSUInteger i = offset + length; i < buffer.length; ++i) {
        require(bytes[i] == guard,
            [NSString stringWithFormat:@"%@ suffix changed at %lu", description,
                (unsigned long)i]);
    }
}

static MTLTextureDescriptor *newCubeDescriptor(MTLPixelFormat format,
                                                MTLTextureType type,
                                                NSUInteger arrayLength) {
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor new];
    descriptor.textureType = type;
    descriptor.pixelFormat = format;
    descriptor.width = kCubeSize;
    descriptor.height = kCubeSize;
    descriptor.depth = 1;
    descriptor.mipmapLevelCount = 2;
    descriptor.sampleCount = 1;
    descriptor.arrayLength = arrayLength;
    descriptor.storageMode = format == MTLPixelFormatDepth32Float ?
        MTLStorageModePrivate : MTLStorageModeShared;
    descriptor.usage = MTLTextureUsageShaderRead;
    return descriptor;
}

static id<MTLTexture> newCubeTextureOptional(id<MTLDevice> device, MTLPixelFormat format,
                                              MTLTextureType type, NSUInteger arrayLength) {
    return [device newTextureWithDescriptor:newCubeDescriptor(format, type, arrayLength)];
}

static id<MTLTexture> newCubeTexture(id<MTLDevice> device, MTLPixelFormat format,
                                     MTLTextureType type, NSUInteger arrayLength,
                                     NSString *description) {
    id<MTLTexture> texture =
        newCubeTextureOptional(device, format, type, arrayLength);
    require(texture != nil, [description stringByAppendingString:@" unavailable"]);
    return texture;
}

static id<MTLTexture> newBaseMipView(id<MTLTexture> texture, MTLTextureType type,
                                     NSUInteger slices, NSUInteger arrayLength) {
    id<MTLTexture> view = [texture newTextureViewWithPixelFormat:texture.pixelFormat
        textureType:type levels:NSMakeRange(1, 1) slices:NSMakeRange(0, slices)];
    require(view != nil, @"base-mip cube texture view unavailable");
    require(view.textureType == type && view.width == kCubeSize / 2 &&
        view.height == kCubeSize / 2,
        @"base-mip cube view has the wrong type or dimensions");
    require(view.parentRelativeLevel == 1 && view.mipmapLevelCount == 1,
        @"base-mip cube view did not select exactly level 1");
    require(view.arrayLength == arrayLength,
        @"base-mip cube view array length does not match its descriptor");
    require(view.parentRelativeSlice == 0,
        @"base-mip cube view did not start at slice 0");
    return view;
}

static const float kColorPattern[2][2][4] = {
    {{0.10f, 0.11f, 0.12f, 0.13f}, {0.20f, 0.21f, 0.22f, 0.23f}},
    {{0.30f, 0.31f, 0.32f, 0.33f}, {0.40f, 0.41f, 0.42f, 0.43f}},
};

static const float kColorArrayPattern[2][2][4] = {
    {{0.50f, 0.51f, 0.52f, 0.53f}, {0.54f, 0.55f, 0.56f, 0.57f}},
    {{0.58f, 0.59f, 0.60f, 0.61f}, {0.62f, 0.63f, 0.64f, 0.65f}},
};

static void uploadColorCube(id<MTLTexture> texture, NSUInteger slices,
                            NSUInteger patternSlice, BOOL arrayPattern) {
    for (NSUInteger level = 0; level < 2; ++level) {
        NSUInteger size = kCubeSize >> level;
        float *texels = calloc(size * size * 4, sizeof(float));
        require(texels != NULL, @"color cube staging allocation failed");
        for (NSUInteger slice = 0; slice < slices; ++slice) {
            for (NSUInteger y = 0; y < size; ++y) {
                for (NSUInteger x = 0; x < size; ++x) {
                    float *pixel = texels + 4 * (y * size + x);
                    if (level == 1 && slice == patternSlice) {
                        const float (*pattern)[2][4] =
                            arrayPattern ? kColorArrayPattern : kColorPattern;
                        memcpy(pixel, pattern[y][x], 4 * sizeof(float));
                    } else {
                        pixel[0] = 99.0f;
                        pixel[1] = 99.0f;
                        pixel[2] = 99.0f;
                        pixel[3] = 99.0f;
                    }
                }
            }
            [texture replaceRegion:MTLRegionMake2D(0, 0, size, size)
                mipmapLevel:level slice:slice withBytes:texels bytesPerRow:size * 4 * sizeof(float)
                bytesPerImage:size * size * 4 * sizeof(float)];
        }
        free(texels);
    }
}

static void uploadUintCube(id<MTLTexture> texture, NSUInteger slices,
                           NSUInteger patternSlice, uint32_t value) {
    for (NSUInteger level = 0; level < 2; ++level) {
        NSUInteger size = kCubeSize >> level;
        uint32_t *texels = calloc(size * size, sizeof(uint32_t));
        require(texels != NULL, @"uint cube staging allocation failed");
        for (NSUInteger slice = 0; slice < slices; ++slice) {
            for (NSUInteger i = 0; i < size * size; ++i) {
                texels[i] = level == 1 && slice == patternSlice ?
                    value : UINT32_C(0xdeadbeef);
            }
            [texture replaceRegion:MTLRegionMake2D(0, 0, size, size)
                mipmapLevel:level slice:slice withBytes:texels bytesPerRow:size * sizeof(uint32_t)
                bytesPerImage:size * size * sizeof(uint32_t)];
        }
        free(texels);
    }
}

static void uploadSintCube(id<MTLTexture> texture, NSUInteger slices,
                           NSUInteger patternSlice, int32_t value) {
    for (NSUInteger level = 0; level < 2; ++level) {
        NSUInteger size = kCubeSize >> level;
        int32_t *texels = calloc(size * size, sizeof(int32_t));
        require(texels != NULL, @"sint cube staging allocation failed");
        for (NSUInteger slice = 0; slice < slices; ++slice) {
            for (NSUInteger i = 0; i < size * size; ++i)
                texels[i] = level == 1 && slice == patternSlice ? value : INT32_C(-9999);
            [texture replaceRegion:MTLRegionMake2D(0, 0, size, size)
                mipmapLevel:level slice:slice withBytes:texels bytesPerRow:size * sizeof(int32_t)
                bytesPerImage:size * size * sizeof(int32_t)];
        }
        free(texels);
    }
}

static void uploadConstantColorCube(id<MTLTexture> texture, NSUInteger slices,
                                    NSUInteger patternSlice, float value) {
    for (NSUInteger level = 0; level < 2; ++level) {
        NSUInteger size = kCubeSize >> level;
        float *texels = calloc(size * size * 4, sizeof(float));
        require(texels != NULL, @"constant color cube staging allocation failed");
        for (NSUInteger slice = 0; slice < slices; ++slice) {
            float sample = level == 1 && slice == patternSlice ? value : 99.0f;
            for (NSUInteger i = 0; i < size * size; ++i) {
                for (NSUInteger component = 0; component < 4; ++component)
                    texels[4 * i + component] = sample;
            }
            [texture replaceRegion:MTLRegionMake2D(0, 0, size, size)
                mipmapLevel:level slice:slice withBytes:texels bytesPerRow:size * 4 * sizeof(float)
                bytesPerImage:size * size * 4 * sizeof(float)];
        }
        free(texels);
    }
}

static void uploadDepthCube(id<MTLDevice> device, id<MTLCommandQueue> queue,
                            id<MTLTexture> texture, NSUInteger slices,
                            NSUInteger patternSlice, float value) {
    for (NSUInteger level = 0; level < 2; ++level) {
        NSUInteger size = kCubeSize >> level;
        NSUInteger planeBytes = size * size * sizeof(float);
        NSUInteger uploadBytes = planeBytes * slices;
        id<MTLBuffer> upload = newSharedBuffer(device, uploadBytes, 0,
            @"depth cube staging buffer");
        for (NSUInteger slice = 0; slice < slices; ++slice) {
            float *texels = (float *)upload.contents + slice * size * size;
            for (NSUInteger i = 0; i < size * size; ++i)
                texels[i] = level == 1 && slice == patternSlice ? value : 0.25f;
        }
        id<MTLCommandBuffer> command = [queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
        require(command != nil && blit != nil, @"depth cube upload encoder unavailable");
        for (NSUInteger slice = 0; slice < slices; ++slice) {
            [blit copyFromBuffer:upload sourceOffset:slice * planeBytes
                sourceBytesPerRow:size * sizeof(float)
                sourceBytesPerImage:planeBytes
                sourceSize:MTLSizeMake(size, size, 1)
                toTexture:texture destinationSlice:slice destinationLevel:level
                destinationOrigin:MTLOriginMake(0, 0, 0)];
        }
        [blit endEncoding];
        commitAndWait(command, @"depth cube upload");
    }
}

static id<MTLSamplerState> newSampler(id<MTLDevice> device, MTLCompareFunction compare,
                                      NSString *description) {
    MTLSamplerDescriptor *descriptor = [MTLSamplerDescriptor new];
    descriptor.minFilter = MTLSamplerMinMagFilterNearest;
    descriptor.magFilter = MTLSamplerMinMagFilterNearest;
    descriptor.mipFilter = MTLSamplerMipFilterNotMipmapped;
    descriptor.sAddressMode = MTLSamplerAddressModeClampToEdge;
    descriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
    descriptor.rAddressMode = MTLSamplerAddressModeClampToEdge;
    descriptor.normalizedCoordinates = YES;
    descriptor.compareFunction = compare;
    descriptor.supportArgumentBuffers = YES;
    id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:descriptor];
    require(sampler != nil, [description stringByAppendingString:@" unavailable"]);
    return sampler;
}

static void writeAIR(id<MTLFunction> function, NSString *directory) {
    SEL selector = NSSelectorFromString(@"bitcodeData");
    NSData *air = [function respondsToSelector:selector] ?
        ((id (*)(id, SEL))objc_msgSend)(function, selector) : nil;
    NSString *path = [directory stringByAppendingPathComponent:
        [function.name stringByAppendingString:@".air"]];
    require(air.length > 0 && [air writeToFile:path atomically:YES],
        [NSString stringWithFormat:@"AIR write failed for %@", function.name]);
    printf("ARGUMENT_GATHER_CUBE_AIR function=%s file=%s bytes=%lu\n",
        function.name.UTF8String, path.UTF8String, (unsigned long)air.length);
}

static void requireFloats(const float *actual, const float *expected, NSUInteger count,
                          const char *caseName) {
    for (NSUInteger i = 0; i < count; ++i) {
        require(fabsf(actual[i] - expected[i]) <= 1.0e-6f,
            [NSString stringWithFormat:@"%s float[%lu]=%.9g expected=%.9g", caseName,
                (unsigned long)i, actual[i], expected[i]]);
    }
}

static void run(id<MTLDevice> device, id<MTLCommandQueue> queue,
                id<MTLComputePipelineState> pipeline, id<MTLArgumentEncoder> encoder,
                NSArray<id<MTLTexture>> *textures, NSArray<id<MTLSamplerState>> *samplers,
                const char *backend) {
    NSUInteger argumentOffset = alignUp(kArgumentOffset, encoder.alignment);
    id<MTLBuffer> arguments = newSharedBuffer(device,
        argumentOffset + encoder.encodedLength + kArgumentTail,
        kArgumentGuard, @"cube gather argument buffer");
    id<MTLBuffer> floatOutput = newSharedBuffer(device,
        kOutputOffset + kFloatOutputCount * sizeof(float) + kOutputTail,
        kOutputGuard, @"cube gather float output");
    id<MTLBuffer> intOutput = newSharedBuffer(device,
        kOutputOffset + kIntOutputCount * sizeof(int32_t) + kOutputTail,
        kOutputGuard, @"cube gather integer output");

    [encoder setArgumentBuffer:arguments offset:argumentOffset];
    for (NSUInteger i = 0; i < textures.count; ++i)
        [encoder setTexture:textures[i] atIndex:i];
    for (NSUInteger i = 0; i < samplers.count; ++i)
        [encoder setSamplerState:samplers[i] atIndex:textures.count + i];
    requireGuard(arguments, argumentOffset, encoder.encodedLength, kArgumentGuard,
        @"cube gather arguments after encode");
    NSData *argumentSnapshot =
        [NSData dataWithBytes:arguments.contents length:arguments.length];

    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLComputeCommandEncoder> compute = [command computeCommandEncoder];
    require(command != nil && compute != nil, @"cube gather compute encoder unavailable");
    [compute setComputePipelineState:pipeline];
    [compute setBuffer:arguments offset:argumentOffset atIndex:3];
    [compute setBuffer:floatOutput offset:kOutputOffset atIndex:0];
    [compute setBuffer:intOutput offset:kOutputOffset atIndex:1];
    for (id<MTLTexture> texture in textures)
        [compute useResource:texture usage:MTLResourceUsageRead];
    [compute dispatchThreadgroups:MTLSizeMake(1, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(2, 1, 1)];
    [compute endEncoding];
    [command commit];
    [command waitUntilCompleted];
    require(command.status == MTLCommandBufferStatusCompleted && command.error == nil,
        command.error.description ?: @"cube gather execution failed");

    requireGuard(arguments, argumentOffset, encoder.encodedLength, kArgumentGuard,
        @"cube gather arguments after GPU");
    requireGuard(floatOutput, kOutputOffset, kFloatOutputCount * sizeof(float), kOutputGuard,
        @"cube gather float output after GPU");
    requireGuard(intOutput, kOutputOffset, kIntOutputCount * sizeof(int32_t), kOutputGuard,
        @"cube gather integer output after GPU");
    require(memcmp(argumentSnapshot.bytes, arguments.contents, arguments.length) == 0,
        @"GPU modified the cube gather argument buffer");

    const float *floats =
        (const float *)((const uint8_t *)floatOutput.contents + kOutputOffset);
    const int32_t *ints =
        (const int32_t *)((const uint8_t *)intOutput.contents + kOutputOffset);
    static const float expected[kFloatOutputCount] = {
        0.30f, 0.40f, 0.20f, 0.10f,
        0.31f, 0.41f, 0.21f, 0.11f,
        0.32f, 0.42f, 0.22f, 0.12f,
        0.33f, 0.43f, 0.23f, 0.13f,
        0.60f, 0.64f, 0.56f, 0.52f,
        0.75f, 0.75f, 0.75f, 0.75f,
        0.80f, 0.80f, 0.80f, 0.80f,
        0.0f, 0.0f, 0.0f, 0.0f,
        1.0f, 1.0f, 1.0f, 1.0f,
        0.0f, 0.0f, 0.0f, 0.0f,
        1.0f, 1.0f, 1.0f, 1.0f,
        0.0f, 0.0f, 0.0f, 0.0f,
        1.0f, 1.0f, 1.0f, 1.0f,
        0.30f, 0.40f, 0.20f, 0.10f,
        0.75f, 0.75f, 0.75f, 0.75f,
    };
    requireFloats(floats, expected, kFloatOutputCount, "cube gather");
    for (NSUInteger i = 0; i < 4; ++i) {
        require(ints[i] == 17,
            [NSString stringWithFormat:@"uint gather[%lu]=%d expected=17",
                (unsigned long)i, ints[i]]);
        require(ints[4 + i] == -19,
            [NSString stringWithFormat:@"sint gather[%lu]=%d expected=-19",
                (unsigned long)i, ints[4 + i]]);
    }

    printf("ARGUMENT_GATHER_CUBE_CASE_PASS backend=%s base_mip=1 floats=%lu ints=%lu "
           "argument_entries=11 argument_bytes=88 "
           "order=BL,BR,TR,TL components=rgba cube_array_layer=1 depth=0.75 "
           "never=0 always=1 dref_less=0,1 nonuniform=2 guards=arguments,float,int\n",
        backend, (unsigned long)kFloatOutputCount, (unsigned long)kIntOutputCount);
}

int main(int argc, const char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        puts("Usage: test-argument-gather-cube BUNDLE OUTPUT_DIR [--capture-only]");
        puts("       test-argument-gather-cube --reference OUTPUT_DIR [--capture-only]");
        return 0;
    }
    BOOL captureOnly = argc == 4 && !strcmp(argv[3], "--capture-only");
    if ((!captureOnly && argc != 3) || (captureOnly && argc != 4)) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(110);
    @autoreleasepool {
        NSError *error = nil;
        NSString *directory = @(argv[2]);
        require([[NSFileManager defaultManager] createDirectoryAtPath:directory
            withIntermediateDirectories:YES attributes:nil error:&error],
            error.description ?: @"output directory unavailable");

        BOOL reference = !strcmp(argv[1], "--reference");
        id<MTLDevice> device = nil;
        if (reference) {
            device = MTLCreateSystemDefaultDevice();
        } else {
            NSBundle *bundle = [NSBundle bundleWithPath:@(argv[1])];
            require([bundle loadAndReturnError:&error], error.description ?: @"bundle load failed");
            device = [[[bundle classNamed:@"GarnetDevice"] alloc] init];
        }
        require(device != nil, @"Metal device unavailable");

        NSString *source =
            @"#include <metal_stdlib>\n"
             "using namespace metal;\n"
             "struct Args {\n"
             "  texturecube<float> color [[id(0)]];\n"
             "  texturecube_array<float> colorArray [[id(1)]];\n"
             "  texturecube<uint> uintCube [[id(2)]];\n"
             "  texturecube<int> intCube [[id(3)]];\n"
             "  depthcube<float> depthCube [[id(4)]];\n"
             "  depthcube_array<float> depthArray [[id(5)]];\n"
             "  texturecube<float> colorAlt [[id(6)]];\n"
             "  sampler ordinary [[id(7)]];\n"
             "  sampler never [[id(8)]];\n"
             "  sampler always [[id(9)]];\n"
             "  sampler less [[id(10)]];\n"
             "};\n"
             "kernel void gather_cube_all(constant Args &args [[buffer(3)]],\n"
             "  device float *floats [[buffer(0)]],\n"
             "  device int *ints [[buffer(1)]],\n"
             "  uint tid [[thread_position_in_grid]]) {\n"
             "  float3 dirX = float3(1.0f, 0.0f, 0.0f);\n"
             "  float3 dirY = float3(0.0f, 1.0f, 0.0f);\n"
             "  if (tid == 0u) {\n"
             "  float4 c0 = args.color.gather(args.ordinary, dirX, component::x);\n"
             "  float4 c1 = args.color.gather(args.ordinary, dirX, component::y);\n"
             "  float4 c2 = args.color.gather(args.ordinary, dirX, component::z);\n"
             "  float4 c3 = args.color.gather(args.ordinary, dirX, component::w);\n"
             "  floats[0]=c0.x; floats[1]=c0.y; floats[2]=c0.z; floats[3]=c0.w;\n"
             "  floats[4]=c1.x; floats[5]=c1.y; floats[6]=c1.z; floats[7]=c1.w;\n"
             "  floats[8]=c2.x; floats[9]=c2.y; floats[10]=c2.z; floats[11]=c2.w;\n"
             "  floats[12]=c3.x; floats[13]=c3.y; floats[14]=c3.z; floats[15]=c3.w;\n"
             "  float4 ca = args.colorArray.gather(args.ordinary, dirY, 1, component::z);\n"
             "  floats[16]=ca.x; floats[17]=ca.y; floats[18]=ca.z; floats[19]=ca.w;\n"
             "  float4 d = args.depthCube.gather(args.ordinary, dirX);\n"
             "  floats[20]=d.x; floats[21]=d.y; floats[22]=d.z; floats[23]=d.w;\n"
             "  float4 da = args.depthArray.gather(args.ordinary, dirY, 1);\n"
             "  floats[24]=da.x; floats[25]=da.y; floats[26]=da.z; floats[27]=da.w;\n"
             "  float4 never = args.depthCube.gather_compare(args.never, dirX, 0.5f);\n"
             "  floats[28]=never.x; floats[29]=never.y; floats[30]=never.z; floats[31]=never.w;\n"
             "  float4 always = args.depthCube.gather_compare(args.always, dirX, 0.5f);\n"
             "  floats[32]=always.x; floats[33]=always.y; floats[34]=always.z; floats[35]=always.w;\n"
             "  float4 neverArray = args.depthArray.gather_compare(args.never, dirY, 1, 0.5f);\n"
             "  floats[36]=neverArray.x; floats[37]=neverArray.y; floats[38]=neverArray.z; floats[39]=neverArray.w;\n"
             "  float4 alwaysArray = args.depthArray.gather_compare(args.always, dirY, 1, 0.5f);\n"
             "  floats[40]=alwaysArray.x; floats[41]=alwaysArray.y; floats[42]=alwaysArray.z; floats[43]=alwaysArray.w;\n"
             "  float4 lessLayer0 = args.depthArray.gather_compare(args.less, dirY, 0, 0.5f);\n"
             "  floats[44]=lessLayer0.x; floats[45]=lessLayer0.y; floats[46]=lessLayer0.z; floats[47]=lessLayer0.w;\n"
             "  float4 lessLayer1 = args.depthArray.gather_compare(args.less, dirY, 1, 0.5f);\n"
             "  floats[48]=lessLayer1.x; floats[49]=lessLayer1.y; floats[50]=lessLayer1.z; floats[51]=lessLayer1.w;\n"
             "  uint4 u = args.uintCube.gather(args.ordinary, dirX, component::x);\n"
             "  int4 s = args.intCube.gather(args.ordinary, dirX, component::x);\n"
             "  ints[0]=int(u.x); ints[1]=int(u.y); ints[2]=int(u.z); ints[3]=int(u.w);\n"
             "  ints[4]=s.x; ints[5]=s.y; ints[6]=s.z; ints[7]=s.w;\n"
             "  }\n"
             "  float4 selected = (tid & 1u) == 0u ?\n"
             "    args.color.gather(args.ordinary, dirX, component::x) :\n"
             "    args.colorAlt.gather(args.ordinary, dirX, component::x);\n"
             "  floats[52 + tid * 4]=selected.x; floats[53 + tid * 4]=selected.y;\n"
             "  floats[54 + tid * 4]=selected.z; floats[55 + tid * 4]=selected.w;\n"
             "}\n";
        require([source writeToFile:[directory stringByAppendingPathComponent:
            @"argument-gather-cube.metal"] atomically:YES encoding:NSUTF8StringEncoding
            error:&error], @"source write failed");

        MTLCompileOptions *options = [MTLCompileOptions new];
        options.languageVersion = MTLLanguageVersion2_1;
        id<MTLLibrary> library =
            [device newLibraryWithSource:source options:options error:&error];
        require(library != nil, error.description ?: @"source compilation failed");
        id<MTLFunction> function = [library newFunctionWithName:@"gather_cube_all"];
        require(function != nil, @"cube gather function unavailable");
        writeAIR(function, directory);

        if (captureOnly) {
            puts("ARGUMENT_GATHER_CUBE_CAPTURE_PASS functions=1 gpu_execution=0 msl=2.1");
            return 0;
        }

        id<MTLArgumentEncoder> encoder = [function newArgumentEncoderWithBufferIndex:3];
        require(encoder != nil && encoder.encodedLength == 11 * sizeof(uint64_t) &&
            encoder.alignment != 0,
            @"cube gather argument encoder must contain exactly eleven 8-byte entries");
        id<MTLCommandQueue> queue = [device newCommandQueue];
        require(queue != nil, @"cube gather command queue unavailable");

        id<MTLTexture> colorTexture = newCubeTexture(device, MTLPixelFormatRGBA32Float,
            MTLTextureTypeCube, 1, @"color cube texture");
        id<MTLTexture> colorArrayTexture = newCubeTexture(device,
            MTLPixelFormatRGBA32Float, MTLTextureTypeCubeArray, 2,
            @"color cube-array texture");
        id<MTLTexture> uintTexture = newCubeTexture(device, MTLPixelFormatR32Uint,
            MTLTextureTypeCube, 1, @"uint cube texture");
        id<MTLTexture> intTexture = newCubeTexture(device, MTLPixelFormatR32Sint,
            MTLTextureTypeCube, 1, @"sint cube texture");
        id<MTLTexture> colorAltTexture = newCubeTexture(device, MTLPixelFormatRGBA32Float,
            MTLTextureTypeCube, 1, @"alternate color cube texture");
        id<MTLTexture> depthTexture = newCubeTextureOptional(device, MTLPixelFormatDepth32Float,
            MTLTextureTypeCube, 1);
        id<MTLTexture> depthArrayTexture = newCubeTextureOptional(device,
            MTLPixelFormatDepth32Float, MTLTextureTypeCubeArray, 2);
        if (depthTexture == nil || depthArrayTexture == nil) {
            require(NO, reference ? @"system Metal depth cube or cube-array texture unavailable" :
                @"bundle depth cube or cube-array texture unavailable");
        }

        uploadColorCube(colorTexture, 6, 0, NO);
        uploadColorCube(colorArrayTexture, 12, 8, YES);
        uploadConstantColorCube(colorAltTexture, 6, 0, 0.75f);
        uploadUintCube(uintTexture, 6, 0, UINT32_C(17));
        uploadSintCube(intTexture, 6, 0, INT32_C(-19));
        uploadDepthCube(device, queue, depthTexture, 6, 0, 0.75f);
        uploadDepthCube(device, queue, depthArrayTexture, 12, 8, 0.80f);

        id<MTLTexture> colorView =
            newBaseMipView(colorTexture, MTLTextureTypeCube, 6, 1);
        id<MTLTexture> colorArrayView =
            newBaseMipView(colorArrayTexture, MTLTextureTypeCubeArray, 12, 2);
        id<MTLTexture> uintView =
            newBaseMipView(uintTexture, MTLTextureTypeCube, 6, 1);
        id<MTLTexture> intView =
            newBaseMipView(intTexture, MTLTextureTypeCube, 6, 1);
        id<MTLTexture> colorAltView =
            newBaseMipView(colorAltTexture, MTLTextureTypeCube, 6, 1);
        id<MTLTexture> depthView =
            newBaseMipView(depthTexture, MTLTextureTypeCube, 6, 1);
        id<MTLTexture> depthArrayView =
            newBaseMipView(depthArrayTexture, MTLTextureTypeCubeArray, 12, 2);

        id<MTLSamplerState> ordinary =
            newSampler(device, MTLCompareFunctionNever, @"ordinary sampler");
        id<MTLSamplerState> never =
            newSampler(device, MTLCompareFunctionNever, @"Never sampler");
        id<MTLSamplerState> always =
            newSampler(device, MTLCompareFunctionAlways, @"Always sampler");
        id<MTLSamplerState> less =
            newSampler(device, MTLCompareFunctionLess, @"Less sampler");
        NSArray<id<MTLTexture>> *textures = @[
            colorView, colorArrayView, uintView, intView, depthView, depthArrayView, colorAltView
        ];
        NSArray<id<MTLSamplerState>> *samplers = @[ordinary, never, always, less];

        id<MTLComputePipelineState> pipeline =
            [device newComputePipelineStateWithFunction:function error:&error];
        require(pipeline != nil, error.description ?: @"cube gather pipeline unavailable");

        run(device, queue, pipeline, encoder, textures, samplers,
            reference ? "reference" : "bundle");
        puts("ARGUMENT_GATHER_CUBE_PASS base_mip=1 cube=1 cube_array=1 "
             "depth=1 dref=1 integer=1 never=1 always=1 nonuniform=1");
    }
    return 0;
}
