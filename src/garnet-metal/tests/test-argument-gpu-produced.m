#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

static void require(BOOL ok, NSString *message) {
    if (!ok) { fprintf(stderr, "ARGUMENT_GPU_FAIL %s\n", message.UTF8String); exit(1); }
}

int main(int argc, const char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        puts("Usage: test-argument-gpu-produced BUNDLE|--reference OUTPUT_DIR");
        return 0;
    }
    if (argc != 3) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(90);
    @autoreleasepool {
        NSError *error = nil;
        NSString *directory = @(argv[2]);
        require([[NSFileManager defaultManager] createDirectoryAtPath:directory withIntermediateDirectories:YES
            attributes:nil error:&error], error.description ?: @"output directory unavailable");
        id<MTLDevice> device = nil;
        if (!strcmp(argv[1], "--reference")) device = MTLCreateSystemDefaultDevice();
        else {
            NSBundle *bundle = [NSBundle bundleWithPath:@(argv[1])];
            require([bundle loadAndReturnError:&error], error.description ?: @"bundle load failed");
            device = [[[bundle classNamed:@"GarnetDevice"] alloc] init];
        }
        require(device != nil, @"Metal device unavailable");
        NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
            "struct Args { device const uint *words [[id(2)]]; uint bias [[id(5)]]; };\n"
            "kernel void produce(device Args &dst [[buffer(3)]], device const Args &src [[buffer(5)]]) {"
            " dst.words = src.words + 4; dst.bias = src.bias + 37; }\n"
            "kernel void consume(device const Args &args [[buffer(3)]], device uint *out [[buffer(0)]],"
            " uint i [[thread_position_in_grid]]) { out[i] = (args.words[i] ^ args.bias) + i; }\n"
            "vertex float4 vertex_consume(device const Args &args [[buffer(3)]], uint vid [[vertex_id]]) {"
            " float2 p = float2(float((vid << 1) & 2), float(vid & 2)) * 2.0 - 1.0;"
            " if (args.words[1] != args.bias) p += 8.0; return float4(p, 0.0, 1.0); }\n"
            "fragment float4 fragment_consume(device const Args &args [[buffer(3)]]) {"
            " return args.words[0] == 0xfeedfaceu ? float4(0, float(args.bias & 1u), 1, 1) : float4(1, 0, 0, 1); }\n";
        require([source writeToFile:[directory stringByAppendingPathComponent:@"gpu-arguments.metal"]
            atomically:YES encoding:NSUTF8StringEncoding error:&error], @"source write failed");
        MTLCompileOptions *options = [MTLCompileOptions new];
        options.languageVersion = MTLLanguageVersion2_1;
        for (NSString *sampler in @[@"input", @"local"]) {
            NSString *invalid = [NSString stringWithFormat:
                @"#include <metal_stdlib>\nusing namespace metal;\n"
                 "struct Resources { sampler filter [[id(0)]]; };\n"
                 "kernel void invalid_sampler(device Resources &dst [[buffer(0)]], sampler input [[sampler(0)]]) {"
                 " constexpr sampler local; dst.filter = %@; }\n", sampler];
            NSError *rejection = nil;
            id<MTLLibrary> samplerLibrary = [device newLibraryWithSource:invalid options:options error:&rejection];
            if (samplerLibrary) {
                id<MTLFunction> invalidFunction = [samplerLibrary newFunctionWithName:@"invalid_sampler"];
                NSData *air = ((id (*)(id, SEL))objc_msgSend)(invalidFunction, NSSelectorFromString(@"bitcodeData"));
                require(air.length && [air writeToFile:[directory stringByAppendingPathComponent:
                    [sampler stringByAppendingString:@"-sampler.air"]] atomically:YES], @"invalid sampler AIR write failed");
                // Check these AIR inputs offline: the VM reference runtime aborts on this unsupported pipeline.
                printf("ARGUMENT_GPU_SAMPLER_CAPTURE sampler=%s pipeline=not-created\n", sampler.UTF8String);
            } else require(rejection != nil, @"sampler compilation failed without an error");
        }
        id<MTLLibrary> library = [device newLibraryWithSource:source options:options error:&error];
        require(library != nil, error.description ?: @"source compilation failed");
        NSMutableDictionary *functions = [NSMutableDictionary dictionary];
        for (NSString *name in @[@"produce", @"consume", @"vertex_consume", @"fragment_consume"]) {
            id<MTLFunction> function = [library newFunctionWithName:name];
            require(function != nil, [@"missing function " stringByAppendingString:name]);
            functions[name] = function;
            SEL selector = NSSelectorFromString(@"bitcodeData");
            NSData *air = [function respondsToSelector:selector] ? ((id (*)(id, SEL))objc_msgSend)(function, selector) : nil;
            require(air.length && [air writeToFile:[directory stringByAppendingPathComponent:
                [name stringByAppendingString:@".air"]] atomically:YES], @"AIR write failed");
        }
        id<MTLArgumentEncoder> layout = [functions[@"consume"] newArgumentEncoderWithBufferIndex:3];
        require(layout && layout.encodedLength && layout.encodedLength <= 4096 &&
            layout.alignment && layout.alignment <= 256 && !(256 % layout.alignment), @"argument layout unavailable");
        id<MTLComputePipelineState> producer = [device newComputePipelineStateWithFunction:functions[@"produce"] error:&error];
        require(producer != nil, error.description ?: @"producer pipeline failed");
        id<MTLComputePipelineState> consumer = [device newComputePipelineStateWithFunction:functions[@"consume"] error:&error];
        require(consumer != nil, error.description ?: @"consumer pipeline failed");
        MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
        descriptor.vertexFunction = functions[@"vertex_consume"];
        descriptor.fragmentFunction = functions[@"fragment_consume"];
        descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        id<MTLRenderPipelineState> render = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        require(render != nil, error.description ?: @"render pipeline failed");

        enum { count = 32, guard = 4, offset = 256 };
        id<MTLBuffer> arguments = [device newBufferWithLength:offset + layout.encodedLength + 256
                                                    options:MTLResourceStorageModeShared];
        id<MTLBuffer> seed = [device newBufferWithLength:arguments.length options:MTLResourceStorageModeShared];
        id<MTLBuffer> decoy = [device newBufferWithLength:256 options:MTLResourceStorageModeShared];
        require(arguments && seed && decoy && arguments.contents && seed.contents && decoy.contents, @"argument allocation failed");
        memset(arguments.contents, 0xa5, arguments.length);
        memset(seed.contents, 0xb6, seed.length);
        memset(decoy.contents, 0, decoy.length);
        [layout setArgumentBuffer:arguments offset:offset];
        [layout setBuffer:decoy offset:0 atIndex:2];
        *(uint32_t *)[layout constantDataAtIndex:5] = 0xdead;
        id<MTLCommandQueue> queue = [device newCommandQueue];
        require(queue != nil, @"queue unavailable");

        for (uint32_t round = 0; round < 2; ++round) {
            @autoreleasepool {
                uint32_t initial[count + 2 * guard];
                for (unsigned i = 0; i < count + 2 * guard; ++i) initial[i] = 0x12340000u + round * 1000 + i * 37;
                initial[guard] = 0xfeedfaceu; initial[guard + 1] = round + 100 + 37;
                id<MTLBuffer> target = [device newBufferWithBytes:initial length:sizeof initial options:MTLResourceStorageModeShared];
                id<MTLBuffer> output = [device newBufferWithLength:offset + count * sizeof(uint32_t) + 256 options:MTLResourceStorageModeShared];
                MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                    width:4 height:4 mipmapped:NO];
                td.storageMode = MTLStorageModeShared; td.usage = MTLTextureUsageRenderTarget;
                id<MTLTexture> pixels = [device newTextureWithDescriptor:td];
                id<MTLFence> fence = [device newFence];
                require(target && output && output.contents && pixels && fence, @"round allocation failed");
                memset(output.contents, 0xcd, output.length);
                [layout setArgumentBuffer:seed offset:offset];
                [layout setBuffer:target offset:0 atIndex:2];
                *(uint32_t *)[layout constantDataAtIndex:5] = round + 100;
                NSData *seedBefore = [NSData dataWithBytes:seed.contents length:seed.length];
                id<MTLCommandBuffer> command = [queue commandBuffer];
                id<MTLComputeCommandEncoder> encode = [command computeCommandEncoder];
                require(encode != nil, @"producer encoder unavailable");
                [encode setComputePipelineState:producer];
                [encode setBuffer:arguments offset:offset atIndex:3];
                [encode setBuffer:seed offset:offset atIndex:5];
                [encode useResource:target usage:MTLResourceUsageRead];
                [encode dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
                [encode updateFence:fence]; [encode endEncoding];

                MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
                pass.colorAttachments[0].texture = pixels;
                pass.colorAttachments[0].loadAction = MTLLoadActionClear;
                pass.colorAttachments[0].storeAction = MTLStoreActionStore;
                pass.colorAttachments[0].clearColor = MTLClearColorMake(1, 0, 1, 1);
                id<MTLRenderCommandEncoder> draw = [command renderCommandEncoderWithDescriptor:pass];
                require(draw != nil, @"render encoder unavailable");
                [draw waitForFence:fence beforeStages:MTLRenderStageVertex];
                [draw setRenderPipelineState:render];
                [draw setVertexBuffer:arguments offset:offset atIndex:3];
                [draw setFragmentBuffer:arguments offset:offset atIndex:3];
                [draw useResource:target usage:MTLResourceUsageRead stages:MTLRenderStageVertex | MTLRenderStageFragment];
                [draw drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                [draw endEncoding];

                encode = [command computeCommandEncoder];
                require(encode != nil, @"consumer encoder unavailable");
                [encode waitForFence:fence];
                [encode setComputePipelineState:consumer];
                [encode setBuffer:arguments offset:offset atIndex:3];
                [encode setBuffer:output offset:offset atIndex:0];
                [encode useResource:target usage:MTLResourceUsageRead];
                __weak id<MTLBuffer> weakTarget = target;
                target = nil;
                require(weakTarget != nil, @"useResource lost indirect target before submission");
                [encode dispatchThreadgroups:MTLSizeMake(count, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
                [encode endEncoding]; [command commit]; [command waitUntilCompleted];
                require(command.status == MTLCommandBufferStatusCompleted && !command.error,
                    command.error.description ?: @"command failed");
                const uint32_t *actual = output.contents;
                for (NSUInteger j = 0; j < output.length / 4; ++j) {
                    uint32_t expected = 0xcdcdcdcdu;
                    if (j >= offset / 4 && j < offset / 4 + count) {
                        unsigned i = (unsigned)j - offset / 4;
                        expected = (initial[guard + i] ^ (round + 100 + 37)) + i;
                    }
                    require(actual[j] == expected, [NSString stringWithFormat:@"round=%u word=%lu actual=%#x expected=%#x",
                        round, (unsigned long)j, actual[j], expected]);
                }
                uint8_t colors[4 * 4 * 4];
                [pixels getBytes:colors bytesPerRow:16 fromRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0];
                for (unsigned i = 0; i < 16; ++i)
                    require(colors[i * 4] == 255 && colors[i * 4 + 1] == (round ? 0 : 255) &&
                        colors[i * 4 + 2] == 0 && colors[i * 4 + 3] == 255, @"render output mismatch");
                for (NSUInteger i = 0; i < arguments.length; ++i)
                    if (i < offset || i >= offset + layout.encodedLength)
                        require(((const uint8_t *)arguments.contents)[i] == 0xa5, @"argument guard changed");
                require(!memcmp(seedBefore.bytes, seed.contents, seed.length), @"producer changed source arguments");
                printf("ARGUMENT_GPU_ROUND_PASS round=%u words=32 pixels=16 stages=compute,vertex,fragment fence=1 early_release=1 guards=exact\n", round);
            }
        }
        puts("ARGUMENT_GPU_PASS rounds=2 buffer_pointer=device-copy msl=2.1");
    }
    return 0;
}
