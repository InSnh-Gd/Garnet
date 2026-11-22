#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

static void require(BOOL ok, NSString *message) {
    if (!ok) { fprintf(stderr, "ARGUMENT_ARRAYS_FAIL %s\n", message.UTF8String); exit(1); }
}

int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc != 3) return 2;
        alarm(90);
        setvbuf(stdout, NULL, _IONBF, 0);
        NSError *error = nil;
        NSString *directory = @(argv[2]);
        require([[NSFileManager defaultManager] createDirectoryAtPath:directory withIntermediateDirectories:YES
            attributes:nil error:&error], error.description ?: @"output directory unavailable");
        id<MTLDevice> device;
        if (!strcmp(argv[1], "--reference")) device = MTLCreateSystemDefaultDevice();
        else {
            NSBundle *bundle = [NSBundle bundleWithPath:@(argv[1])];
            require([bundle loadAndReturnError:&error], error.description ?: @"bundle load failed");
            device = [[[bundle classNamed:@"GarnetDevice"] alloc] init];
        }
        require(device != nil, @"device unavailable");
        NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
            "struct Resources { array<texture2d<float>,2> images [[id(2)]]; "
            "array<sampler,2> filters [[id(7)]]; array<device const uint *,2> values [[id(11)]]; };\n"
            "kernel void argument_arrays(constant Resources &r [[buffer(3)]], "
            "device uint *out [[buffer(0)]], constant uint &flip [[buffer(1)]], uint i [[thread_position_in_grid]]) { "
            "uint t = (i ^ flip) & 1u, s = (i >> 1) & 1u; "
            "out[i] = uint(r.images[t].sample(r.filters[s], float2(1.25f,0.5f), level(0.0f)).x) "
            "+ r.values[t][i]; }\n"
            "vertex float4 argument_arrays_vertex(uint vid [[vertex_id]], constant Resources &r [[buffer(3)]], "
            "constant uint &flip [[buffer(1)]]) { uint t = (vid ^ flip) & 1u, s = (vid >> 1) & 1u; "
            "uint v = uint(r.images[t].sample(r.filters[s], float2(1.25f,0.5f), level(0.0f)).x) + r.values[t][vid]; "
            "uint expected = 1010 + 200*t + (s ? 0 : 10) + (t ? 8 : 4) + vid; "
            "float2 p = float2(float((vid << 1) & 2), float(vid & 2)) * 2.0 - 1.0; "
            "if (v != expected) p += 8.0; return float4(p, 0.0, 1.0); }\n"
            "fragment float4 argument_arrays_fragment(float4 position [[position]], constant Resources &r [[buffer(3)]], "
            "constant uint &flip [[buffer(1)]]) { uint i = uint(position.x) + 4 * uint(position.y); "
            "uint t = (i ^ flip) & 1u, s = (i >> 1) & 1u; "
            "uint v = uint(r.images[t].sample(r.filters[s], float2(1.25f,0.5f), level(0.0f)).x) + r.values[t][i]; "
            "return float4(float(v & 255u) / 255.0, float(t), float(s), 1.0); }\n";
        require([[source dataUsingEncoding:NSUTF8StringEncoding] writeToFile:
            [directory stringByAppendingPathComponent:@"argument-arrays.metal"] atomically:YES], @"source write failed");
        MTLCompileOptions *options = [MTLCompileOptions new];
        options.languageVersion = MTLLanguageVersion2_1;
        id<MTLLibrary> library = [device newLibraryWithSource:source options:options error:&error];
        require(library != nil, error.description ?: @"source compilation failed");
        id<MTLFunction> function = [library newFunctionWithName:@"argument_arrays"];
        require(function != nil, @"function unavailable");
        SEL bitcode = NSSelectorFromString(@"bitcodeData");
        NSData *air = [function respondsToSelector:bitcode] ? ((id (*)(id, SEL))objc_msgSend)(function, bitcode) : nil;
        require(air.length && [air writeToFile:[directory stringByAppendingPathComponent:@"argument-arrays.air"]
            atomically:YES], @"AIR artifact write failed");
        id<MTLArgumentEncoder> arguments = [function newArgumentEncoderWithBufferIndex:3];
        require(arguments && arguments.encodedLength && arguments.encodedLength < 4096 &&
            arguments.alignment && arguments.alignment <= 4096, @"array layout unavailable");
        NSUInteger offset = (256 + arguments.alignment - 1) / arguments.alignment * arguments.alignment;
        id<MTLBuffer> table = [device newBufferWithLength:offset + arguments.encodedLength + 256
            options:MTLResourceStorageModeShared];
        require(table && table.contents, @"argument allocation failed");
        memset(table.contents, 0xa7, table.length);
        [arguments setArgumentBuffer:table offset:offset];
        id<MTLTexture> textures[2];
        id<MTLSamplerState> samplers[2];
        id<MTLBuffer> inputs[2];
        NSUInteger offsets[2] = {16, 32};
        for (NSUInteger k = 0; k < 2; ++k) {
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float
                width:2 height:1 mipmapped:NO];
            td.storageMode = MTLStorageModeShared; td.usage = MTLTextureUsageShaderRead;
            textures[k] = [device newTextureWithDescriptor:td];
            float pixels[2] = {(float)(10 + k * 100), (float)(20 + k * 100)};
            require(textures[k] != nil, @"texture allocation failed");
            [textures[k] replaceRegion:MTLRegionMake2D(0, 0, 2, 1) mipmapLevel:0 withBytes:pixels bytesPerRow:sizeof pixels];
            MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
            sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterNearest;
            sd.sAddressMode = k ? MTLSamplerAddressModeRepeat : MTLSamplerAddressModeClampToEdge;
            sd.supportArgumentBuffers = YES;
            samplers[k] = [device newSamplerStateWithDescriptor:sd];
            inputs[k] = [device newBufferWithLength:512 options:MTLResourceStorageModeShared];
            require(samplers[k] && inputs[k] && inputs[k].contents, @"indirect allocation failed");
            for (NSUInteger i = 0; i < 128; ++i) ((uint32_t *)inputs[k].contents)[i] = 1000 + (uint32_t)(100 * k + i);
        }
        [arguments setTextures:textures withRange:NSMakeRange(2, 2)];
        [arguments setSamplerStates:samplers withRange:NSMakeRange(7, 2)];
        [arguments setBuffers:inputs offsets:offsets withRange:NSMakeRange(11, 2)];
        for (NSUInteger i = 0; i < table.length; ++i)
            if (i < offset || i >= offset + arguments.encodedLength)
                require(((uint8_t *)table.contents)[i] == 0xa7, @"argument encoding crossed guards");
        NSData *before = [NSData dataWithBytes:table.contents length:table.length];
        printf("ARGUMENT_ARRAYS_ENCODING_PASS length=%lu alignment=%lu images=2 samplers=2 pointers=2\n",
            (unsigned long)arguments.encodedLength, (unsigned long)arguments.alignment);
        id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function error:&error];
        require(pipeline != nil, error.description ?: @"pipeline creation failed");
        MTLRenderPipelineDescriptor *renderDescriptor = [MTLRenderPipelineDescriptor new];
        renderDescriptor.vertexFunction = [library newFunctionWithName:@"argument_arrays_vertex"];
        renderDescriptor.fragmentFunction = [library newFunctionWithName:@"argument_arrays_fragment"];
        renderDescriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        require(renderDescriptor.vertexFunction && renderDescriptor.fragmentFunction, @"render array functions unavailable");
        for (id<MTLFunction> stage in @[renderDescriptor.vertexFunction, renderDescriptor.fragmentFunction]) {
            NSData *stageAIR = [stage respondsToSelector:bitcode] ? ((id (*)(id, SEL))objc_msgSend)(stage, bitcode) : nil;
            require(stageAIR.length && [stageAIR writeToFile:[directory stringByAppendingPathComponent:
                [stage.name stringByAppendingString:@".air"]] atomically:YES], @"render AIR artifact write failed");
        }
        id<MTLRenderPipelineState> render = [device newRenderPipelineStateWithDescriptor:renderDescriptor error:&error];
        require(render != nil, error.description ?: @"render array pipeline creation failed");
        id<MTLCommandQueue> queue = [device newCommandQueue];
        require(queue != nil, @"queue unavailable");
        for (uint32_t flip = 0; flip < 2; ++flip) {
            id<MTLBuffer> output = [device newBufferWithLength:768 options:MTLResourceStorageModeShared];
            require(output && output.contents, @"output allocation failed");
            memset(output.contents, 0x9b, output.length);
            id<MTLCommandBuffer> command = [queue commandBuffer];
            id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
            require(encoder != nil, @"encoder unavailable");
            [encoder setComputePipelineState:pipeline];
            [encoder setBuffer:table offset:offset atIndex:3];
            [encoder setBuffer:output offset:256 atIndex:0];
            [encoder setBytes:&flip length:sizeof flip atIndex:1];
            for (NSUInteger k = 0; k < 2; ++k) {
                [encoder useResource:textures[k] usage:MTLResourceUsageRead];
                [encoder useResource:inputs[k] usage:MTLResourceUsageRead];
            }
            [encoder dispatchThreads:MTLSizeMake(64, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
            [encoder endEncoding]; [command commit]; [command waitUntilCompleted];
            require(command.status == MTLCommandBufferStatusCompleted && !command.error,
                command.error.description ?: @"command failed");
            const uint32_t *words = output.contents;
            for (NSUInteger j = 0; j < output.length / 4; ++j) {
                uint32_t expected = 0x9b9b9b9bu;
                if (j >= 64 && j < 128) {
                    uint32_t i = (uint32_t)j - 64, t = (i ^ flip) & 1u, s = (i >> 1) & 1u;
                    expected = 10 + t * 100 + (s ? 0 : 10) + 1000 + 100 * t + (uint32_t)offsets[t] / 4 + i;
                }
                require(words[j] == expected, [NSString stringWithFormat:@"flip=%u word=%lu actual=%u expected=%u",
                    flip, (unsigned long)j, words[j], expected]);
            }
            require(!memcmp(before.bytes, table.contents, table.length), @"GPU changed argument storage");
            printf("ARGUMENT_ARRAYS_ROUND_PASS flip=%u outputs=64 nonuniform_images=1 nonuniform_samplers=1 nonuniform_pointers=1 guards=exact\n", flip);

            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                width:4 height:4 mipmapped:NO];
            td.storageMode = MTLStorageModeShared; td.usage = MTLTextureUsageRenderTarget;
            id<MTLTexture> pixels = [device newTextureWithDescriptor:td];
            require(pixels != nil, @"render target allocation failed");
            MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = pixels;
            pass.colorAttachments[0].loadAction = MTLLoadActionClear;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            pass.colorAttachments[0].clearColor = MTLClearColorMake(1, 0, 1, 1);
            command = [queue commandBuffer];
            id<MTLRenderCommandEncoder> draw = [command renderCommandEncoderWithDescriptor:pass];
            require(draw != nil, @"render encoder unavailable");
            [draw setRenderPipelineState:render];
            [draw setVertexBuffer:table offset:offset atIndex:3];
            [draw setFragmentBuffer:table offset:offset atIndex:3];
            [draw setVertexBytes:&flip length:sizeof flip atIndex:1];
            [draw setFragmentBytes:&flip length:sizeof flip atIndex:1];
            for (NSUInteger k = 0; k < 2; ++k) {
                [draw useResource:textures[k] usage:MTLResourceUsageRead stages:MTLRenderStageVertex | MTLRenderStageFragment];
                [draw useResource:inputs[k] usage:MTLResourceUsageRead stages:MTLRenderStageVertex | MTLRenderStageFragment];
            }
            [draw drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [draw endEncoding]; [command commit]; [command waitUntilCompleted];
            require(command.status == MTLCommandBufferStatusCompleted && !command.error,
                command.error.description ?: @"array render command failed");
            uint8_t colors[64];
            [pixels getBytes:colors bytesPerRow:16 fromRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0];
            for (uint32_t i = 0; i < 16; ++i) {
                uint32_t t = (i ^ flip) & 1u, s = (i >> 1) & 1u;
                uint32_t v = 1010 + t * 200 + (s ? 0 : 10) + (uint32_t)offsets[t] / 4 + i;
                require(colors[i * 4] == s * 255 && colors[i * 4 + 1] == t * 255 &&
                    colors[i * 4 + 2] == (v & 255u) && colors[i * 4 + 3] == 255,
                    [NSString stringWithFormat:@"flip=%u pixel=%u BGRA=%u,%u,%u,%u expected=%u,%u,%u,255",
                     flip, i, colors[i * 4], colors[i * 4 + 1], colors[i * 4 + 2], colors[i * 4 + 3], s * 255, t * 255, v & 255u]);
            }
            require(!memcmp(before.bytes, table.contents, table.length), @"render changed argument storage");
            printf("ARGUMENT_ARRAYS_RENDER_PASS flip=%u pixels=16 stages=vertex,fragment dynamic_images=1 dynamic_samplers=1 dynamic_pointers=1\n", flip);
        }
        puts("ARGUMENT_ARRAYS_PASS rounds=2 outputs=128 msl=2.1");
    }
    return 0;
}
