#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

static void require(BOOL ok, NSString *message) {
    if (!ok) { fprintf(stderr, "ARGUMENT_NULL_FAIL %s\n", message.UTF8String); exit(1); }
}

static NSUInteger align_up(NSUInteger value, NSUInteger alignment) {
    return alignment ? (value + alignment - 1) / alignment * alignment : value;
}

int main(int argc, const char **argv) {
    if (argc != 4 && !(argc == 5 && !strcmp(argv[4], "--capture-only"))) return 2;
    alarm(90); setvbuf(stdout, NULL, _IONBF, 0);
    @autoreleasepool {
        NSString *kind = @(argv[3]), *directory = @(argv[2]); NSError *error = nil;
        BOOL flat = [kind hasPrefix:@"flat-"];
        NSString *mode = flat ? [kind substringFromIndex:5] : kind;
        BOOL writable = [mode hasSuffix:@"-rw"], integer = [mode hasPrefix:@"uint-"];
        BOOL fragmentMode = [mode hasPrefix:@"fragment-"];
        BOOL fragmentGather = [mode hasPrefix:@"fragment-gather-2d"];
        BOOL fragmentBias = [mode hasPrefix:@"fragment-bias-2d"];
        BOOL sampling = [mode hasPrefix:@"sample-"] || [mode hasPrefix:@"gather-"] || fragmentMode;
        BOOL gather = [mode hasPrefix:@"gather-"] || fragmentGather;
        BOOL sampleCube = [mode hasPrefix:@"sample-cube"];
        BOOL sampleArray = [mode hasPrefix:@"sample-array"];
        BOOL sample3D = [mode hasPrefix:@"sample-3d"];
        BOOL array = sampleArray || [mode hasPrefix:@"array-"];
        BOOL volume = sample3D || [mode hasPrefix:@"3d-"];
        BOOL cube = sampleCube;
        BOOL empty = [mode containsString:@"-empty"];
        BOOL nilSampler = [mode containsString:@"-nil-sampler"];
        require(!sampling || (!writable && !integer), @"sampling modes are read-only float textures");
        require(!gather || [mode hasPrefix:@"gather-2d"] || fragmentGather, @"gather is restricted to 2D textures");
        require(!fragmentMode || [mode hasPrefix:@"fragment-sample-2d"] || fragmentGather || fragmentBias,
            @"unknown fragment sampling mode");
        require(!nilSampler || (sampling && empty), @"nil-sampler modes must also empty the textures");
        require([[NSFileManager defaultManager] createDirectoryAtPath:directory withIntermediateDirectories:YES
            attributes:nil error:&error], error.description ?: @"output directory");
        id<MTLDevice> device;
        if (!strcmp(argv[1], "--reference")) device = MTLCreateSystemDefaultDevice();
        else {
            NSBundle *bundle = [NSBundle bundleWithPath:@(argv[1])];
            require([bundle loadAndReturnError:&error], error.description ?: @"bundle load");
            device = [[[bundle classNamed:@"GarnetDevice"] alloc] init];
        }
        require(device != nil, @"device unavailable");
        NSString *shape = cube ? @"texturecube" : volume ? @"texture3d" : array ? @"texture2d_array" : @"texture2d";
        NSString *coordinates = sampleCube ? @"float3(1.0, 0.0, 0.0)" :
            (sample3D || sampleArray) ? @"float3(0.5, 0.5, 1.0)" :
            sampling ? @"float2(0.5, 0.5)" :
            volume ? @"uint3(x, 0, 1)" : array ? @"uint2(x, 0), 1u" : @"uint2(x, 0)";
        NSString *samplingCoordinates = sampleCube ? @"float3(1.0, 0.0, 0.0)" :
            sample3D ? @"float3(0.5, 0.5, 1.0)" :
            sampleArray ? @"float2(0.5), 1u" : @"float2(0.5)";
        NSString *access;
        NSString *declarations;
        if (flat) {
            access = @"a.images[i & 1u]";
            declarations = [NSString stringWithFormat:
                @"struct Args { array<%@<%@, access::%@>,2> images [[id(1)]]; };\n",
                shape, integer ? @"uint" : @"float", writable ? @"read_write" : @"read"];
        } else {
            access = @"a.child[(i >> 2u) & 1u].images[i & 1u]";
            declarations = [NSString stringWithFormat:
                @"struct Child { array<%@<%@, access::%@>,2> images [[id(1)]]; uint pad [[id(4)]]; };\n"
                 "struct Args { array<Child,2> child [[id(3)]]; };\n",
                shape, integer ? @"uint" : @"float", writable ? @"read_write" : @"read"];
        }
        NSString *source = nil;
        if (sampling) {
            NSString *sampleDeclarations;
            if (flat) {
                sampleDeclarations = [NSString stringWithFormat:
                    @"struct Args { sampler s [[id(0)]]; array<%@<%@>,2> images [[id(1)]]; };\n",
                    shape, @"float"];
            } else {
                sampleDeclarations = [NSString stringWithFormat:
                    @"struct Child { array<%@<%@>,2> images [[id(1)]]; uint pad [[id(4)]]; };\n"
                     "struct Args { sampler s [[id(0)]]; array<Child,2> child [[id(3)]]; };\n",
                    shape, @"float"];
            }
            NSString *fieldValue = gather ?
                [NSString stringWithFormat:@"image.gather(a.s, %@)", samplingCoordinates] :
                [NSString stringWithFormat:@"image.sample(a.s, %@, level(0.0f))", samplingCoordinates];
            NSString *constantValue = gather ?
                [NSString stringWithFormat:@"image.gather(cs, %@)", samplingCoordinates] :
                [NSString stringWithFormat:@"image.sample(cs, %@, level(0.0f))", samplingCoordinates];
            source = [NSString stringWithFormat:
                @"#include <metal_stdlib>\nusing namespace metal;\n%@"
                 "kernel void null_texels(constant Args &a [[buffer(3)]], device uint *out [[buffer(0)]],"
                 " uint i [[thread_position_in_grid]]) { auto image = %@;"
                 " constexpr sampler cs(coord::normalized, address::clamp_to_edge, filter::nearest, mip_filter::nearest);"
                 " float4 fieldValue = %@; float4 constantValue = %@;"
                 " out[2*i] = uint(fieldValue.x + 2*fieldValue.y + 4*fieldValue.z + 8*fieldValue.w);"
                 " out[2*i+1] = uint(constantValue.x + 2*constantValue.y + 4*constantValue.z + 8*constantValue.w); }\n",
                sampleDeclarations, access, fieldValue, constantValue];
        } else {
            source = [NSString stringWithFormat:
                @"#include <metal_stdlib>\nusing namespace metal;\n%@"
                 "kernel void null_texels(constant Args &a [[buffer(3)]], device uint *out [[buffer(0)]], uint i [[thread_position_in_grid]]) {"
                 " auto image = %@; uint x = (i >> 1u) & 1u;"
                 " auto before = image.read(%@); out[2*i] = uint(before.x + 2*before.y + 4*before.z + 8*before.w); %@"
                 " auto after = image.read(%@); out[2*i+1] = uint(after.x + 2*after.y + 4*after.z + 8*after.w); }\n",
                 declarations, access, coordinates,
                 writable ? [NSString stringWithFormat:@"image.write(%@, %@);",
                    integer ? @"uint4(105u+i, 7u, 9u, 11u)" : @"float4(float(105u+i), 7.0f, 9.0f, 11.0f)",
                    coordinates] : @"",
                 coordinates];
        }
        if (fragmentMode) {
            NSString *fieldValue = nil, *constantValue = nil;
            if (fragmentGather) {
                fieldValue = @"image.gather(a.s, p.xy * 0.25f)";
                constantValue = @"image.gather(cs, p.xy * 0.25f)";
            } else {
                NSString *bias = fragmentBias ? @", bias(1.0f)" : @"";
                fieldValue = [NSString stringWithFormat:@"image.sample(a.s, p.xy * 0.25f%@)", bias];
                constantValue = [NSString stringWithFormat:@"image.sample(cs, p.xy * 0.25f%@)", bias];
            }
            source = [source stringByAppendingFormat:
                @"vertex float4 null_vertex(uint vid [[vertex_id]]) {"
                 " float2 p = float2(float((vid << 1u) & 2u), float(vid & 2u));"
                 " return float4(p * 2.0 - 1.0, 0.0, 1.0); }"
                 "fragment float4 null_fragment(float4 p [[position]], constant Args &a [[buffer(3)]]) {"
                 " uint i = uint(p.x) >> 1u; auto image = %@;"
                 " constexpr sampler cs(coord::normalized, address::clamp_to_edge, filter::nearest, mip_filter::nearest);"
                 " float4 fieldValue = %@; float4 constantValue = %@;"
                 " return float4(fieldValue.r, constantValue.g, fieldValue.b, constantValue.a); }\n",
                access, fieldValue, constantValue];
        }
        require([source writeToFile:[directory stringByAppendingPathComponent:@"null.metal"] atomically:YES
            encoding:NSUTF8StringEncoding error:&error], @"write source");
        MTLCompileOptions *options = [MTLCompileOptions new]; options.languageVersion = MTLLanguageVersion2_1;
        id<MTLLibrary> library = [device newLibraryWithSource:source options:options error:&error];
        require(library != nil, error.description ?: @"library");
        id<MTLFunction> function = [library newFunctionWithName:@"null_texels"];
        require(function != nil, @"function");
        SEL bitcode = NSSelectorFromString(@"bitcodeData");
        NSData *air = [function respondsToSelector:bitcode] ? ((id (*)(id, SEL))objc_msgSend)(function, bitcode) : nil;
        require(air.length && [air writeToFile:[directory stringByAppendingPathComponent:@"null.air"] atomically:YES],
            @"AIR artifact");
        id<MTLFunction> vertexFunction = fragmentMode ? [library newFunctionWithName:@"null_vertex"] : nil;
        id<MTLFunction> fragmentFunction = fragmentMode ? [library newFunctionWithName:@"null_fragment"] : nil;
        require(!fragmentMode || (vertexFunction && fragmentFunction), @"fragment functions");
        if (fragmentMode) {
            for (id<MTLFunction> stage in @[vertexFunction, fragmentFunction]) {
                NSData *stageAir = [stage respondsToSelector:bitcode] ? ((id (*)(id, SEL))objc_msgSend)(stage, bitcode) : nil;
                require(stageAir.length && [stageAir writeToFile:[directory stringByAppendingPathComponent:
                    [stage.name stringByAppendingString:@".air"]] atomically:YES], @"fragment AIR artifact");
            }
        }
        id<MTLArgumentEncoder> encoder = [function newArgumentEncoderWithBufferIndex:3];
        require(encoder && encoder.encodedLength && encoder.encodedLength < 4096 &&
            encoder.alignment && encoder.alignment <= 4096, @"argument layout");
        NSUInteger offset = align_up(256, encoder.alignment);
        id<MTLBuffer> arguments = [device newBufferWithLength:offset + encoder.encodedLength + 64
            options:MTLResourceStorageModeShared];
        id<MTLBuffer> privateArguments = [device newBufferWithLength:arguments.length options:MTLResourceStorageModePrivate];
        id<MTLBuffer> output = [device newBufferWithLength:96 options:MTLResourceStorageModeShared];
        require(arguments.contents && privateArguments && output.contents, @"buffers");
        memset(arguments.contents, 0xa5, arguments.length); memset(output.contents, 0xcd, output.length);
        [encoder setArgumentBuffer:arguments offset:offset];
        unsigned groups = flat ? 1 : 2, invocations = groups * 4;
        id<MTLTexture> textures[4] = {nil};
        for (unsigned group = 0; group < groups; ++group) for (unsigned image = 0; image < 2; ++image) {
            MTLTextureDescriptor *td;
            if (fragmentMode) {
                td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                    width:8 height:8 mipmapped:YES];
                td.mipmapLevelCount = 4;
                td.storageMode = MTLStorageModeShared;
                td.usage = MTLTextureUsageShaderRead;
            } else if (sampling) {
                td = cube ?
                    [MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float size:2 mipmapped:NO] :
                    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:2 height:2 mipmapped:NO];
                td.storageMode = MTLStorageModeShared;
                td.usage = MTLTextureUsageShaderRead;
                if (volume) { td.textureType = MTLTextureType3D; td.depth = 2; }
                if (array) { td.textureType = MTLTextureType2DArray; td.arrayLength = 2; }
            } else {
                td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
                    integer ? MTLPixelFormatR32Uint : MTLPixelFormatR32Float width:2 height:1 mipmapped:NO];
                td.storageMode = MTLStorageModeShared;
                td.usage = MTLTextureUsageShaderRead | (writable ? MTLTextureUsageShaderWrite : 0);
                if (volume) { td.textureType = MTLTextureType3D; td.depth = 2; }
                if (array) { td.textureType = MTLTextureType2DArray; td.arrayLength = 2; }
            }
            id<MTLTexture> texture = [device newTextureWithDescriptor:td];
            require(texture != nil, @"texture"); textures[2*group+image] = texture;
            if (fragmentMode) {
                for (unsigned level = 0; level < 4; ++level) {
                    NSUInteger width = 8 >> level, height = 8 >> level;
                    float texels[256];
                    float scale = (float)(level + 1);
                    for (NSUInteger j = 0; j < width * height * 4; j += 4) {
                        texels[j] = scale; texels[j+1] = 2.0f * scale;
                        texels[j+2] = 3.0f * scale; texels[j+3] = 4.0f * scale;
                    }
                    [texture replaceRegion:MTLRegionMake2D(0, 0, width, height) mipmapLevel:level slice:0
                        withBytes:texels bytesPerRow:width * 4 * sizeof(float) bytesPerImage:0];
                }
            } else if (sampling) {
                float texels[32];
                for (unsigned j = 0; j < 32; j += 4) {
                    texels[j] = 1.0f; texels[j+1] = 2.0f; texels[j+2] = 3.0f; texels[j+3] = 4.0f;
                }
                if (volume) {
                    [texture replaceRegion:MTLRegionMake3D(0, 0, 0, 2, 2, 2) mipmapLevel:0 slice:0
                        withBytes:texels bytesPerRow:32 bytesPerImage:64];
                } else if (array) {
                    for (unsigned slice = 0; slice < 2; ++slice)
                        [texture replaceRegion:MTLRegionMake2D(0, 0, 2, 2) mipmapLevel:0 slice:slice
                            withBytes:texels bytesPerRow:32 bytesPerImage:0];
                } else if (cube) {
                    for (unsigned slice = 0; slice < 6; ++slice)
                        [texture replaceRegion:MTLRegionMake2D(0, 0, 2, 2) mipmapLevel:0 slice:slice
                            withBytes:texels bytesPerRow:32 bytesPerImage:0];
                } else {
                    [texture replaceRegion:MTLRegionMake2D(0, 0, 2, 2) mipmapLevel:0 slice:0
                        withBytes:texels bytesPerRow:32 bytesPerImage:0];
                }
            } else {
                uint32_t pixels[4];
                for (unsigned j = 0; j < 4; ++j) {
                    uint32_t value = j < 2 ? 21 + j : 41 + (j & 1);
                    if (integer) pixels[j] = value;
                    else { float converted = (float)value; memcpy(&pixels[j], &converted, sizeof converted); }
                }
                if (volume) {
                    [texture replaceRegion:MTLRegionMake3D(0, 0, 0, 2, 1, 2) mipmapLevel:0 slice:0
                        withBytes:pixels bytesPerRow:8 bytesPerImage:8];
                } else if (array) {
                    [texture replaceRegion:MTLRegionMake3D(0, 0, 0, 2, 1, 1) mipmapLevel:0 slice:0
                        withBytes:pixels bytesPerRow:8 bytesPerImage:0];
                    [texture replaceRegion:MTLRegionMake3D(0, 0, 0, 2, 1, 1) mipmapLevel:0 slice:1
                        withBytes:pixels + 2 bytesPerRow:8 bytesPerImage:0];
                } else {
                    [texture replaceRegion:MTLRegionMake3D(0, 0, 0, 2, 1, 1) mipmapLevel:0 slice:0
                        withBytes:pixels bytesPerRow:8 bytesPerImage:0];
                }
            }
            BOOL nullTexture = empty || image == group;
            NSUInteger argumentIndex = flat ? 1 + image : 4 + group * 5 + image;
            [encoder setTexture:nullTexture ? nil : texture atIndex:argumentIndex];
        }
        id<MTLSamplerState> sampler = nil;
        if (sampling && !nilSampler) {
            MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
            sd.supportArgumentBuffers = YES;
            sd.normalizedCoordinates = YES;
            sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterNearest;
            sd.mipFilter = MTLSamplerMipFilterNearest;
            sd.sAddressMode = sd.tAddressMode = sd.rAddressMode = MTLSamplerAddressModeClampToEdge;
            sampler = [device newSamplerStateWithDescriptor:sd];
            require(sampler != nil, @"sampler");
        }
        if (sampling) [encoder setSamplerState:sampler atIndex:0];
        for (NSUInteger i = 0; i < arguments.length; ++i)
            if (i < offset || i >= offset + encoder.encodedLength)
                require(((uint8_t *)arguments.contents)[i] == 0xa5, @"argument encoding crossed guards");
        printf("ARGUMENT_NULL_LAYOUT kind=%s path=%s length=%lu alignment=%lu offset=%lu gpu_execution=0\n",
            argv[3], flat ? "flat" : "nested", (unsigned long)encoder.encodedLength,
            (unsigned long)encoder.alignment, (unsigned long)offset);
        if (argc == 5) {
            printf("ARGUMENT_NULL_CAPTURE kind=%s path=%s gpu_execution=0\n", argv[3], flat ? "flat" : "nested");
            return 0;
        }
        id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function error:&error];
        require(pipeline != nil, error.description ?: @"pipeline");
        id<MTLRenderPipelineState> renderPipeline = nil;
        id<MTLTexture> target = nil;
        if (fragmentMode) {
            MTLRenderPipelineDescriptor *renderDescriptor = [MTLRenderPipelineDescriptor new];
            renderDescriptor.vertexFunction = vertexFunction;
            renderDescriptor.fragmentFunction = fragmentFunction;
            renderDescriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
            renderPipeline = [device newRenderPipelineStateWithDescriptor:renderDescriptor error:&error];
            require(renderPipeline != nil, error.description ?: @"render pipeline");
            MTLTextureDescriptor *targetDescriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
                MTLPixelFormatRGBA32Float width:4 height:2 mipmapped:NO];
            targetDescriptor.storageMode = MTLStorageModeShared;
            targetDescriptor.usage = MTLTextureUsageRenderTarget;
            target = [device newTextureWithDescriptor:targetDescriptor];
            require(target != nil, @"render target");
        }
        id<MTLCommandQueue> queue = [device newCommandQueue];
        require(queue != nil, @"command queue");
        id<MTLCommandBuffer> command = [queue commandBuffer];
        require(command != nil, @"command buffer");
        id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
        require(blit != nil, @"argument copy encoder");
        [blit copyFromBuffer:arguments sourceOffset:0 toBuffer:privateArguments destinationOffset:0 size:arguments.length];
        [blit endEncoding];
        id<MTLComputeCommandEncoder> compute = [command computeCommandEncoder];
        require(compute != nil, @"compute encoder");
        [compute setComputePipelineState:pipeline]; [compute setBuffer:privateArguments offset:offset atIndex:3];
        [compute setBuffer:output offset:16 atIndex:0];
        for (unsigned i = 0; i < groups * 2; ++i) {
            unsigned group = i >> 1, image = i & 1;
            if (!empty && image != group)
                [compute useResource:textures[i] usage:MTLResourceUsageRead | (writable ? MTLResourceUsageWrite : 0)];
        }
        [compute dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(invocations, 1, 1)];
        [compute endEncoding];
        if (fragmentMode) {
            MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = target;
            pass.colorAttachments[0].loadAction = MTLLoadActionClear;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            pass.colorAttachments[0].clearColor = MTLClearColorMake(9.0, 9.0, 9.0, 9.0);
            id<MTLRenderCommandEncoder> draw = [command renderCommandEncoderWithDescriptor:pass];
            require(draw != nil, @"render encoder");
            [draw setRenderPipelineState:renderPipeline];
            [draw setFragmentBuffer:privateArguments offset:offset atIndex:3];
            for (unsigned i = 0; i < groups * 2; ++i) {
                unsigned group = i >> 1, image = i & 1;
                if (!empty && image != group)
                    [draw useResource:textures[i] usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
            }
            [draw drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [draw endEncoding];
        }
        if (strcmp(argv[1], "--reference")) {
            for (unsigned i = 0; i < 4; ++i) textures[i] = nil;
            sampler = nil;
        }
        [command commit]; [command waitUntilCompleted];
        require(command.status == MTLCommandBufferStatusCompleted, command.error.description ?: @"execution");
        for (unsigned word = 0; word < 24; ++word) {
            uint32_t expected = 0xcdcdcdcdu;
            if (word >= 4 && word < 4 + 2 * invocations) {
                unsigned i = (word - 4) / 2, x = (i >> 1u) & 1u;
                BOOL nullTexture = empty || (i & 1u) == ((i >> 2u) & 1u);
                if (sampling) {
                    expected = nullTexture ? 0 : gather ? 15 : 49;
                } else {
                    uint32_t before = 21 + ((volume || array) ? 20 : 0) + x + 8;
                    expected = nullTexture ? 0 :
                        writable && (word & 1u) ? 105 + i + 8 : before;
                }
            }
            require(((uint32_t *)output.contents)[word] == expected,
                [NSString stringWithFormat:@"kind=%@ word=%u actual=%u expected=%u", kind, word,
                    ((uint32_t *)output.contents)[word], expected]);
        }
        if (fragmentMode) {
            float pixels[32];
            [target getBytes:pixels bytesPerRow:4 * 4 * sizeof(float)
                fromRegion:MTLRegionMake2D(0, 0, 4, 2) mipmapLevel:0];
            for (unsigned i = 0; i < 8; ++i) {
                // Both fixed backends require implicit sampling to select one image per quad.
                unsigned selection = (i & 3u) >> 1u;
                BOOL nullTexture = empty || !(selection & 1u);
                float expected[4] = {0, 0, 0, 0};
                if (!nullTexture) {
                    if (fragmentGather) {
                        expected[0] = expected[1] = expected[2] = expected[3] = 1.0f;
                    } else if (fragmentBias) {
                        expected[0] = 3.0f; expected[1] = 6.0f; expected[2] = 9.0f; expected[3] = 12.0f;
                    } else {
                        expected[0] = 2.0f; expected[1] = 4.0f; expected[2] = 6.0f; expected[3] = 8.0f;
                    }
                }
                for (unsigned channel = 0; channel < 4; ++channel)
                    require(fabsf(pixels[4*i+channel] - expected[channel]) <= 0.00001f,
                        [NSString stringWithFormat:@"kind=%@ pixel=%u channel=%u actual=%g expected=%g",
                            kind, i, channel, pixels[4*i+channel], expected[channel]]);
            }
        }
        if (fragmentMode) {
            printf("ARGUMENT_NULL_PASS kind=%s path=%s checks=24 pixels=8 source=8x8x4 "
                   "fragment=implicit-lod target=4x2 %s gpu_execution=1\n",
                argv[3], flat ? "flat" : "nested",
                fragmentGather ? "gather=base-level" : fragmentBias ? "sample=bias1-mip2" : "sample=derivative-mip1");
        } else if (sampling) {
            printf("ARGUMENT_NULL_PASS kind=%s path=%s checks=24 null_texture=zero "
                   "field_sampler=%u constexpr_sampler=1 %s gpu_execution=1\n",
                argv[3], flat ? "flat" : "nested", nilSampler ? 0u : 1u,
                gather ? "gather=uniform-rgba" : "sample=explicit-level0");
        } else {
            printf("ARGUMENT_NULL_PASS kind=%s path=%s checks=24 null_read=0 null_write=discard gpu_execution=1\n",
                argv[3], flat ? "flat" : "nested");
        }
    }
    return 0;
}
