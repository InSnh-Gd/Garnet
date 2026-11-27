#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks, oracleCases;
static const NSUInteger kOffset = 256;
static const uint8_t kCanary = 0xa5;

static int require(int ok, const char *message) {
    ++checks;
    if (!ok) { fprintf(stderr, "SAMPLER_PATTERN_FAIL %s\n", message); exit(1); }
    return 1;
}

#include "test-sampler-pattern-oracle.h"

static void completed(id<MTLCommandBuffer> command) {
    require(command != nil, "command buffer available");
    [command commit];
    [command waitUntilCompleted];
    require(command.status == MTLCommandBufferStatusCompleted && !command.error,
        command.error.description.UTF8String ?: "GPU completion");
}

static id<MTLBuffer> guarded(id<MTLDevice> device, NSUInteger bytes) {
    id<MTLBuffer> result = [device newBufferWithLength:kOffset + bytes + kOffset
        options:MTLResourceStorageModeShared];
    require(result && result.contents, "guarded buffer allocation");
    memset(result.contents, kCanary, result.length);
    return result;
}

static void guards(id<MTLBuffer> buffer, NSUInteger bytes) {
    const uint8_t *data = buffer.contents;
    for (NSUInteger i = 0; i < buffer.length; ++i)
        if (i < kOffset || i >= kOffset + bytes)
            require(data[i] == kCanary, "prefix/tail guard unchanged");
}

static MTLSamplerDescriptor *descriptor(const OracleCase *test) {
    MTLSamplerDescriptor *d = [MTLSamplerDescriptor new];
    d.normalizedCoordinates = !test->pixelCoordinates;
    d.minFilter = test->filter == OracleFilterLinear || test->filter == OracleFilterMinLinear;
    d.magFilter = test->filter == OracleFilterLinear || test->filter == OracleFilterMagLinear;
    d.mipFilter = test->pixelCoordinates || test->mipFilter == OracleMipNone ?
        MTLSamplerMipFilterNotMipmapped : test->mipFilter == OracleMipLinear ?
        MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
    switch (test->address) {
    case OracleAddressClamp: d.sAddressMode = MTLSamplerAddressModeClampToEdge; break;
    case OracleAddressRepeat: d.sAddressMode = MTLSamplerAddressModeRepeat; break;
    case OracleAddressMirroredRepeat: d.sAddressMode = MTLSamplerAddressModeMirrorRepeat; break;
    case OracleAddressMirrorClamp: d.sAddressMode = MTLSamplerAddressModeMirrorClampToEdge; break;
    case OracleAddressZero: d.sAddressMode = MTLSamplerAddressModeClampToZero; break;
    default: d.sAddressMode = MTLSamplerAddressModeClampToBorderColor; break;
    }
    d.tAddressMode = d.rAddressMode = d.sAddressMode;
    d.borderColor = test->address == OracleAddressBorderBlack ? MTLSamplerBorderColorOpaqueBlack :
        test->address == OracleAddressBorderWhite ? MTLSamplerBorderColorOpaqueWhite :
        MTLSamplerBorderColorTransparentBlack;
    d.lodMinClamp = test->lodMin;
    d.lodMaxClamp = test->lodMax;
    d.supportArgumentBuffers = YES;
    return d;
}

static NSString *staticState(const OracleCase *test) {
    const char *address;
    switch (test->address) {
    case OracleAddressClamp: address = "clamp_to_edge"; break;
    case OracleAddressRepeat: address = "repeat"; break;
    case OracleAddressMirroredRepeat: address = "mirrored_repeat"; break;
    case OracleAddressZero: address = "clamp_to_zero"; break;
    case OracleAddressMirrorClamp: return nil; // API-only state; MSL Table 7 has no equivalent.
    default: address = "clamp_to_border"; break;
    }
    return [NSString stringWithFormat:
        @"coord::%s, address::%s, min_filter::%s, mag_filter::%s, mip_filter::%s, "
         "border_color::%s, lod_clamp(%.9ef, %.9ef)",
        test->pixelCoordinates ? "pixel" : "normalized", address,
        test->filter == OracleFilterLinear || test->filter == OracleFilterMinLinear ? "linear" : "nearest",
        test->filter == OracleFilterLinear || test->filter == OracleFilterMagLinear ? "linear" : "nearest",
        test->pixelCoordinates || test->mipFilter == OracleMipNone ? "none" :
            test->mipFilter == OracleMipLinear ? "linear" : "nearest",
        test->address == OracleAddressBorderBlack ? "opaque_black" :
            test->address == OracleAddressBorderWhite ? "opaque_white" : "transparent_black",
        (double)test->lodMin, (double)test->lodMax];
}

static id<MTLComputePipelineState> pipeline(id<MTLDevice> device, NSString *source,
    NSString *entry, NSString *directory, NSString *label) {
    NSError *error = nil;
    require([source writeToFile:[directory stringByAppendingPathComponent:
        [label stringByAppendingString:@".metal"]] atomically:YES encoding:NSUTF8StringEncoding
        error:&error], "write authored source");
    MTLCompileOptions *options = [MTLCompileOptions new];
    options.languageVersion = MTLLanguageVersion2_1;
    id<MTLLibrary> library = [device newLibraryWithSource:source options:options error:&error];
    require(library != nil, error.description.UTF8String ?: "compile source");
    id<MTLFunction> function = [library newFunctionWithName:entry];
    require(function != nil, "authored function available");
    SEL selector = NSSelectorFromString(@"bitcodeData");
    if ([function respondsToSelector:selector]) {
        NSData *air = ((id (*)(id, SEL))objc_msgSend)(function, selector);
        require(air.length && [air writeToFile:[directory stringByAppendingPathComponent:
            [label stringByAppendingString:@".air"]] atomically:YES], "capture authored AIR");
    }
    id<MTLComputePipelineState> result = [device newComputePipelineStateWithFunction:function error:&error];
    require(result != nil, error.description.UTF8String ?: "create pipeline");
    return result;
}

static id<MTLTexture> texture(id<MTLDevice> device, unsigned resource) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
        MTLPixelFormatRGBA32Float width:2 height:2 mipmapped:YES];
    d.storageMode = MTLStorageModeShared;
    d.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> result = [device newTextureWithDescriptor:d];
    require(result != nil, "pattern texture allocation");
    for (NSUInteger mip = 0; mip < 2; ++mip) {
        NSUInteger size = 2u >> mip;
        OracleColor data[4];
        for (NSUInteger y = 0; y < size; ++y)
            for (NSUInteger x = 0; x < size; ++x) data[y * size + x] = oracle_texel((int)mip, (int)x, (int)y, resource);
        [result replaceRegion:MTLRegionMake2D(0, 0, size, size) mipmapLevel:mip
            withBytes:data bytesPerRow:size * sizeof(OracleColor)];
    }
    return result;
}

static NSString *source(NSString *state, BOOL pixel) {
    NSString *sample = pixel ? @".sample(s, p.xy)" : @".sample(s, p.xy, level(p.z))";
    return [NSString stringWithFormat:
        @"#include <metal_stdlib>\nusing namespace metal;\n"
         "struct Args { array<texture2d<float>,2> images [[id(0)]]; array<sampler,2> filters [[id(2)]]; };\n"
         "kernel void dynamic_sample(constant Args &a [[buffer(3)]], device float4 *out [[buffer(0)]], "
         "constant float4 &p [[buffer(1)]], constant uint &i [[buffer(2)]], "
         "texture2d<float> direct [[texture(0)]], sampler directFilter [[sampler(0)]]) { "
         "sampler s = a.filters[i]; out[0] = a.images[i]%@; s = directFilter; out[1] = direct%@; }\n%@",
        sample, sample, state ? [NSString stringWithFormat:
            @"kernel void static_sample(device float4 *out [[buffer(0)]], constant float4 &p [[buffer(1)]], "
             "texture2d<float> direct [[texture(0)]]) { constexpr sampler s(%@); out[0] = direct%@; }\n",
            state, sample] : @""];
}

static OracleColor execute(id<MTLDevice> device, id<MTLCommandQueue> queue,
    id<MTLComputePipelineState> pso, id<MTLArgumentEncoder> arguments,
    id<MTLTexture> __strong images[2], id<MTLSamplerState> __strong samplers[2], const OracleCase *test,
    unsigned resource, BOOL isStatic, float clampOverride) {
    id<MTLBuffer> output = guarded(device, 2 * sizeof(OracleColor));
    float values[] = {test->u, test->v, test->lod, 0};
    id<MTLBuffer> parameters = guarded(device, sizeof(values));
    memcpy((uint8_t *)parameters.contents + kOffset, values, sizeof(values));
    id<MTLBuffer> selection = guarded(device, sizeof(resource));
    memcpy((uint8_t *)selection.contents + kOffset, &resource, sizeof(resource));
    id<MTLBuffer> encoded = nil;
    NSData *before = nil;
    if (!isStatic) {
        require(arguments && arguments.alignment && kOffset % arguments.alignment == 0,
            "argument alignment");
        encoded = guarded(device, arguments.encodedLength);
        [arguments setArgumentBuffer:encoded offset:kOffset];
        for (NSUInteger i = 0; i < 2; ++i) {
            [arguments setTexture:images[i] atIndex:i];
            [arguments setSamplerState:samplers[i] atIndex:2 + i];
        }
        before = [NSData dataWithBytes:encoded.contents length:encoded.length];
    }
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
    require(encoder != nil, "pattern encoder");
    [encoder setComputePipelineState:pso];
    [encoder setBuffer:output offset:kOffset atIndex:0];
    [encoder setBuffer:parameters offset:kOffset atIndex:1];
    [encoder setTexture:images[resource] atIndex:0];
    if (!isStatic) {
        [encoder setBuffer:selection offset:kOffset atIndex:2];
        [encoder setBuffer:encoded offset:kOffset atIndex:3];
        if (clampOverride >= 0)
            [encoder setSamplerState:samplers[resource] lodMinClamp:clampOverride lodMaxClamp:clampOverride atIndex:0];
        else [encoder setSamplerState:samplers[resource] atIndex:0];
        for (NSUInteger i = 0; i < 2; ++i) [encoder useResource:images[i] usage:MTLResourceUsageRead];
    }
    [encoder dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [encoder endEncoding];
    completed(command);
    guards(output, 2 * sizeof(OracleColor));
    guards(parameters, sizeof(values));
    guards(selection, sizeof(resource));
    require(!memcmp((uint8_t *)parameters.contents + kOffset, values, sizeof(values)) &&
        !memcmp((uint8_t *)selection.contents + kOffset, &resource, sizeof(resource)), "parameters unchanged");
    int lo, hi;
    OracleColor expected = oracle_evaluate_resource(test, resource, &lo, &hi);
    const OracleColor *actual = (const OracleColor *)((uint8_t *)output.contents + kOffset);
    if (!oracle_color_close(actual[0], expected)) {
        fprintf(stderr, "SAMPLER_PATTERN_MISMATCH name=%s resource=%u static=%d "
            "expected=%g,%g,%g,%g actual=%g,%g,%g,%g\n", test->name, resource, isStatic,
            expected.r, expected.g, expected.b, expected.a, actual[0].r, actual[0].g, actual[0].b, actual[0].a);
        require(0, "GPU pattern matches fixed CPU oracle");
    }
    if (isStatic) {
        const uint8_t *unused = (const uint8_t *)&actual[1];
        for (NSUInteger i = 0; i < sizeof(OracleColor); ++i) require(unused[i] == kCanary, "static unused output unchanged");
    } else {
        OracleCase overridden = *test;
        if (clampOverride >= 0) overridden.lodMin = overridden.lodMax = clampOverride;
        require(oracle_color_close(actual[1], oracle_evaluate_resource(&overridden, resource, &lo, &hi)),
            "direct sample matches oracle and effective LOD clamp");
        require(!memcmp(encoded.contents, before.bytes, encoded.length), "argument bytes unchanged");
    }
    return actual[0];
}

#include "test-sampler-anisotropy.inc"

int main(int argc, const char **argv) {
    @autoreleasepool {
        require((argc == 4 || (argc == 5 && !strcmp(argv[4], "--anisotropy"))) &&
            (!strcmp(argv[1], "--reference") || !strcmp(argv[1], "--bundle")),
            "usage: test-sampler-patterns --reference - OUTPUT | --bundle BUNDLE OUTPUT [--anisotropy]");
        BOOL reference = !strcmp(argv[1], "--reference");
        NSError *error = nil;
        id<MTLDevice> device;
        if (reference) device = MTLCreateSystemDefaultDevice();
        else {
            NSBundle *bundle = [NSBundle bundleWithPath:@(argv[2])];
            require([bundle loadAndReturnError:&error], error.description.UTF8String ?: "bundle load");
            device = [[[bundle classNamed:@"GarnetDevice"] alloc] init];
        }
        require(device != nil, "device available");
        NSString *directory = @(argv[3]);
        require([[NSFileManager defaultManager] createDirectoryAtPath:directory withIntermediateDirectories:YES
            attributes:nil error:&error], "evidence directory");
        run_cpu_texture_oracle();
        id<MTLCommandQueue> queue = [device newCommandQueue];
        require(queue != nil, "queue available");
        if (argc == 5) {
            runAnisotropy(device, queue, directory, reference);
            return 0;
        }
        id<MTLTexture> images[] = {texture(device, 0), texture(device, 1)};
        unsigned dynamicCases = 0, staticCases = 0;
        for (NSUInteger i = 0; i < sizeof(oracleCasesTable) / sizeof(oracleCasesTable[0]); ++i) {
            @autoreleasepool {
                const OracleCase *test = &oracleCasesTable[i];
                NSString *state = staticState(test);
                NSString *msl = source(state, test->pixelCoordinates);
                NSString *label = @(test->name);
                printf("SAMPLER_PATTERN_BEGIN name=%s\n", test->name); fflush(stdout);
                id<MTLComputePipelineState> dynamic = pipeline(device, msl, @"dynamic_sample", directory,
                    [label stringByAppendingString:@"-dynamic"]);
                MTLCompileOptions *options = [MTLCompileOptions new]; options.languageVersion = MTLLanguageVersion2_1;
                id<MTLLibrary> library = [device newLibraryWithSource:msl options:options error:&error];
                id<MTLArgumentEncoder> arguments = [[library newFunctionWithName:@"dynamic_sample"] newArgumentEncoderWithBufferIndex:3];
                MTLSamplerDescriptor *d = descriptor(test);
                id<MTLSamplerState> samplers[] = {[device newSamplerStateWithDescriptor:d], [device newSamplerStateWithDescriptor:d]};
                require(samplers[0] && samplers[1], "valid runtime samplers created");
                id<MTLComputePipelineState> compiledStatic = state ? pipeline(device, msl, @"static_sample", directory,
                    [label stringByAppendingString:@"-static"]) : nil;
                for (unsigned resource = 0; resource < 2; ++resource) {
                    execute(device, queue, dynamic, arguments, images, samplers, test, resource, NO, -1);
                    ++dynamicCases;
                    if (compiledStatic) {
                        execute(device, queue, compiledStatic, nil, images, samplers, test, resource, YES, -1);
                        ++staticCases;
                    }
                }
                if (!test->pixelCoordinates && test->mipFilter != OracleMipNone && test->lod == 0) {
                    execute(device, queue, dynamic, arguments, images, samplers, test, 1, NO, 1);
                    ++dynamicCases;
                }
                printf("SAMPLER_PATTERN_CASE_PASS name=%s resources=2 dynamic_and_direct=1 constexpr=%d gpu_execution=1\n",
                    test->name, state != nil);
            }
        }
        printf("SAMPLER_PATTERNS_PASS backend=%s patterns=%u dynamic_dispatches=%u static_dispatches=%u "
            "guards=output,argument,parameters nonzero_resource=1 cpu_oracle=1 gpu_execution=1\n",
            reference ? "reference" : "bundle", oracleCases, dynamicCases, staticCases);
    }
    return 0;
}
