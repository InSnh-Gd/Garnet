#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const char *stage = "startup";

static void require(BOOL ok, NSString *message) {
    if (!ok) {
        fprintf(stderr, "ARGUMENT_RESOURCES_FAIL stage=%s %s\n", stage, message.UTF8String);
        exit(1);
    }
}

static void save(NSData *data, NSString *directory, NSString *name) {
    NSError *error = nil;
    require([data writeToFile:[directory stringByAppendingPathComponent:name]
                     options:NSDataWritingAtomic error:&error], error.description ?: @"artifact write failed");
}

int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc < 3 || argc > 4) return 2;
        BOOL encodeOnly = argc == 4 && !strcmp(argv[3], "--encode-only");
        if (argc == 4 && !encodeOnly) return 2;
        setvbuf(stdout, NULL, _IONBF, 0);
        @try {
            NSString *directory = @(argv[2]);
            NSError *error = nil;
            require([[NSFileManager defaultManager] createDirectoryAtPath:directory
                        withIntermediateDirectories:YES attributes:nil error:&error], error.description);
            stage = "device";
            id<MTLDevice> device = nil;
            if (!strcmp(argv[1], "--reference")) device = MTLCreateSystemDefaultDevice();
            else {
                NSBundle *bundle = [NSBundle bundleWithPath:@(argv[1])];
                require([bundle loadAndReturnError:&error], error.description ?: @"bundle load failed");
                Class cls = [bundle classNamed:@"GarnetDevice"];
                require(cls != Nil, @"Metal class missing");
                device = [[cls alloc] init];
            }
            require(device != nil, @"device unavailable");
            printf("ARGUMENT_RESOURCES_DEVICE %s\n", device.name.UTF8String);
            NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
                "struct Resources {\n"
                "  texture2d<float, access::sample> image [[id(2)]];\n"
                "  sampler filtering [[id(7)]];\n"
                "};\n"
                "kernel void argument_resources(device const Resources &args [[buffer(3)]],\n"
                "  device uint4 *result [[buffer(0)]], uint i [[thread_position_in_grid]]) {\n"
                "  float2 uv = float2((i & 1u) ? 0.75f : 0.25f, (i & 2u) ? 0.75f : 0.25f);\n"
                "  result[i] = uint4(args.image.sample(args.filtering, uv, level(0.0f)));\n"
                "}\n";
            save([source dataUsingEncoding:NSUTF8StringEncoding], directory, @"argument-resources.metal");
            stage = "compile";
            id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
            require(library != nil, error.description ?: @"MSL compilation failed");
            id<MTLFunction> function = [library newFunctionWithName:@"argument_resources"];
            require(function != nil, @"kernel missing");
            SEL bitcode = sel_registerName("bitcodeData");
            NSData *air = [function respondsToSelector:bitcode]
                ? ((id (*)(id, SEL))objc_msgSend)(function, bitcode) : nil;
            require(air.length > 0, @"function AIR unavailable");
            save(air, directory, @"argument-resources.air");
            stage = "newArgumentEncoderWithBufferIndex:3";
            puts("ARGUMENT_RESOURCES_STAGE newArgumentEncoderWithBufferIndex:3");
            id<MTLArgumentEncoder> arguments = [function newArgumentEncoderWithBufferIndex:3];
            require(arguments != nil, @"argument encoder missing");
            NSUInteger length = arguments.encodedLength, alignment = arguments.alignment;
            require(length > 0 && length < (1u << 20), @"invalid encodedLength");
            require(alignment > 0 && alignment <= 4096 && !(alignment & (alignment - 1)), @"invalid alignment");
            printf("ARGUMENT_RESOURCES_LAYOUT length=%lu alignment=%lu\n", (unsigned long)length, (unsigned long)alignment);
            NSUInteger offset = ((256 + alignment - 1) / alignment) * alignment;
            id<MTLBuffer> encoded = [device newBufferWithLength:offset + length + 256 options:MTLResourceStorageModeShared];
            require(encoded != nil && encoded.contents != NULL, @"argument allocation failed");
            memset(encoded.contents, 0xa7, encoded.length);
            [arguments setArgumentBuffer:encoded offset:offset];

            stage = "resources";
            MTLTextureDescriptor *textureDescription = [MTLTextureDescriptor
                texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:4 height:4 mipmapped:NO];
            textureDescription.storageMode = MTLStorageModeShared;
            textureDescription.usage = MTLTextureUsageShaderRead;
            id<MTLTexture> texture = [device newTextureWithDescriptor:textureDescription];
            require(texture != nil, @"texture allocation failed");
            float texels[4 * 4 * 4];
            for (unsigned y = 0; y < 4; ++y)
                for (unsigned x = 0; x < 4; ++x) {
                    unsigned quadrant = (x / 2) + (y / 2) * 2;
                    for (unsigned c = 0; c < 4; ++c) texels[(y * 4 + x) * 4 + c] = (float)(quadrant * 4 + c + 1);
                }
            [texture replaceRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0 withBytes:texels bytesPerRow:4 * 4 * sizeof(float)];
            save([NSData dataWithBytes:texels length:sizeof texels], directory, @"texture-input.bin");
            MTLSamplerDescriptor *samplerDescription = [MTLSamplerDescriptor new];
            samplerDescription.minFilter = MTLSamplerMinMagFilterNearest;
            samplerDescription.magFilter = MTLSamplerMinMagFilterNearest;
            samplerDescription.sAddressMode = MTLSamplerAddressModeClampToEdge;
            samplerDescription.tAddressMode = MTLSamplerAddressModeClampToEdge;
            samplerDescription.normalizedCoordinates = YES;
            samplerDescription.supportArgumentBuffers = YES;
            id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:samplerDescription];
            require(sampler != nil, @"sampler allocation failed");
            stage = "setTexture:id2";
            puts("ARGUMENT_RESOURCES_STAGE setTexture:id2");
            [arguments setTexture:texture atIndex:2];
            stage = "setSamplerState:id7";
            puts("ARGUMENT_RESOURCES_STAGE setSamplerState:id7");
            [arguments setSamplerState:sampler atIndex:7];
            const uint8_t *bytes = encoded.contents;
            for (NSUInteger j = 0; j < encoded.length; ++j)
                if (j < offset || j >= offset + length) require(bytes[j] == 0xa7, @"argument guards changed");
            NSData *snapshot = [NSData dataWithBytes:encoded.contents length:encoded.length];
            save(snapshot, directory, @"argument-buffer.bin");
            NSDictionary *layout = @{@"encoded_length": @(length), @"alignment": @(alignment),
                @"argument_buffer_offset": @(offset), @"argument_buffer_index": @3,
                @"texture_id": @2, @"sampler_id": @7, @"output_offset": @256};
            save([NSJSONSerialization dataWithJSONObject:layout options:NSJSONWritingPrettyPrinted error:&error],
                 directory, @"encoder-layout.json");
            puts("ARGUMENT_RESOURCES_ENCODING_PASS texture_id=2 sampler_id=7 guards=unchanged");
            if (encodeOnly) return 0;

            stage = "pipeline";
            puts("ARGUMENT_RESOURCES_STAGE pipeline");
            id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function error:&error];
            require(pipeline != nil, error.description ?: @"pipeline creation failed");
            id<MTLBuffer> output = [device newBufferWithLength:256 + 16 * 4 + 16 options:MTLResourceStorageModeShared];
            require(output != nil && output.contents != NULL, @"output allocation failed");
            uint32_t *words = output.contents;
            for (NSUInteger j = 0; j < output.length / 4; ++j) words[j] = 0x9b000000u + (uint32_t)j;
            id<MTLCommandQueue> queue = [device newCommandQueue];
            id<MTLCommandBuffer> command = [queue commandBuffer];
            id<MTLComputeCommandEncoder> compute = [command computeCommandEncoder];
            require(compute != nil, @"compute encoder unavailable");
            [compute setComputePipelineState:pipeline];
            [compute setBuffer:encoded offset:offset atIndex:3];
            [compute setBuffer:output offset:256 atIndex:0];
            stage = "useResource:texture";
            [compute useResource:texture usage:MTLResourceUsageRead];
            __weak id<MTLTexture> retainedTexture = texture;
            arguments = nil;
            texture = nil;
            require(retainedTexture != nil, @"indirect texture lost before submission");
            [compute dispatchThreadgroups:MTLSizeMake(4, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
            [compute endEncoding];
            stage = "execute";
            [command commit];
            [command waitUntilCompleted];
            require(command.status == MTLCommandBufferStatusCompleted && command.error == nil,
                    command.error.description ?: @"command did not complete");
            save([NSData dataWithBytes:output.contents length:output.length], directory, @"result.bin");
            stage = "verify";
            for (NSUInteger j = 0; j < output.length / 4; ++j) {
                uint32_t expected = j >= 64 && j < 80 ? (uint32_t)(j - 64 + 1) : 0x9b000000u + (uint32_t)j;
                require(words[j] == expected, [NSString stringWithFormat:@"word %lu actual=%08x expected=%08x",
                    (unsigned long)j, words[j], expected]);
            }
            require(memcmp(encoded.contents, snapshot.bytes, encoded.length) == 0, @"GPU changed argument storage");
            puts("ARGUMENT_RESOURCES_PASS quadrants=4 channels=16 exact_values=1..16 output_guards=unchanged argument_guards=unchanged indirect_texture_alive=YES");
        } @catch (NSException *exception) {
            fprintf(stderr, "ARGUMENT_RESOURCES_EXCEPTION stage=%s name=%s reason=%s\n",
                    stage, exception.name.UTF8String, exception.reason.UTF8String);
            return 1;
        }
    }
    return 0;
}
