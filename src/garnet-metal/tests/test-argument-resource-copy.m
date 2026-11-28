#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

static void require(BOOL ok, NSString *message) {
    if (!ok) { fprintf(stderr, "ARGUMENT_COPY_FAIL %s\n", message.UTF8String); exit(1); }
}

int main(int argc, const char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        puts("Usage: test-argument-resource-copy BUNDLE|--reference OUTPUT_DIR");
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
            "struct Args { array<texture2d<float>, 2> images [[id(2)]];"
            " array<sampler, 2> filters [[id(7)]]; constant uint *data [[id(11)]]; };\n"
            "kernel void copy_resources(constant Args &src [[buffer(3)]], device Args &dst [[buffer(5)]],"
            " device uint *out [[buffer(0)]], constant uint &flip [[buffer(1)]]) {"
            " for (uint j = 0; j != 2; ++j) { uint i = (j ^ flip) & 1u;"
            " dst.images[j] = src.images[i]; dst.filters[j] = src.filters[1u - i]; }"
            " dst.data = src.data + 4; out[0] = src.data[0]; out[1] = dst.data[1]; }\n"
            "kernel void consume_resources(constant Args &args [[buffer(5)]], device uint *out [[buffer(0)]],"
            " uint i [[thread_position_in_grid]]) {"
            " out[i] = args.data[i] + uint(args.images[i & 1u].sample(args.filters[(i >> 1u) & 1u], float2(1.25, 0.5)).x); }\n"
            "vertex float4 resource_vertex(uint vid [[vertex_id]], constant Args &args [[buffer(5)]],"
            " constant uint &flip [[buffer(1)]]) {"
            " uint image = (vid & 1u) ^ flip, filter = 1u - (((vid >> 1u) & 1u) ^ flip);"
            " uint pixel = image ? (filter ? 83u : 47u) : (filter ? 29u : 11u);"
            " uint value = args.data[vid] + uint(args.images[vid & 1u].sample(args.filters[(vid >> 1u) & 1u], float2(1.25, 0.5), level(0.0)).x);"
            " float2 p = float2(float((vid << 1) & 2), float(vid & 2)) * 2.0 - 1.0;"
            " if (value != 0x76540000u + flip * 97u + (4u + vid) * 13u + pixel) p += 8.0;"
            " return float4(p, 0.0, 1.0); }\n"
            "fragment float4 resource_fragment(float4 position [[position]], constant Args &args [[buffer(5)]]) {"
            " uint i = uint(position.x) + 4u * uint(position.y);"
            " uint value = args.data[i] + uint(args.images[i & 1u].sample(args.filters[(i >> 1u) & 1u], float2(1.25, 0.5)).x);"
            " return float4(float(value & 255u) / 255.0, float(i & 1u), float((i >> 1u) & 1u), 1.0); }\n";
        require([source writeToFile:[directory stringByAppendingPathComponent:@"resource-copy.metal"]
            atomically:YES encoding:NSUTF8StringEncoding error:&error], @"source write failed");
        MTLCompileOptions *options = [MTLCompileOptions new];
        options.languageVersion = MTLLanguageVersion2_1;
        id<MTLLibrary> library = [device newLibraryWithSource:source options:options error:&error];
        require(library != nil, error.description ?: @"source compilation failed");
        id<MTLFunction> function = [library newFunctionWithName:@"copy_resources"];
        require(function != nil, @"missing copy function");
        NSData *air = [function respondsToSelector:NSSelectorFromString(@"bitcodeData")] ?
            ((id (*)(id, SEL))objc_msgSend)(function, NSSelectorFromString(@"bitcodeData")) : nil;
        require(air.length && [air writeToFile:[directory stringByAppendingPathComponent:@"resource-copy.air"]
            atomically:YES], @"AIR write failed");
        id<MTLArgumentEncoder> layout = [function newArgumentEncoderWithBufferIndex:5];
        require(layout && layout.encodedLength >= 40 && layout.encodedLength <= 4096 &&
            layout.alignment && !(256 % layout.alignment), @"argument layout unavailable");
        printf("ARGUMENT_COPY_LAYOUT bytes=%lu alignment=%lu\n",
            (unsigned long)layout.encodedLength, (unsigned long)layout.alignment);
        id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function error:&error];
        require(pipeline != nil, error.description ?: @"producer pipeline failed");
        id<MTLFunction> consumerFunction = [library newFunctionWithName:@"consume_resources"];
        id<MTLComputePipelineState> consumer = [device newComputePipelineStateWithFunction:consumerFunction error:&error];
        require(consumer != nil, error.description ?: @"consumer pipeline failed");
        MTLRenderPipelineDescriptor *renderDescriptor = [MTLRenderPipelineDescriptor new];
        renderDescriptor.vertexFunction = [library newFunctionWithName:@"resource_vertex"];
        renderDescriptor.fragmentFunction = [library newFunctionWithName:@"resource_fragment"];
        renderDescriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        id<MTLRenderPipelineState> render = [device newRenderPipelineStateWithDescriptor:renderDescriptor error:&error];
        require(render != nil, error.description ?: @"render pipeline failed");
        id<MTLFence> fence = [device newFence];
        require(fence != nil, @"fence unavailable");
        id<MTLCommandQueue> queue = [device newCommandQueue];
        require(queue != nil, @"command queue unavailable");
        enum { offset = 256, dataCount = 16 };
        NSUInteger length = offset + layout.encodedLength + 256;
        for (NSUInteger mode = 0; mode < 2; ++mode) {
            for (uint32_t round = 0; round < 2; ++round) {
                @autoreleasepool {
                    id<MTLBuffer> seed = [device newBufferWithLength:length options:MTLResourceStorageModeShared];
                    id<MTLBuffer> initial = [device newBufferWithLength:length options:MTLResourceStorageModeShared];
                    id<MTLBuffer> expected = [device newBufferWithLength:length options:MTLResourceStorageModeShared];
                    id<MTLBuffer> readback = [device newBufferWithLength:length options:MTLResourceStorageModeShared];
                    id<MTLBuffer> destination = mode ? [device newBufferWithLength:length options:MTLResourceStorageModePrivate] : readback;
                    id<MTLBuffer> data = [device newBufferWithLength:dataCount * sizeof(uint32_t) options:MTLResourceStorageModeShared];
                    id<MTLBuffer> output = [device newBufferWithLength:128 options:MTLResourceStorageModeShared];
                    id<MTLBuffer> flip = [device newBufferWithBytes:&round length:sizeof round options:MTLResourceStorageModeShared];
                    require(seed && initial && expected && readback && destination && data && output && flip,
                        @"buffer allocation failed");
                    memset(seed.contents, 0xb6, length);
                    memset(initial.contents, 0xa5, length);
                    memset(expected.contents, 0xa5, length);
                    memset(readback.contents, 0xa5, length);
                    memset(output.contents, 0xcd, output.length);
                    uint32_t values[dataCount];
                    for (unsigned i = 0; i < dataCount; ++i) values[i] = 0x76540000u + round * 97 + i * 13;
                    memcpy(data.contents, values, sizeof values);
                    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                        width:2 height:1 mipmapped:NO];
                    descriptor.storageMode = MTLStorageModeShared;
                    descriptor.usage = MTLTextureUsageShaderRead;
                    id<MTLTexture> images[2] = { [device newTextureWithDescriptor:descriptor], [device newTextureWithDescriptor:descriptor] };
                    id<MTLTexture> upload[2] = {images[0], images[1]};
                    id<MTLHeap> heap = nil;
                    if (mode) {
                        descriptor.storageMode = MTLStorageModePrivate;
                        MTLSizeAndAlign size = [device heapTextureSizeAndAlignWithDescriptor:descriptor];
                        require(size.size && size.align && size.size < 16 * 1024 * 1024, @"heap texture requirements unavailable");
                        MTLHeapDescriptor *heapDescriptor = [MTLHeapDescriptor new];
                        heapDescriptor.storageMode = MTLStorageModePrivate;
                        heapDescriptor.size = 4 * ((size.size + size.align - 1) / size.align * size.align);
                        heap = [device newHeapWithDescriptor:heapDescriptor];
                        images[0] = [heap newTextureWithDescriptor:descriptor]; images[1] = [heap newTextureWithDescriptor:descriptor];
                        require(heap && images[0] && images[1], @"heap resource allocation failed");
                    }
                    MTLSamplerDescriptor *sampler = [MTLSamplerDescriptor new];
                    sampler.supportArgumentBuffers = YES;
                    sampler.sAddressMode = MTLSamplerAddressModeRepeat;
                    id<MTLSamplerState> filters[2];
                    filters[0] = [device newSamplerStateWithDescriptor:sampler];
                    sampler.sAddressMode = MTLSamplerAddressModeClampToEdge;
                    filters[1] = [device newSamplerStateWithDescriptor:sampler];
                    require(images[0] && images[1] && filters[0] && filters[1], @"resource allocation failed");
                    const float pixels[2][8] = {{11, 0, 0, 1, 29, 0, 0, 1}, {47, 0, 0, 1, 83, 0, 0, 1}};
                    for (unsigned i = 0; i < 2; ++i)
                        [upload[i] replaceRegion:MTLRegionMake2D(0, 0, 2, 1) mipmapLevel:0 withBytes:pixels[i] bytesPerRow:sizeof pixels[i]];
                    [layout setArgumentBuffer:seed offset:offset];
                    NSMutableArray *extraSamplers = [NSMutableArray array];
                    for (unsigned i = 0; i < 128; ++i) {
                        MTLSamplerDescriptor *extra = [MTLSamplerDescriptor new];
                        extra.supportArgumentBuffers = YES; extra.lodMaxClamp = (float)i + 1;
                        id<MTLSamplerState> state = [device newSamplerStateWithDescriptor:extra];
                        require(state != nil, @"extra sampler allocation failed");
                        [extraSamplers addObject:state];
                        [layout setSamplerState:state atIndex:7];
                    }
                    for (unsigned i = 0; i < 2; ++i) {
                        [layout setTexture:images[i] atIndex:2 + i];
                        [layout setSamplerState:filters[i] atIndex:7 + i];
                    }
                    [layout setBuffer:data offset:0 atIndex:11];
                    [layout setArgumentBuffer:expected offset:offset];
                    for (unsigned j = 0; j < 2; ++j) {
                        unsigned i = (j ^ round) & 1u;
                        [layout setTexture:images[i] atIndex:2 + j];
                        [layout setSamplerState:filters[1u - i] atIndex:7 + j];
                    }
                    [layout setBuffer:data offset:4 * sizeof(uint32_t) atIndex:11];
                    NSData *seedBefore = [NSData dataWithBytes:seed.contents length:length];
                    if (heap) {
                        id<MTLCommandBuffer> uploadCommand = [queue commandBuffer];
                        id<MTLBlitCommandEncoder> uploadEncoder = [uploadCommand blitCommandEncoder];
                        require(uploadEncoder != nil, @"heap upload encoder unavailable");
                        for (unsigned i = 0; i < 2; ++i)
                            [uploadEncoder copyFromTexture:upload[i] sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                                sourceSize:MTLSizeMake(2, 1, 1) toTexture:images[i] destinationSlice:0 destinationLevel:0
                                destinationOrigin:MTLOriginMake(0, 0, 0)];
                        [uploadEncoder endEncoding]; [uploadCommand commit]; [uploadCommand waitUntilCompleted];
                        require(uploadCommand.status == MTLCommandBufferStatusCompleted, uploadCommand.error.description ?: @"heap upload failed");
                    }
                    id<MTLCommandBuffer> command = [queue commandBuffer];
                    id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
                    require(blit != nil, @"initialization encoder unavailable");
                    [blit copyFromBuffer:initial sourceOffset:0 toBuffer:destination destinationOffset:0 size:length];
                    [blit endEncoding];
                    id<MTLComputeCommandEncoder> compute = [command computeCommandEncoder];
                    require(compute != nil, @"compute encoder unavailable");
                    [compute setComputePipelineState:pipeline];
                    [compute setBuffer:seed offset:offset atIndex:3];
                    [compute setBuffer:destination offset:offset atIndex:5];
                    [compute setBuffer:output offset:16 atIndex:0];
                    [compute setBuffer:flip offset:0 atIndex:1];
                    [compute useResource:data usage:MTLResourceUsageRead];
                    if (heap) [compute useHeap:heap];
                    else for (unsigned i = 0; i < 2; ++i) [compute useResource:images[i] usage:MTLResourceUsageRead];
                    [compute dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
                    [compute updateFence:fence];
                    [compute endEncoding];
                    compute = [command computeCommandEncoder];
                    require(compute != nil, @"consumer encoder unavailable");
                    [compute waitForFence:fence];
                    [compute setComputePipelineState:consumer];
                    [compute setBuffer:destination offset:offset atIndex:5];
                    [compute setBuffer:output offset:32 atIndex:0];
                    [compute useResource:data usage:MTLResourceUsageRead];
                    if (heap) [compute useHeap:heap];
                    else for (unsigned i = 0; i < 2; ++i) [compute useResource:images[i] usage:MTLResourceUsageRead];
                    [compute dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(8, 1, 1)];
                    [compute endEncoding];

                    MTLTextureDescriptor *targetDescriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                        width:4 height:2 mipmapped:NO];
                    targetDescriptor.storageMode = MTLStorageModeShared; targetDescriptor.usage = MTLTextureUsageRenderTarget;
                    id<MTLTexture> target = [device newTextureWithDescriptor:targetDescriptor];
                    require(target != nil, @"render target unavailable");
                    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
                    pass.colorAttachments[0].texture = target; pass.colorAttachments[0].loadAction = MTLLoadActionClear;
                    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
                    pass.colorAttachments[0].clearColor = MTLClearColorMake(1, 0, 1, 1);
                    id<MTLRenderCommandEncoder> draw = [command renderCommandEncoderWithDescriptor:pass];
                    require(draw != nil, @"render encoder unavailable");
                    [draw waitForFence:fence beforeStages:MTLRenderStageVertex];
                    [draw setRenderPipelineState:render];
                    [draw setVertexBuffer:destination offset:offset atIndex:5];
                    [draw setFragmentBuffer:destination offset:offset atIndex:5];
                    [draw setVertexBytes:&round length:sizeof round atIndex:1];
                    [draw useResource:data usage:MTLResourceUsageRead stages:MTLRenderStageVertex | MTLRenderStageFragment];
                    if (heap) [draw useHeap:heap stages:MTLRenderStageVertex | MTLRenderStageFragment];
                    else for (unsigned i = 0; i < 2; ++i)
                        [draw useResource:images[i] usage:MTLResourceUsageRead stages:MTLRenderStageVertex | MTLRenderStageFragment];
                    [draw drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                    [draw endEncoding];
                    if (strcmp(argv[1], "--reference")) {
                        images[0] = nil; images[1] = nil;
                        filters[0] = nil; filters[1] = nil;
                        upload[0] = nil; upload[1] = nil; heap = nil;
                        [extraSamplers removeAllObjects];
                    }
                    if (mode) {
                        blit = [command blitCommandEncoder];
                        require(blit != nil, @"readback encoder unavailable");
                        [blit copyFromBuffer:destination sourceOffset:0 toBuffer:readback destinationOffset:0 size:length];
                        [blit endEncoding];
                    }
                    [command commit]; [command waitUntilCompleted];
                    require(command.status == MTLCommandBufferStatusCompleted, command.error.description ?: @"GPU copy failed");
                    require(!memcmp(seed.contents, seedBefore.bytes, length), @"source or source guard changed");
                    require(!memcmp(readback.contents, expected.contents, length), @"GPU encoded resource bytes differ from argument encoder or guard changed");
                    for (unsigned i = 0; i < 32; ++i) {
                        uint32_t want = i == 4 ? values[0] : i == 5 ? values[5] : 0xcdcdcdcdu;
                        if (i >= 8 && i < 16) {
                            unsigned thread = i - 8, image = (thread & 1u) ^ round;
                            unsigned sampler = 1u - (((thread >> 1u) & 1u) ^ round);
                            want = values[4 + thread] + (uint32_t)pixels[image][sampler * 4];
                        }
                        if (((uint32_t *)output.contents)[i] != want)
                            fprintf(stderr, "ARGUMENT_COPY_MISMATCH mode=%lu round=%u word=%u got=%u want=%u\n",
                                (unsigned long)mode, round, i, ((uint32_t *)output.contents)[i], want);
                        require(((uint32_t *)output.contents)[i] == want, @"GPU token consumer, constant pointer access or output guard mismatch");
                    }
                    require(!memcmp(data.contents, values, sizeof values), @"constant source data changed");
                    uint8_t colors[32];
                    [target getBytes:colors bytesPerRow:16 fromRegion:MTLRegionMake2D(0, 0, 4, 2) mipmapLevel:0];
                    for (unsigned i = 0; i < 8; ++i) {
                        unsigned image = (i & 1u) ^ round, filter = 1u - (((i >> 1u) & 1u) ^ round);
                        uint32_t value = values[4 + i] + (uint32_t)pixels[image][filter * 4];
                        require(colors[4*i] == (((i >> 1u) & 1u) * 255) && colors[4*i + 1] == ((i & 1u) * 255) &&
                            colors[4*i + 2] == (value & 255u) && colors[4*i + 3] == 255,
                            [NSString stringWithFormat:@"GPU resource render mismatch mode=%lu round=%u pixel=%u BGRA=%u,%u,%u,%u",
                                (unsigned long)mode, round, i, colors[4*i], colors[4*i+1], colors[4*i+2], colors[4*i+3]]);
                    }
                    printf("ARGUMENT_COPY_ROUND_PASS mode=%s round=%u bytes=%lu\n", mode ? "private" : "shared", round, (unsigned long)length);
                }
            }
        }
        puts("ARGUMENT_COPY_PASS rounds=2 modes=shared,private tokens=texture,sampler,const-pointer gpu_consumer=1 stages=compute,vertex,fragment useHeap=1 extra_samplers=128");
    }
    return 0;
}
