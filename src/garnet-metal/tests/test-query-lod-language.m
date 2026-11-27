// Compile-only extension probe. This is separate from the fixed MSL 2.1 baseline.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void require(BOOL ok, NSString *message) {
    if (!ok) { fprintf(stderr, "QUERY_LANGUAGE_FAIL %s\n", message.UTF8String); exit(1); }
}
int main(int argc, char **argv) {
    @autoreleasepool {
        require(argc == 3 || (argc == 4 && !strcmp(argv[3], "--compile-only")), @"usage: test-query-lod-language 2.1|2.2|2.3 output-directory [--compile-only]");
        BOOL compileOnly = argc == 4;
        MTLLanguageVersion version;
        if (!strcmp(argv[1], "2.1")) version = MTLLanguageVersion2_1;
        else if (!strcmp(argv[1], "2.2")) version = MTLLanguageVersion2_2;
        else if (!strcmp(argv[1], "2.3")) version = MTLLanguageVersion2_3;
        else { require(NO, @"unsupported language argument"); return 1; }
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        require(device != nil, @"no Metal device");
        if (@available(macOS 11.0, *)) {
            // A lazy device getter must stay read-only. Opening the backend is
            // deliberate here so the second answer can use actual Vulkan caps.
            BOOL preopen = device.supportsQueryTextureLOD;
            id<MTLCommandQueue> queue = [device newCommandQueue];
            require(queue != nil, @"backend queue unavailable");
            printf("QUERY_LANGUAGE_DEVICE name=%s preopen=%d supported=%d language=%s\n",
                device.name.UTF8String, preopen, device.supportsQueryTextureLOD, argv[1]);
            if (!device.supportsQueryTextureLOD && !compileOnly) {
                puts("QUERY_LANGUAGE_SKIP actual device does not advertise LOD query extension");
                return 77;
            }
        } else {
            puts("QUERY_LANGUAGE_DEVICE supported=0 reason=API-unavailable");
            if (!compileOnly) return 77;
        }
        if (compileOnly) puts("QUERY_LANGUAGE_COMPILE_ONLY no device execution or advertised support inferred");
        NSString *directSource = @"#include <metal_stdlib>\nusing namespace metal;\n"
            "fragment float4 query_direct(float4 p [[position]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {\n"
            "  float2 uv = p.xy / float2(32.0, 16.0);\n"
            "  return float4(t.calculate_clamped_lod(s, uv), t.calculate_unclamped_lod(s, uv), 0.0, 1.0);\n}\n";
        NSString *argumentSource = @"#include <metal_stdlib>\nusing namespace metal;\n"
            "struct Resources { array<texture2d<float>, 2> images [[id(0)]]; array<sampler, 2> samplers [[id(2)]]; };\n"
            "fragment float4 query_arguments(float4 p [[position]], constant Resources &r [[buffer(0)]]) {\n"
            "  uint i = uint(p.x) & 1u; uint j = uint(p.y) & 1u; float2 uv = p.xy / float2(32.0, 16.0);\n"
            "  return float4(r.images[i].calculate_clamped_lod(r.samplers[j], uv),\n"
            "    r.images[i].calculate_unclamped_lod(r.samplers[j], uv), 0.0, 1.0);\n}\n";
        MTLCompileOptions *options = [MTLCompileOptions new];
        options.languageVersion = version;
        NSError *error = nil;
        NSString *directory = [NSString stringWithUTF8String:argv[2]];
        require([[NSFileManager defaultManager] createDirectoryAtPath:directory withIntermediateDirectories:YES attributes:nil error:&error], error.localizedDescription ?: @"output directory unavailable");
        NSArray<NSString *> *sources = @[directSource, argumentSource];
        NSUInteger sourceIndex = 0;
        BOOL failed = NO;
        for (NSString *name in @[@"query_direct", @"query_arguments"]) {
            NSString *source = sources[sourceIndex++];
            require([source writeToFile:[directory stringByAppendingPathComponent:[name stringByAppendingString:@".metal"]] atomically:YES encoding:NSUTF8StringEncoding error:&error], @"source write failed");
            error = nil;
            id<MTLLibrary> library = [device newLibraryWithSource:source options:options error:&error];
            if (version == MTLLanguageVersion2_1) {
                if (library) {
                    fprintf(stderr, "QUERY_LANGUAGE_FAIL language=2.1 function=%s unexpectedly admitted the extension\n", name.UTF8String);
                    failed = YES;
                } else {
                    printf("QUERY_LANGUAGE_PASS language=2.1 function=%s refused=%s\n", name.UTF8String, error.localizedDescription.UTF8String);
                }
                continue;
            }
            if (!library) {
                fprintf(stderr, "QUERY_LANGUAGE_FAIL language=%s function=%s compile=%s\n", argv[1], name.UTF8String,
                    (error.localizedDescription ?: @"query extension compilation failed").UTF8String);
                failed = YES;
                continue;
            }
            id<MTLFunction> function = [library newFunctionWithName:name];
            SEL selector = NSSelectorFromString(@"bitcodeData");
            NSData *air = [function respondsToSelector:selector] ? ((id (*)(id, SEL))objc_msgSend)(function, selector) : nil;
            require(air.length > 0, [name stringByAppendingString:@" AIR unavailable"]);
            require([air writeToFile:[directory stringByAppendingPathComponent:[name stringByAppendingString:@".air"]] atomically:YES], @"AIR write failed");
            printf("QUERY_LANGUAGE_PASS language=%s function=%s air_bytes=%lu\n", argv[1], name.UTF8String, (unsigned long)air.length);
        }
        return failed ? 1 : 0;
    }
    return 0;
}
