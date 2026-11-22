#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const NSUInteger guard = 256, width = 7;
static void require(BOOL ok, NSString *message) {
    if (!ok) { fprintf(stderr, "TEXTURE_BUFFER_FAIL %s\n", message.UTF8String); exit(1); }
}
static void finish(id<MTLCommandBuffer> cb) {
    [cb commit]; [cb waitUntilCompleted];
    require(cb.status == MTLCommandBufferStatusCompleted && !cb.error,
        cb.error.description ?: @"command did not complete");
}
static id<MTLBuffer> guarded(id<MTLDevice> device, NSUInteger payload) {
    id<MTLBuffer> result = [device newBufferWithLength:guard + payload + guard
        options:MTLResourceStorageModeShared];
    require(result && result.contents, @"shared backing unavailable");
    memset(result.contents, 0xa5, result.length); return result;
}
static void guards(id<MTLBuffer> b, NSUInteger payload) {
    const uint8_t *p = b.contents;
    for (NSUInteger i = 0; i < b.length; ++i)
        if (i < guard || i >= guard + payload)
            require(p[i] == 0xa5, [NSString stringWithFormat:@"guard changed at %lu", (unsigned long)i]);
}
static uint32_t bits(unsigned kind, int value) {
    if (kind != 0) return (uint32_t)(kind == 2 ? -value : value);
    float f = (float)value; uint32_t u; memcpy(&u, &f, sizeof u); return u;
}

int main(int argc, const char **argv) {
    @autoreleasepool {
        require(argc >= 4 && (!strcmp(argv[1], "--reference") || !strcmp(argv[1], "--bundle")),
            @"usage: test-texture-buffer --reference - OUTPUT|--bundle BUNDLE OUTPUT "
             "[--kind 0|1|2] [--dynamic] [--read-write] [--create-only|--compile-only] [--row BYTES] [--in-bounds] [--standalone]");
        BOOL reference = !strcmp(argv[1], "--reference"), dynamic = NO, createOnly = NO;
        BOOL compileOnly = NO, inBounds = NO, standalone = NO, readWrite = NO; unsigned kind = 0; NSUInteger row = 256;
        for (int i = 4; i < argc; ++i) {
            if (!strcmp(argv[i], "--dynamic")) dynamic = YES;
            else if (!strcmp(argv[i], "--create-only")) createOnly = YES;
            else if (!strcmp(argv[i], "--compile-only")) compileOnly = YES;
            else if (!strcmp(argv[i], "--in-bounds")) inBounds = YES;
            else if (!strcmp(argv[i], "--standalone")) standalone = YES;
            else if (!strcmp(argv[i], "--read-write")) readWrite = YES;
            else if (!strcmp(argv[i], "--kind") && i + 1 < argc) kind = (unsigned)atoi(argv[++i]);
            else if (!strcmp(argv[i], "--row") && i + 1 < argc) row = strtoull(argv[++i], NULL, 10);
            else require(NO, @"invalid texture-buffer option");
        }
        require(kind < 3 && !(compileOnly && createOnly), @"invalid kind or mode");
        NSError *error = nil; id<MTLDevice> device;
        if (reference) device = MTLCreateSystemDefaultDevice();
        else {
            NSBundle *bundle = [NSBundle bundleWithPath:@(argv[2])];
            require([bundle loadAndReturnError:&error], error.description ?: @"bundle unavailable");
            device = [[[bundle classNamed:@"GarnetDevice"] alloc] init];
        }
        require(device != nil, @"device unavailable");
        NSString *directory = @(argv[3]);
        require([[NSFileManager defaultManager] createDirectoryAtPath:directory
            withIntermediateDirectories:YES attributes:nil error:&error], @"output directory unavailable");
        printf("TEXTURE_BUFFER_CASE kind=%u dynamic=%d row=%lu mode=%s\n", kind, dynamic,
            (unsigned long)row, createOnly ? "create" : compileOnly ? "capture" : "execute"); fflush(stdout);
        const char *scalar = kind == 0 ? "float" : kind == 1 ? "uint" : "int";
        const char *access = readWrite ? "read_write" : "write";
        NSString *declarations = dynamic ? [NSString stringWithFormat:
            @"struct Args { array<texture_buffer<%s,access::read>,2> src [[id(0)]]; "
             "array<texture_buffer<%s,access::%s>,2> dst [[id(2)]]; };\n", scalar, scalar, access] : @"";
        NSString *parameters = dynamic ? @"constant Args& a [[buffer(3)]], " :
            [NSString stringWithFormat:@"texture_buffer<%s,access::read> src [[texture(0)]], "
             "texture_buffer<%s,access::%s> dst [[texture(1)]], ", scalar, scalar, access];
        NSString *src = dynamic ? @"a.src[s]" : @"src", *dst = dynamic ? @"a.dst[s]" : @"dst";
        NSString *oob = inBounds ? @"" : [NSString stringWithFormat:
            @"o[2]=as_type<uint4>(%@.read(7u)); o[3]=as_type<uint4>(%@.read(0xffffffffu)); "
             "%@.write(%s4(91),7u); %@.write(%s4(92),0xffffffffu);", src, src, dst, scalar, dst, scalar];
        NSString *readBack = readWrite ? [NSString stringWithFormat:
            @"%@.fence(); o[2]=as_type<uint4>(%@.read(3u));", dst, dst] : @"";
        NSString *source = [NSString stringWithFormat:
            @"#include <metal_stdlib>\nusing namespace metal;\n%@"
             "kernel void texel_ops(%@constant uint& s [[buffer(0)]], device uint4* o [[buffer(1)]]) {"
             "o[0]=as_type<uint4>(%@.read(1u)); o[1]=uint4(%@.get_width(),0,0,0); %@ "
             "%@.write(%s4(%s73),3u); %@ }\n", declarations, parameters, src, src, oob,
            dst, scalar, kind == 2 ? "-" : "", readBack];
        require([source writeToFile:[directory stringByAppendingPathComponent:@"texel.metal"]
            atomically:YES encoding:NSUTF8StringEncoding error:&error], @"source write failed");
        id<MTLFunction> function = nil;
        if (!createOnly) {
            MTLCompileOptions *options = [MTLCompileOptions new]; options.languageVersion = MTLLanguageVersion2_1;
            id<MTLLibrary> library = [device newLibraryWithSource:source options:options error:&error];
            require(library != nil, error.description ?: @"MSL compile failed");
            function = [library newFunctionWithName:@"texel_ops"];
            require(function != nil, @"function unavailable");
            SEL selector = NSSelectorFromString(@"bitcodeData");
            require([function respondsToSelector:selector], @"AIR capture unavailable");
            NSData *air = ((id (*)(id, SEL))objc_msgSend)(function, selector);
            require(air.length && [air writeToFile:[directory stringByAppendingPathComponent:@"texel.air"]
                atomically:YES], @"AIR write failed");
        }
        if (compileOnly) { puts("TEXTURE_BUFFER_CAPTURE msl=2.1 gpu_execution=0"); return 0; }
        MTLPixelFormat format = kind == 0 ? MTLPixelFormatR32Float :
            kind == 1 ? MTLPixelFormatR32Uint : MTLPixelFormatR32Sint;
        MTLTextureDescriptor *descriptor = [MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:
            format width:width resourceOptions:MTLResourceStorageModeShared usage:
            MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite];
        if (standalone) {
            require(createOnly, @"standalone mode currently checks creation and CPU access only");
            id<MTLTexture> image = [device newTextureWithDescriptor:descriptor];
            require(image && image.textureType == MTLTextureTypeTextureBuffer && image.width == width &&
                image.buffer == nil, @"standalone texture buffer unavailable or wrong properties");
            uint32_t value = bits(kind, 37), actual = 0;
            [image replaceRegion:MTLRegionMake1D(3,1) mipmapLevel:0 withBytes:&value bytesPerRow:0];
            [image getBytes:&actual bytesPerRow:0 fromRegion:MTLRegionMake1D(3,1) mipmapLevel:0];
            require(actual == value, @"standalone texture-buffer CPU access differs");
            puts("TEXTURE_BUFFER_CREATE_PASS standalone=1 cpu_access=1 gpu_execution=0"); return 0;
        }
        id<MTLBuffer> backing[4]; id<MTLTexture> texture[4];
        for (unsigned n = 0; n < 4; ++n) {
            backing[n] = guarded(device, width * 4);
            uint32_t *p = (uint32_t *)((uint8_t *)backing[n].contents + guard);
            for (unsigned x = 0; x < width; ++x) p[x] = bits(kind, (int)(n * 100 + x + 1));
            texture[n] = [backing[n] newTextureWithDescriptor:descriptor offset:guard bytesPerRow:row];
            require(texture[n] != nil, [NSString stringWithFormat:@"texture buffer %u unavailable", n]);
            require(texture[n].textureType == MTLTextureTypeTextureBuffer && texture[n].width == width &&
                texture[n].height == 1 && texture[n].depth == 1 && texture[n].mipmapLevelCount == 1 &&
                texture[n].buffer == backing[n] && texture[n].bufferOffset == guard, @"texture properties differ");
        }
        if (createOnly) { puts("TEXTURE_BUFFER_CREATE_PASS gpu_execution=0"); return 0; }
        id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function error:&error];
        require(pipeline != nil, error.description ?: @"pipeline unavailable");
        id<MTLCommandQueue> queue = [device newCommandQueue]; require(queue != nil, @"queue unavailable");
        id<MTLArgumentEncoder> ae = dynamic ? [function newArgumentEncoderWithBufferIndex:3] : nil;
        require(!dynamic || ae != nil, @"argument encoder unavailable");
        id<MTLBuffer> args = dynamic ? guarded(device, ae.encodedLength) : nil;
        if (dynamic) {
            [ae setArgumentBuffer:args offset:guard];
            for (unsigned n = 0; n < 4; ++n) [ae setTexture:texture[n] atIndex:n];
        }
        NSData *originalArgs = dynamic ? [NSData dataWithBytes:args.contents length:args.length] : nil;
        for (uint32_t selected = 0; selected < 2; ++selected) {
            id<MTLBuffer> output = guarded(device, 16 * 4), selection = guarded(device, 4);
            memcpy((uint8_t *)selection.contents + guard, &selected, 4);
            id<MTLCommandBuffer> cb = [queue commandBuffer]; id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
            [enc setComputePipelineState:pipeline]; [enc setBuffer:selection offset:guard atIndex:0];
            [enc setBuffer:output offset:guard atIndex:1];
            if (dynamic) {
                [enc setBuffer:args offset:guard atIndex:3];
                for (unsigned n = 0; n < 4; ++n)
                    [enc useResource:texture[n] usage:n < 2 ? MTLResourceUsageRead : MTLResourceUsageWrite];
            } else {
                [enc setTexture:texture[selected] atIndex:0]; [enc setTexture:texture[2 + selected] atIndex:1];
            }
            [enc dispatchThreadgroups:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];
            [enc endEncoding]; finish(cb);
            const uint32_t *p = (const uint32_t *)((const uint8_t *)output.contents + guard);
            uint32_t alpha = kind == 0 ? bits(0,1) : 1;
            uint32_t expected[] = {bits(kind, (int)(selected * 100 + 2)),0,0,alpha,
                (uint32_t)width,0,0,0, 0,0,0,alpha, 0,0,0,alpha};
            if (readWrite) expected[8] = bits(kind,73);
            for (unsigned i = 0; i < (inBounds ? readWrite ? 12u : 8u : 16u); ++i)
                require(p[i] == expected[i], [NSString stringWithFormat:
                    @"selection=%u word=%u expected=%u actual=%u", selected, i, expected[i], p[i]]);
            guards(output, 64); guards(selection, 4);
            for (unsigned n = 0; n < 4; ++n) {
                guards(backing[n], width * 4);
                const uint32_t *data = (const uint32_t *)((const uint8_t *)backing[n].contents + guard);
                for (unsigned x = 0; x < width; ++x) {
                    BOOL written = n >= 2 && n - 2 <= selected && x == 3;
                    require(data[x] == bits(kind, written ? 73 : (int)(n * 100 + x + 1)),
                        [NSString stringWithFormat:@"backing n=%u x=%u changed incorrectly", n, x]);
                }
            }
            if (dynamic) require(!memcmp(originalArgs.bytes, args.contents, args.length), @"argument bytes changed");
        }
        printf("TEXTURE_BUFFER_PASS backend=%s kind=%u dynamic=%d read_write=%d dispatches=2 msl=2.1 oob=%d gpu_execution=1\n",
            reference ? "reference" : "bundle", kind, dynamic, readWrite, !inBounds);
    }
    return 0;
}
