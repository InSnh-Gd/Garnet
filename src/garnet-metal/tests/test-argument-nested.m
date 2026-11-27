#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

static void require(BOOL ok, NSString *message) {
    if (!ok) { fprintf(stderr, "ARGUMENT_NESTED_FAIL %s\n", message.UTF8String); exit(1); }
}

static void checkNestedBinding(NSArray<id<MTLBinding>> *bindings, NSString *stage, BOOL pointer,
                               id<MTLArgumentEncoder> child) {
    id<MTLBinding> binding = nil;
    for (id<MTLBinding> candidate in bindings)
        if (candidate.type == MTLBindingTypeBuffer && candidate.index == 3) { binding = candidate; break; }
    require(binding != nil && binding.used, [NSString stringWithFormat:@"%@ buffer(3) reflection missing", stage]);
    if (!pointer) return;
    MTLStructType *structure = ((id<MTLBufferBinding>)binding).bufferStructType;
    MTLStructMember *member = [structure memberByName:@"child"];
    MTLPointerType *pointerType = member.pointerType;
    require(structure != nil && member != nil && pointerType != nil,
        [NSString stringWithFormat:@"%@ buffer(3).child pointer reflection missing", stage]);
    require(pointerType.elementIsArgumentBuffer && pointerType.dataSize == child.encodedLength &&
        pointerType.alignment == child.alignment,
        [NSString stringWithFormat:@"%@ buffer(3).child pointer layout mismatch: argument=%d data=%lu/%lu align=%lu/%lu",
            stage, pointerType.elementIsArgumentBuffer, (unsigned long)pointerType.dataSize,
            (unsigned long)child.encodedLength, (unsigned long)pointerType.alignment,
            (unsigned long)child.alignment]);
}

int main(int argc, const char **argv) {
    if (argc != 4 && !(argc == 5 && !strcmp(argv[4], "--capture-only"))) return 2;
    alarm(90); setvbuf(stdout, NULL, _IONBF, 0);
    @autoreleasepool {
        NSError *error = nil;
        NSString *kind = @(argv[3]), *directory = @(argv[2]);
        BOOL gpuCopy = [kind hasSuffix:@"-copy"];
        BOOL queries = [kind containsString:@"-query"], unsignedImage = [kind containsString:@"-uint"];
        BOOL query3D = [kind containsString:@"-3d"], queryArray = [kind containsString:@"-array"];
        BOOL queryView = [kind containsString:@"-view"], queryMS = [kind containsString:@"-ms"];
        BOOL readWrite = [kind containsString:@"-rw"], readOnly = [kind containsString:@"-read"];
        BOOL texelRead = (readWrite || readOnly) && !queries;
        BOOL rendering = [kind containsString:@"-render"];
        BOOL encodedNulls = [kind containsString:@"-null"];
        BOOL nulls = encodedNulls && !queries, nullOnly = [kind containsString:@"-null-only"];
        NSString *shape = [kind componentsSeparatedByString:@"-"][0];
        BOOL records = [shape isEqual:@"records"], pointer = [shape isEqual:@"pointer"];
        require(records || pointer || [shape isEqual:@"inline"], @"unknown nesting case");
        require(!texelRead || records || pointer, @"texel modes need two distinct records");
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
        NSString *field = records ? @"array<Child,2> child [[id(3)]];" : pointer ?
            @"constant Child *child [[id(3)]];" : @"Child child [[id(3)]];";
        NSString *access = records || pointer ? @"a.child[(i >> 2u) & 1u]" : @"a.child";
        NSString *imageType = readWrite ? @"texture2d<float, access::read_write>" : readOnly ? @"texture2d<float, access::read>" : @"texture2d<float>";
        if (unsignedImage) imageType = @"texture2d<uint, access::read>";
        if (query3D) imageType = @"texture3d<float, access::read>";
        if (queryArray) imageType = @"texture2d_array<float, access::read>";
        if (queryMS) imageType = @"texture2d_ms<float, access::read>";
        NSString *body = texelRead ? [NSString stringWithFormat:
            @"uint2 xy = uint2((i >> 1u) & 1u, 0); auto image = %@.images[i & 1u];"
             " float value = image.read(xy).x; out[i] = uint(value) + image.get_width() * 1000u + image.get_height() * 10000u; %@",
            access, readWrite ? @"image.write(float4(value + 200.0, 0, 0, 1), xy);" : @""] :
            [NSString stringWithFormat:@"out[i] = uint(%@.images[i & 1u].sample(%@.filter, float2(1.25, 0.5)).x);", access, access];
        if (nulls) body = [NSString stringWithFormat:
            @"auto image = %@.images[i & 1u]; if (is_null_texture(image)) out[i] = 701u + i; else { %@ }",
            access, nullOnly ? @"out[i] = 17u + i;" : body];
        if (queries) body = [NSString stringWithFormat:
            @"auto image = %@.images[i & 1u]; %@"
             " out[i] = %@ * 1000u + %@ * 10000u + %@ * 100000u %@;", access,
             queryMS ? @"" : @"uint mip = (i >> 1u) & 1u;",
             queryMS ? @"image.get_width()" : @"image.get_width(mip)",
             queryMS ? @"image.get_height()" : @"image.get_height(mip)",
             queryMS ? @"image.get_num_samples()" : @"image.get_num_mip_levels()",
             query3D ? @"+ image.get_depth(mip) * 1000000u" : queryArray ? @"+ image.get_array_size() * 1000000u" : @""];
        NSString *source = [NSString stringWithFormat:
            @"#include <metal_stdlib>\nusing namespace metal;\n"
             "struct Child { array<%@,2> images [[id(1)]]; sampler filter [[id(4)]]; };\n"
             "struct Args { %@ };\n"
             "kernel void nested_resources(constant Args &a [[buffer(3)]], device uint *out [[buffer(0)]],"
             " uint i [[thread_position_in_grid]]) { %@ }\n",
             imageType, field, body];
        if (rendering && !queries) source = [source stringByAppendingFormat:
            @"vertex float4 nested_vertex(uint vid [[vertex_id]], constant Args &a [[buffer(3)]]) {"
             " uint i = vid + 3u, group = (i >> 2u) & 1u;"
             " uint value = uint(%@.images[i & 1u].sample(%@.filter, float2(1.25, 0.5), level(0.0)).x);"
             " float2 p = float2(float((vid << 1u) & 2u), float(vid & 2u)) * 2.0 - 1.0;"
             " if (value != (group ? 129u : 11u) + 20u * (i & 1u)) p += 8.0;"
             " return float4(p, 0.0, 1.0); }"
             "fragment float4 nested_fragment(float4 p [[position]], constant Args &a [[buffer(3)]]) {"
             " uint i = uint(p.x) + 4u * uint(p.y);"
             " float value = %@.images[i & 1u].sample(%@.filter, float2(1.25, 0.5)).x;"
             " return float4(value / 255.0, float(i & 1u), float((i >> 2u) & 1u), 1.0); }\n",
             access, access, access, access];
        if (rendering && queries) source = [source stringByAppendingFormat:
            @"vertex float4 nested_vertex(uint vid [[vertex_id]], constant Args &a [[buffer(3)]]) {"
             " uint i = vid + 3u, group = (i >> 2u) & 1u, mip = (i >> 1u) & 1u;"
             " auto image = %@.images[i & 1u]; uint value = image.get_width(mip) + image.get_height(mip);"
             " float2 p = float2(float((vid << 1u) & 2u), float(vid & 2u)) * 2.0 - 1.0;"
             " if (value != ((8u + 4u * group + 2u * (i & 1u)) >> mip) + ((4u + 2u * group) >> mip)) p += 8.0;"
             " return float4(p, 0.0, 1.0); }"
             "fragment float4 nested_fragment(float4 p [[position]], constant Args &a [[buffer(3)]]) {"
             " uint i = uint(p.x) + 4u * uint(p.y), mip = (i >> 1u) & 1u; auto image = %@.images[i & 1u];"
             " return float4(float(image.get_width(mip)) / 255.0, float(image.get_height(mip)) / 255.0,"
             " float(image.get_num_mip_levels()) / 255.0, 1.0); }\n", access, access];
        require([source writeToFile:[directory stringByAppendingPathComponent:@"nested.metal"] atomically:YES
            encoding:NSUTF8StringEncoding error:&error], @"source write failed");
        MTLCompileOptions *options = [MTLCompileOptions new]; options.languageVersion = MTLLanguageVersion2_1;
        id<MTLLibrary> library = [device newLibraryWithSource:source options:options error:&error];
        require(library != nil, error.description ?: @"compile failed");
        id<MTLFunction> function = [library newFunctionWithName:@"nested_resources"];
        require(function != nil, @"function unavailable");
        NSData *air = [function respondsToSelector:NSSelectorFromString(@"bitcodeData")] ?
            ((id (*)(id, SEL))objc_msgSend)(function, NSSelectorFromString(@"bitcodeData")) : nil;
        require(air.length && [air writeToFile:[directory stringByAppendingPathComponent:@"nested.air"] atomically:YES], @"AIR write");
        if (rendering) for (NSString *name in @[@"nested_vertex", @"nested_fragment"]) {
            id<MTLFunction> stage = [library newFunctionWithName:name];
            NSData *code = [stage respondsToSelector:NSSelectorFromString(@"bitcodeData")] ?
                ((id (*)(id, SEL))objc_msgSend)(stage, NSSelectorFromString(@"bitcodeData")) : nil;
            require(code.length && [code writeToFile:[directory stringByAppendingPathComponent:[name stringByAppendingString:@".air"]] atomically:YES], @"render AIR write");
        }
        id<MTLArgumentEncoder> root = [function newArgumentEncoderWithBufferIndex:3];
        require(root && root.encodedLength && root.encodedLength < 4096, @"root layout unavailable");
        id<MTLArgumentEncoder> child = pointer ? [root newArgumentEncoderForBufferAtIndex:3] : nil;
        require(!pointer || (child && child.encodedLength == 24), @"child layout mismatch");
        printf("ARGUMENT_NESTED_LAYOUT kind=%s root=%lu child=%lu\n", argv[3], (unsigned long)root.encodedLength,
            (unsigned long)child.encodedLength);
        if (argc == 5) { printf("ARGUMENT_NESTED_CAPTURE kind=%s gpu_execution=0\n", argv[3]); return 0; }
        MTLPipelineOption reflectionOptions = MTLPipelineOptionBindingInfo | MTLPipelineOptionBufferTypeInfo;
        MTLComputePipelineReflection *computeReflection = nil;
        id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function options:reflectionOptions
            reflection:&computeReflection error:&error];
        require(pipeline != nil && computeReflection != nil, error.description ?: @"pipeline reflection failed");
        checkNestedBinding(computeReflection.bindings, @"compute", pointer, child);
        id<MTLRenderPipelineState> renderPipeline = nil;
        id<MTLTexture> target = nil;
        if (rendering) {
            MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
            descriptor.vertexFunction = [library newFunctionWithName:@"nested_vertex"];
            descriptor.fragmentFunction = [library newFunctionWithName:@"nested_fragment"];
            descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
            MTLRenderPipelineReflection *renderReflection = nil;
            renderPipeline = [device newRenderPipelineStateWithDescriptor:descriptor options:reflectionOptions
                reflection:&renderReflection error:&error];
            require(renderPipeline != nil && renderReflection != nil,
                error.description ?: @"nested render pipeline reflection failed");
            checkNestedBinding(renderReflection.vertexBindings, @"vertex", pointer, child);
            checkNestedBinding(renderReflection.fragmentBindings, @"fragment", pointer, child);
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:4 height:2 mipmapped:NO];
            td.storageMode = MTLStorageModeShared; td.usage = MTLTextureUsageRenderTarget;
            target = [device newTextureWithDescriptor:td]; require(target != nil, @"render target allocation");
        }
        printf("ARGUMENT_NESTED_REFLECTION_PASS kind=%s compute=1 render=%d pointer=%d\n",
            argv[3], rendering, pointer);
        id<MTLBuffer> table = [device newBufferWithLength:root.encodedLength + 512 options:MTLResourceStorageModeShared];
        id<MTLBuffer> children = pointer ? [device newBufferWithLength:child.encodedLength * 2 + 512
            options:MTLResourceStorageModeShared] : nil;
        require(table && table.contents && (!pointer || (children && children.contents)), @"argument allocation");
        id<MTLBuffer> boundTable = gpuCopy ? [device newBufferWithLength:table.length options:MTLResourceStorageModePrivate] : table;
        id<MTLBuffer> boundChildren = gpuCopy && pointer ? [device newBufferWithLength:children.length options:MTLResourceStorageModePrivate] : children;
        id<MTLBuffer> tableReadback = gpuCopy ? [device newBufferWithLength:table.length options:MTLResourceStorageModeShared] : nil;
        id<MTLBuffer> childReadback = gpuCopy && pointer ? [device newBufferWithLength:children.length options:MTLResourceStorageModeShared] : nil;
        require(boundTable && (!pointer || boundChildren) && (!gpuCopy || (tableReadback && (!pointer || childReadback))), @"GPU argument allocation");
        memset(table.contents, 0xa7, table.length);
        if (pointer) memset(children.contents, 0xb6, children.length);
        [root setArgumentBuffer:table offset:256];
        if (pointer) [root setBuffer:boundChildren offset:256 atIndex:3];
        id<MTLTexture> textures[4]; id<MTLTexture> readbackTextures[4]; id<MTLSamplerState> samplers[2];
        unsigned groups = pointer || records ? 2 : 1;
        for (unsigned group = 0; group < groups; ++group) {
            id<MTLArgumentEncoder> encoder = pointer ? child : root;
            if (pointer) [child setArgumentBuffer:children offset:256 + group * child.encodedLength];
            for (unsigned image = 0; image < 2; ++image) {
                MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:unsignedImage ? MTLPixelFormatR32Uint : MTLPixelFormatR32Float
                    width:queries ? 8 + 4 * group + 2 * image : texelRead ? 2 + 2 * group + image : 2
                    height:queries ? 4 + 2 * group : texelRead ? 1 + group : 1 mipmapped:queries];
                if (queries) td.mipmapLevelCount = 3;
                if (query3D) { td.textureType = MTLTextureType3D; td.depth = 4 + 2 * image; }
                if (queryArray) { td.textureType = MTLTextureType2DArray; td.arrayLength = 2 + group; }
                if (queryView) { td.width *= 2; td.height *= 2; td.mipmapLevelCount = 4; }
                if (queryMS) { td.textureType = MTLTextureType2DMultisample; td.sampleCount = 4; td.mipmapLevelCount = 1; }
                td.storageMode = MTLStorageModeShared; td.usage = MTLTextureUsageShaderRead | (readWrite ? MTLTextureUsageShaderWrite : 0);
                if (queryMS) td.storageMode = MTLStorageModePrivate;
                textures[2*group + image] = [device newTextureWithDescriptor:td];
                require(textures[2*group + image] != nil, @"texture allocation");
                if (queryView) {
                    id<MTLTexture> view = [textures[2*group + image] newTextureViewWithPixelFormat:td.pixelFormat
                        textureType:MTLTextureType2D levels:NSMakeRange(1, 3) slices:NSMakeRange(0, 1)];
                    require(view != nil, @"query texture view allocation"); textures[2*group + image] = view;
                }
                if (texelRead) {
                    readbackTextures[2*group + image] = [device newTextureWithDescriptor:td];
                    require(readbackTextures[2*group + image] != nil, @"texture readback allocation");
                }
                if (!queries) {
                    float pixels[10];
                    for (unsigned j = 0; j < td.width * td.height; ++j) pixels[j] = j == 0 ? 11 + 100 * group + 20 * image : j == 1 ? 29 + 100 * group + 20 * image : 73 + j;
                    [textures[2*group + image] replaceRegion:MTLRegionMake2D(0, 0, td.width, td.height) mipmapLevel:0
                        withBytes:pixels bytesPerRow:td.width * sizeof(float)];
                }
                [encoder setTexture:encodedNulls && image == group ? nil : textures[2*group + image]
                    atIndex:(pointer ? 1 : 4 + group * 5) + image];
            }
            MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new]; sd.supportArgumentBuffers = YES;
            sd.sAddressMode = group ? MTLSamplerAddressModeClampToEdge : MTLSamplerAddressModeRepeat;
            samplers[group] = [device newSamplerStateWithDescriptor:sd];
            require(samplers[group] != nil, @"sampler allocation");
            [encoder setSamplerState:samplers[group] atIndex:pointer ? 4 : 7 + group * 5];
        }
        NSData *tableBefore = [NSData dataWithBytes:table.contents length:table.length];
        NSData *childBefore = pointer ? [NSData dataWithBytes:children.contents length:children.length] : nil;
        id<MTLBuffer> output = [device newBufferWithLength:128 options:MTLResourceStorageModeShared];
        require(output && output.contents, @"output allocation"); memset(output.contents, 0xcd, output.length);
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLCommandBuffer> command = [queue commandBuffer];
        if (gpuCopy) {
            id<MTLBlitCommandEncoder> copy = [command blitCommandEncoder]; require(copy != nil, @"argument copy encoder");
            [copy copyFromBuffer:table sourceOffset:0 toBuffer:boundTable destinationOffset:0 size:table.length];
            if (pointer) [copy copyFromBuffer:children sourceOffset:0 toBuffer:boundChildren destinationOffset:0 size:children.length];
            [copy endEncoding];
        }
        id<MTLComputeCommandEncoder> encode = [command computeCommandEncoder]; require(encode != nil, @"encoder unavailable");
        [encode setComputePipelineState:pipeline]; [encode setBuffer:boundTable offset:256 atIndex:3];
        [encode setBuffer:output offset:16 atIndex:0];
        for (unsigned i = 0; i < groups * 2; ++i) [encode useResource:textures[i] usage:MTLResourceUsageRead | (readWrite ? MTLResourceUsageWrite : 0)];
        if (boundChildren) [encode useResource:boundChildren usage:MTLResourceUsageRead];
        [encode dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(8, 1, 1)];
        [encode endEncoding];
        if (rendering) {
            MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = target; pass.colorAttachments[0].loadAction = MTLLoadActionClear;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore; pass.colorAttachments[0].clearColor = MTLClearColorMake(1, 0, 1, 1);
            id<MTLRenderCommandEncoder> draw = [command renderCommandEncoderWithDescriptor:pass];
            require(draw != nil, @"render encoder unavailable"); [draw setRenderPipelineState:renderPipeline];
            [draw setVertexBuffer:boundTable offset:256 atIndex:3]; [draw setFragmentBuffer:boundTable offset:256 atIndex:3];
            for (unsigned i = 0; i < groups * 2; ++i)
                [draw useResource:textures[i] usage:MTLResourceUsageRead stages:MTLRenderStageVertex | MTLRenderStageFragment];
            if (boundChildren) [draw useResource:boundChildren usage:MTLResourceUsageRead stages:MTLRenderStageVertex | MTLRenderStageFragment];
            [draw drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [draw endEncoding];
        }
        if (texelRead) {
            id<MTLBlitCommandEncoder> copy = [command blitCommandEncoder]; require(copy != nil, @"texture readback encoder");
            for (unsigned i = 0; i < groups * 2; ++i)
                [copy copyFromTexture:textures[i] sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                    sourceSize:MTLSizeMake(textures[i].width, textures[i].height, 1) toTexture:readbackTextures[i]
                    destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
            [copy endEncoding];
        }
        if (gpuCopy) {
            id<MTLBlitCommandEncoder> copy = [command blitCommandEncoder]; require(copy != nil, @"argument readback encoder");
            [copy copyFromBuffer:boundTable sourceOffset:0 toBuffer:tableReadback destinationOffset:0 size:table.length];
            if (pointer) [copy copyFromBuffer:boundChildren sourceOffset:0 toBuffer:childReadback destinationOffset:0 size:children.length];
            [copy endEncoding];
        }
        if (strcmp(argv[1], "--reference")) {
            for (unsigned i = 0; i < groups * 2; ++i) textures[i] = nil;
            for (unsigned i = 0; i < groups; ++i) samplers[i] = nil;
        }
        [command commit]; [command waitUntilCompleted];
        require(command.status == MTLCommandBufferStatusCompleted, command.error.description ?: @"GPU execution failed");
        for (unsigned word = 0; word < 32; ++word) {
            uint32_t expected = 0xcdcdcdcdu;
            if (word >= 4 && word < 12) {
                unsigned i = word - 4, group = groups == 2 ? ((i >> 2u) & 1u) : 0;
                expected = (texelRead ? ((i & 2u) ? 29 : 11) : group ? 29 : 11) + 100 * group + 20 * (i & 1u);
                if (texelRead) expected += (2 + 2 * group + (i & 1u)) * 1000 + (1 + group) * 10000;
                if (nulls) expected = (i & 1u) == group ? 701u + i : nullOnly ? 17u + i : expected;
                if (queries) {
                    unsigned mip = queryMS ? 0 : (i >> 1u) & 1u;
                    expected = encodedNulls && (i & 1u) == group ? 0 :
                        ((8 + 4 * group + 2 * (i & 1u)) >> mip) * 1000 + ((4 + 2 * group) >> mip) * 10000 +
                        (queryMS ? 400000 : 300000) + (query3D ? ((4 + 2 * (i & 1u)) >> mip) * 1000000 :
                            queryArray ? (2 + group) * 1000000 : 0);
                }
            }
            require(((uint32_t *)output.contents)[word] == expected,
                [NSString stringWithFormat:@"kind=%@ word=%u actual=%u expected=%u", kind, word,
                    ((uint32_t *)output.contents)[word], expected]);
        }
        if (texelRead) for (unsigned group = 0; group < groups; ++group) for (unsigned image = 0; image < 2; ++image) {
            id<MTLTexture> texture = readbackTextures[2*group + image]; float pixels[10];
            [texture getBytes:pixels bytesPerRow:texture.width * sizeof(float) fromRegion:MTLRegionMake2D(0, 0, texture.width, texture.height) mipmapLevel:0];
            for (unsigned j = 0; j < texture.width * texture.height; ++j) {
                float expected = j == 0 ? 11 + 100 * group + 20 * image : j == 1 ? 29 + 100 * group + 20 * image : 73 + j;
                if (readWrite && j < 2) expected += 200;
                require(pixels[j] == expected, @"texture read/write or untouched texel mismatch");
            }
        }
        if (rendering) {
            uint8_t colors[32]; [target getBytes:colors bytesPerRow:16 fromRegion:MTLRegionMake2D(0, 0, 4, 2) mipmapLevel:0];
            for (unsigned i = 0; i < 8; ++i) {
                unsigned group = (i >> 2u) & 1u, image = i & 1u;
                unsigned mip = (i >> 1u) & 1u;
                require(colors[4*i] == (queries ? 3 : group * 255) &&
                    colors[4*i+1] == (queries ? (4 + 2 * group) >> mip : image * 255) &&
                    colors[4*i+2] == (queries ? (8 + 4 * group + 2 * image) >> mip : (group ? 129 : 11) + 20 * image) && colors[4*i+3] == 255,
                    [NSString stringWithFormat:@"nested render pixel=%u BGRA=%u,%u,%u,%u", i, colors[4*i], colors[4*i+1], colors[4*i+2], colors[4*i+3]]);
            }
        }
        require(!memcmp(table.contents, tableBefore.bytes, table.length) &&
            (!pointer || !memcmp(children.contents, childBefore.bytes, children.length)), @"argument guard/data changed");
        if (gpuCopy) require(!memcmp(tableReadback.contents, tableBefore.bytes, table.length) &&
            (!pointer || !memcmp(childReadback.contents, childBefore.bytes, children.length)), @"GPU argument guard/data changed");
        printf("ARGUMENT_NESTED_PASS kind=%s outputs=8 guards=exact gpu_copy=%d render_pixels=%u\n", argv[3], gpuCopy, rendering ? 8 : 0);
    }
    return 0;
}
