#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOKit/IOKitLib.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/sysctl.h>

static void fail(NSString *message) __attribute__((noreturn));
static void fail(NSString *message) {
    fprintf(stderr, "ARGBUF_SAMPLER_FAIL %s\n", message.UTF8String);
    exit(1);
}

static void prepareBackend(BOOL live) {
    if (!live) {
        setenv("RADV_DARWIN_MOCK", "1", 1);
        setenv("RADV_DARWIN_MOCK_GC", "gfx1200", 1);
        return;
    }
    const char *mock = getenv("RADV_DARWIN_MOCK");
    if (mock && strcmp(mock, "0")) fail(@"live mode refuses RADV_DARWIN_MOCK");
    char features[4096] = {0};
    size_t size = sizeof features - 1;
    if (sysctlbyname("machdep.cpu.features", features, &size, NULL, 0) || strstr(features, "VMM"))
        fail(@"live mode requires a physical machine");
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("GarnetDriver"));
    if (!service) fail(@"live mode requires GarnetDriver");
    CFTypeRef vendor = IORegistryEntrySearchCFProperty(service, kIOServicePlane, CFSTR("vendor-id"),
        kCFAllocatorDefault, kIORegistryIterateParents | kIORegistryIterateRecursively);
    CFTypeRef chip = IORegistryEntrySearchCFProperty(service, kIOServicePlane, CFSTR("device-id"),
        kCFAllocatorDefault, kIORegistryIterateParents | kIORegistryIterateRecursively);
    uint32_t vendorID = 0, deviceID = 0;
    if (vendor && CFGetTypeID(vendor) == CFDataGetTypeID() && CFDataGetLength(vendor) == sizeof vendorID)
        memcpy(&vendorID, CFDataGetBytePtr(vendor), sizeof vendorID);
    if (chip && CFGetTypeID(chip) == CFDataGetTypeID() && CFDataGetLength(chip) == sizeof deviceID)
        memcpy(&deviceID, CFDataGetBytePtr(chip), sizeof deviceID);
    if (vendor) CFRelease(vendor);
    if (chip) CFRelease(chip);
    IOObjectRelease(service);
    if (vendorID != 0x1002 || deviceID != 0x7590) fail(@"live mode requires PCI 1002:7590");
    puts("ARGBUF_SAMPLER_LIVE_DEVICE pci=1002:7590");
}

static id<MTLComputePipelineState> makePipeline(id<MTLDevice> device, id<MTLLibrary> library,
                                                NSString *name, MTLComputePipelineReflection **reflection,
                                                NSError **error) {
    id<MTLFunction> function = [library newFunctionWithName:name];
    if (!function) fail([NSString stringWithFormat:@"missing function %@", name]);
    MTLComputePipelineDescriptor *descriptor = [MTLComputePipelineDescriptor new];
    descriptor.computeFunction = function;
    MTLPipelineOption options = MTLPipelineOptionBindingInfo | MTLPipelineOptionBufferTypeInfo;
    return [device newComputePipelineStateWithDescriptor:descriptor options:options
        reflection:reflection error:error];
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        setvbuf(stdout, NULL, _IONBF, 0);
        if (argc == 2 && !strcmp(argv[1], "--help")) {
            puts("Usage: test-compute-argbuf-sampler --reference\n       test-compute-argbuf-sampler BUNDLE [--vm|--live]\nDefault: VM/mock. Live mode requires physical PCI 1002:7590 and rejects mock.");
            return 0;
        }
        BOOL reference = argc == 2 && !strcmp(argv[1], "--reference");
        BOOL live = argc == 3 && !strcmp(argv[2], "--live");
        if (!reference && ((argc != 2 && argc != 3) || argv[1][0] == '-' || geteuid() != 0 ||
            (argc == 3 && !live && strcmp(argv[2], "--vm")))) return 2;
        alarm(90);
        id<MTLDevice> device = nil;
        if (reference) device = MTLCreateSystemDefaultDevice();
        else {
            prepareBackend(live);
            setenv("RADV_DEBUG", "nocache", 1);
            setenv("MESA_SHADER_CACHE_DISABLE", "true", 1);
            setenv("RDNA4M_ALLOW", "1", 1);
            setenv("RDNA4M_TEST_INPROC", "1", 1);
            NSError *loadError = nil;
            if (![[NSBundle bundleWithPath:@(argv[1])] loadAndReturnError:&loadError])
                fail(loadError.localizedDescription ?: @"bundle load failed");
            device = [[objc_getClass("GarnetDevice") alloc] init];
            if (device) ((void (*)(id, SEL))objc_msgSend)(device, sel_registerName("initLimits"));
        }
        if (!device) fail(@"Metal device unavailable");
        printf("ARGBUF_SAMPLER_BACKEND mode=%s device=%s\n", reference ? "reference" : live ? "live" : "vm-mock",
            device.name.UTF8String);

        NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
            "struct Params { float4 color; float scale; uint count; };\n"
            "struct Resources { device float *values [[id(0)]]; texture2d<float> tex [[id(1)]]; sampler smp [[id(2)]]; };\n"
            "kernel void probe_kernel(device float *out [[buffer(0)]], constant Params &p [[buffer(1)]], "
            "constant Resources &r [[buffer(2)]], uint i [[thread_position_in_grid]]) { "
            "if(i<p.count) out[i]=r.values[i]*p.scale+r.tex.sample(r.smp,float2(1.25,0.5)).x; }\n"
            "struct TextureOnly { texture2d<float> tex [[id(1)]]; };\n"
            "kernel void argbuf_texture_only(device float *out [[buffer(0)]], "
            "constant TextureOnly &r [[buffer(2)]], uint i [[thread_position_in_grid]]) { "
            "out[i]=r.tex.read(uint2(0)).x; }\n"
            "kernel void direct_texture_sampler(device float *out [[buffer(0)]], "
            "texture2d<float> tex [[texture(0)]], sampler smp [[sampler(0)]], "
            "uint i [[thread_position_in_grid]]) { out[i]=tex.sample(smp,float2(0.5)).x; }\n";
        NSError *error = nil;
        id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
        if (!library) fail([NSString stringWithFormat:@"source compilation failed: %@", error]);

        NSString *const controls[] = {@"direct_texture_sampler", @"argbuf_texture_only"};
        NSMutableArray<id<MTLComputePipelineState>> *controlPipelines = [NSMutableArray array];
        for (NSUInteger i = 0; i < sizeof controls / sizeof controls[0]; ++i) {
            MTLComputePipelineReflection *reflectionInfo = nil;
            error = nil;
            id<MTLComputePipelineState> pipeline = makePipeline(device, library, controls[i],
                &reflectionInfo, &error);
            if (!pipeline || !reflectionInfo || error)
                fail([NSString stringWithFormat:@"control %@ failed: %@", controls[i], error]);
            [controlPipelines addObject:pipeline];
        }

        MTLComputePipelineReflection *probeReflection = nil;
        error = nil;
        id<MTLComputePipelineState> probe = makePipeline(device, library, @"probe_kernel",
            &probeReflection, &error);
        if (!probe || !probeReflection || error)
            fail([NSString stringWithFormat:@"probe pipeline with reflection failed: %@", error.localizedDescription ?: @"no NSError"]);

        const float inputValues[] = { 0.25f, 1.5f, -2.0f, 3.125f };
        const float texels[] = { 0.25f, 0.0f, 0.0f, 1.0f, 0.75f, 0.0f, 0.0f, 1.0f };
        const float scale = 2.0f;
        const uint32_t count = (uint32_t)(sizeof inputValues / sizeof inputValues[0]);
        id<MTLCommandBuffer> command;
        id<MTLBuffer> output;
        __weak id<MTLTexture> retainedTexture;
        __weak id<MTLSamplerState> retainedSampler;
        __attribute__((objc_precise_lifetime)) id<MTLSamplerState> referenceSampler;
        @autoreleasepool {
        id<MTLBuffer> input = [device newBufferWithBytes:inputValues length:sizeof inputValues options:MTLResourceStorageModeShared];
        output = [device newBufferWithLength:sizeof inputValues options:MTLResourceStorageModeShared];
        MTLTextureDescriptor *textureDescriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                                                                                      width:2 height:1 mipmapped:NO];
        textureDescriptor.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> texture = [device newTextureWithDescriptor:textureDescriptor];
        MTLSamplerDescriptor *samplerDescriptor = [MTLSamplerDescriptor new];
        samplerDescriptor.minFilter = MTLSamplerMinMagFilterNearest;
        samplerDescriptor.magFilter = MTLSamplerMinMagFilterNearest;
        samplerDescriptor.mipFilter = MTLSamplerMipFilterNotMipmapped;
        samplerDescriptor.sAddressMode = MTLSamplerAddressModeRepeat;
        samplerDescriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
        samplerDescriptor.normalizedCoordinates = YES;
        samplerDescriptor.supportArgumentBuffers = YES;
        id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:samplerDescriptor];
        // Keep the oracle's sampler alive; RDNA4 independently checks early release.
        if (reference) referenceSampler = sampler;
        if (!input || !output || !output.contents || !texture || !sampler)
            fail(@"probe resource allocation failed");
        [texture replaceRegion:MTLRegionMake2D(0, 0, 2, 1) mipmapLevel:0 withBytes:texels bytesPerRow:sizeof texels];
        id<MTLCommandQueue> queue = [device newCommandQueue];
        for (NSUInteger i = 0; i < controlPipelines.count; ++i) {
            memset(output.contents, 0xa7, output.length);
            id<MTLCommandBuffer> control = [queue commandBuffer];
            id<MTLComputeCommandEncoder> encoder = [control computeCommandEncoder];
            [encoder setComputePipelineState:controlPipelines[i]];
            [encoder setBuffer:output offset:0 atIndex:0];
            if (!i) {
                [encoder setTexture:texture atIndex:0];
                [encoder setSamplerState:sampler atIndex:0];
            } else {
                id<MTLArgumentEncoder> textureArguments = [[library newFunctionWithName:controls[i]] newArgumentEncoderWithBufferIndex:2];
                id<MTLBuffer> storage = [device newBufferWithLength:textureArguments.encodedLength options:MTLResourceStorageModeShared];
                if (!textureArguments || !storage) fail(@"texture control argument buffer unavailable");
                [textureArguments setArgumentBuffer:storage offset:0];
                [textureArguments setTexture:texture atIndex:1];
                [encoder setBuffer:storage offset:0 atIndex:2];
                [encoder useResource:texture usage:MTLResourceUsageRead];
            }
            [encoder dispatchThreads:MTLSizeMake(count, 1, 1) threadsPerThreadgroup:MTLSizeMake(count, 1, 1)];
            [encoder endEncoding];
            [control commit];
            [control waitUntilCompleted];
            if (control.status != MTLCommandBufferStatusCompleted || control.error)
                fail([NSString stringWithFormat:@"control %@ dispatch failed: %@", controls[i], control.error]);
            const float *words = output.contents;
            float expected = i ? 0.25f : 0.75f;
            for (uint32_t word = 0; word < count; ++word)
                if (words[word] != expected) fail([NSString stringWithFormat:@"control %@ output[%u]=%.9g expected=%.9g", controls[i], word, words[word], expected]);
            printf("ARGBUF_SAMPLER_CONTROL_PASS %s words=%u output=%.9g\n", [controls[i] UTF8String], count, expected);
        }
        memset(output.contents, 0xa7, output.length);
        id<MTLFunction> probeFunction = [library newFunctionWithName:@"probe_kernel"];
        id<MTLArgumentEncoder> arguments = [probeFunction newArgumentEncoderWithBufferIndex:2];
        if (!arguments) fail(@"argument encoder missing for buffer(2)");
        id<MTLBuffer> argumentBuffer = [device newBufferWithLength:arguments.encodedLength options:MTLResourceStorageModeShared];
        if (!argumentBuffer || !argumentBuffer.contents) fail(@"argument buffer allocation failed");
        [arguments setArgumentBuffer:argumentBuffer offset:0];
        [arguments setTexture:texture atIndex:1];
        [arguments setSamplerState:sampler atIndex:2];
        struct { float color[4]; float scale; uint32_t count; } params = {{0, 0, 0, 0}, scale, count};
        [arguments setBuffer:input offset:0 atIndex:0];

        command = [queue commandBuffer];
        id<MTLComputeCommandEncoder> compute = [command computeCommandEncoder];
        if (!queue || !command || !compute) fail(@"compute command setup failed");
        [compute setComputePipelineState:probe];
        [compute setBuffer:output offset:0 atIndex:0];
        [compute setBytes:&params length:sizeof params atIndex:1];
        [compute setBuffer:argumentBuffer offset:0 atIndex:2];
        [compute useResource:input usage:MTLResourceUsageRead];
        [compute useResource:texture usage:MTLResourceUsageRead];
        [compute dispatchThreads:MTLSizeMake(count, 1, 1) threadsPerThreadgroup:MTLSizeMake(count, 1, 1)];
        [compute endEncoding];
        retainedTexture = texture;
        retainedSampler = sampler;
        }
        if (!reference && (!retainedTexture || !retainedSampler)) fail(@"argument resources released before command completion");
        [command commit]; [command waitUntilCompleted];
        if (command.status != MTLCommandBufferStatusCompleted || command.error)
            fail([NSString stringWithFormat:@"probe dispatch failed: %@", command.error.localizedDescription ?: @"no NSError"]);
        const float *actual = output.contents;
        BOOL outputsMatch = YES;
        NSMutableString *outputValues = [NSMutableString string];
        for (uint32_t i = 0; i < count; ++i) {
            float expected = inputValues[i] * scale + 0.25f;
            float tolerance = 1.0e-5f * (expected < 0.0f ? -expected : expected) + 1.0e-5f;
            [outputValues appendFormat:@" %u=%.9g(expected %.9g)", i, actual[i], expected];
            if (!(actual[i] == actual[i]) || (actual[i] - expected > tolerance) || (expected - actual[i] > tolerance))
                outputsMatch = NO;
        }
        if (!outputsMatch) fail([NSString stringWithFormat:@"probe outputs:%@", outputValues]);
        printf("ARGBUF_SAMPLER_LIFETIME_PASS released_before_commit=1 reference_sampler_held=%d\n", reference);
        puts("ARGBUF_SAMPLER_PASS");
        return 0;
    }
}
