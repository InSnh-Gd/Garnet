#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

static void require(BOOL ok, NSString *message) {
    if (!ok) { fprintf(stderr, "ARGUMENT_SAMPLER_SCOPE_FAIL %s\n", message.UTF8String); exit(1); }
}

static id<MTLSamplerState> makeSampler(id<MTLDevice> device, NSUInteger tag) {
    MTLSamplerDescriptor *descriptor = [MTLSamplerDescriptor new];
    descriptor.supportArgumentBuffers = YES;
    descriptor.minFilter = descriptor.magFilter = MTLSamplerMinMagFilterNearest;
    descriptor.mipFilter = MTLSamplerMipFilterNotMipmapped;
    descriptor.sAddressMode = (tag & 1u) ? MTLSamplerAddressModeRepeat : MTLSamplerAddressModeClampToEdge;
    descriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
    descriptor.rAddressMode = MTLSamplerAddressModeClampToEdge;
    descriptor.normalizedCoordinates = YES;
    descriptor.lodMinClamp = 0.0f;
    descriptor.lodMaxClamp = 1.0f + (float)tag;
    return [device newSamplerStateWithDescriptor:descriptor];
}

static id<MTLArgumentEncoder> makeEncoder(id<MTLLibrary> library, NSString *name) {
    id<MTLFunction> function = [library newFunctionWithName:name];
    require(function != nil, [NSString stringWithFormat:@"missing function %@", name]);
    id<MTLArgumentEncoder> encoder = [function newArgumentEncoderWithBufferIndex:3];
    require(encoder != nil && encoder.encodedLength >= sizeof(uint64_t),
        [NSString stringWithFormat:@"missing argument encoder %@", name]);
    return encoder;
}

int main(int argc, const char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        puts("Usage: test-argument-sampler-scope BUNDLE|--reference OUTPUT_DIR");
        return 0;
    }
    if (argc != 3) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(90);
    @autoreleasepool {
        NSError *error = nil;
        BOOL reference = !strcmp(argv[1], "--reference");
        BOOL ownerAssertions = !reference;
        NSString *directory = @(argv[2]);
        require([[NSFileManager defaultManager] createDirectoryAtPath:directory withIntermediateDirectories:YES
            attributes:nil error:&error], error.description ?: @"output directory unavailable");
        id<MTLDevice> device = nil;
        if (reference) device = MTLCreateSystemDefaultDevice();
        else {
            NSBundle *bundle = [NSBundle bundleWithPath:@(argv[1])];
            require([bundle loadAndReturnError:&error], error.description ?: @"bundle load failed");
            device = [[[bundle classNamed:@"GarnetDevice"] alloc] init];
        }
        require(device != nil, @"Metal device unavailable");

        NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
            "struct SamplerArgs { texture2d<float> image [[id(2)]]; sampler filter [[id(7)]]; };\n"
            "struct PointerArgs { constant uint *data [[id(7)]]; };\n"
            "struct TextureArgs { texture2d<float> image [[id(7)]]; };\n"
            "kernel void sampler_scope(constant SamplerArgs &a [[buffer(3)]], device uint *out [[buffer(0)]]) {"
            " out[0] = uint(a.image.sample(a.filter, float2(0.5)).x); }\n"
            "kernel void pointer_scope(constant PointerArgs &a [[buffer(3)]], device uint *out [[buffer(0)]]) {"
            " out[0] = a.data[0]; }\n"
            "kernel void texture_scope(constant TextureArgs &a [[buffer(3)]], device uint *out [[buffer(0)]]) {"
            " out[0] = uint(a.image.read(uint2(0)).x); }\n";
        require([source writeToFile:[directory stringByAppendingPathComponent:@"sampler-scope.metal"]
            atomically:YES encoding:NSUTF8StringEncoding error:&error], @"source write failed");
        MTLCompileOptions *compile = [MTLCompileOptions new];
        compile.languageVersion = MTLLanguageVersion2_1;
        id<MTLLibrary> library = [device newLibraryWithSource:source options:compile error:&error];
        require(library != nil, error.description ?: @"source compilation failed");

        id<MTLBuffer> argumentBuffer = [device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
        id<MTLBuffer> pointer = [device newBufferWithLength:16 options:MTLResourceStorageModeShared];
        MTLTextureDescriptor *textureDescriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
            width:1 height:1 mipmapped:NO];
        textureDescriptor.storageMode = MTLStorageModeShared;
        textureDescriptor.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> texture = [device newTextureWithDescriptor:textureDescriptor];
        require(argumentBuffer && argumentBuffer.contents && pointer && pointer.contents && texture,
            @"scope resources unavailable");

        // Argument-buffer ownership is an RDNA4 backend guarantee; Apple's
        // reference implementation does not make the same weak-liveness promise.
        __weak id<MTLSamplerState> firstSampler = nil;
        __weak id<MTLSamplerState> previousSampler = nil;
        __weak id<MTLSamplerState> lastSampler = nil;
        @autoreleasepool {
            id<MTLArgumentEncoder> encoder = makeEncoder(library, @"sampler_scope");
            [encoder setArgumentBuffer:argumentBuffer offset:0];
            for (NSUInteger i = 0; i < 128; ++i) {
                @autoreleasepool {
                    __attribute__((objc_precise_lifetime)) id<MTLSamplerState> state =
                        makeSampler(device, i);
                    require(state != nil, @"sampler allocation failed");
                    if (i == 0) firstSampler = state;
                    if (i == 126) previousSampler = state;
                    lastSampler = state;
                    [encoder setSamplerState:state atIndex:7];
                    state = nil;
                }
            }
            encoder = nil;
        }
        if (ownerAssertions) {
            require(firstSampler == nil, @"same-slot history retained the first sampler");
            require(previousSampler == nil, @"same-slot history retained the previous sampler");
            require(lastSampler != nil, @"argument buffer released the last sampler");
        }

        __weak id<MTLSamplerState> nilTarget = nil;
        @autoreleasepool {
            id<MTLArgumentEncoder> encoder = makeEncoder(library, @"sampler_scope");
            [encoder setArgumentBuffer:argumentBuffer offset:0];
            __attribute__((objc_precise_lifetime)) id<MTLSamplerState> state = makeSampler(device, 200);
            require(state != nil, @"nil-target sampler allocation failed");
            nilTarget = state;
            [encoder setSamplerState:state atIndex:7];
            state = nil;
            encoder = nil;
        }
        if (ownerAssertions) require(nilTarget != nil, @"argument buffer did not own nil-target sampler");
        @autoreleasepool {
            id<MTLArgumentEncoder> encoder = makeEncoder(library, @"sampler_scope");
            [encoder setArgumentBuffer:argumentBuffer offset:0];
            [encoder setSamplerState:nil atIndex:7];
            encoder = nil;
        }
        if (ownerAssertions) require(nilTarget == nil, @"nil sampler setter retained the old slot");

        __weak id<MTLSamplerState> pointerTarget = nil;
        @autoreleasepool {
            id<MTLArgumentEncoder> encoder = makeEncoder(library, @"sampler_scope");
            [encoder setArgumentBuffer:argumentBuffer offset:0];
            __attribute__((objc_precise_lifetime)) id<MTLSamplerState> state = makeSampler(device, 201);
            require(state != nil, @"pointer-target sampler allocation failed");
            pointerTarget = state;
            [encoder setSamplerState:state atIndex:7];
            state = nil;
            encoder = nil;
        }
        if (ownerAssertions) require(pointerTarget != nil, @"argument buffer did not own pointer-target sampler");
        @autoreleasepool {
            // SamplerArgs.filter[[id(7)]] starts 8 bytes into the layout;
            // offset 0 would overwrite image[[id(2)]] instead.
            id<MTLArgumentEncoder> encoder = makeEncoder(library, @"pointer_scope");
            [encoder setArgumentBuffer:argumentBuffer offset:sizeof(uint64_t)];
            [encoder setBuffer:pointer offset:0 atIndex:7];
            encoder = nil;
        }
        if (ownerAssertions) require(pointerTarget == nil, @"pointer write retained the old sampler slot");

        __weak id<MTLSamplerState> textureTarget = nil;
        @autoreleasepool {
            id<MTLArgumentEncoder> encoder = makeEncoder(library, @"sampler_scope");
            [encoder setArgumentBuffer:argumentBuffer offset:0];
            __attribute__((objc_precise_lifetime)) id<MTLSamplerState> state = makeSampler(device, 202);
            require(state != nil, @"texture-target sampler allocation failed");
            textureTarget = state;
            [encoder setSamplerState:state atIndex:7];
            state = nil;
            encoder = nil;
        }
        if (ownerAssertions) require(textureTarget != nil, @"argument buffer did not own texture-target sampler");
        @autoreleasepool {
            // Match the 8-byte sampler slot in SamplerArgs, not the image slot.
            id<MTLArgumentEncoder> encoder = makeEncoder(library, @"texture_scope");
            [encoder setArgumentBuffer:argumentBuffer offset:sizeof(uint64_t)];
            [encoder setTexture:texture atIndex:7];
            encoder = nil;
        }
        if (ownerAssertions) require(textureTarget == nil, @"texture write retained the old sampler slot");

        __weak id<MTLSamplerState> guardedTarget = nil;
        @autoreleasepool {
            id<MTLArgumentEncoder> encoder = makeEncoder(library, @"sampler_scope");
            [encoder setArgumentBuffer:argumentBuffer offset:0];
            __attribute__((objc_precise_lifetime)) id<MTLSamplerState> state = makeSampler(device, 203);
            require(state != nil, @"guarded sampler allocation failed");
            guardedTarget = state;
            [encoder setSamplerState:state atIndex:7];
            state = nil;
            encoder = nil;
        }
        if (ownerAssertions) require(guardedTarget != nil, @"argument buffer did not own guarded sampler");
        if (ownerAssertions) {
            @autoreleasepool {
                id<MTLArgumentEncoder> encoder = makeEncoder(library, @"sampler_scope");
                [encoder setArgumentBuffer:argumentBuffer offset:0];
                __attribute__((objc_precise_lifetime)) id<MTLSamplerState> first = makeSampler(device, 204);
                __attribute__((objc_precise_lifetime)) id<MTLSamplerState> second = makeSampler(device, 205);
                id<MTLSamplerState> invalid[2] = {first, second};
                BOOL rejected = NO;
                @try {
                    [encoder setSamplerStates:invalid withRange:NSMakeRange(7, 2)];
                } @catch (NSException *exception) {
                    rejected = YES;
                }
                require(rejected, @"out-of-range sampler batch write was accepted");
                first = nil; second = nil; encoder = nil;
            }
            require(guardedTarget != nil, @"failed batch write released the existing slot");
        } else {
            // Apple aborts on this validation failure with MTL_ERROR_MODE=assert.
            puts("ARGUMENT_SAMPLER_SCOPE_SKIP case=invalid_bulk reason=reference_assert_mode");
        }

        __weak id<MTLSamplerState> bufferTarget = nil;
        @autoreleasepool {
            id<MTLBuffer> scopedBuffer = [device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
            require(scopedBuffer && scopedBuffer.contents, @"scoped argument buffer unavailable");
            id<MTLArgumentEncoder> encoder = makeEncoder(library, @"sampler_scope");
            [encoder setArgumentBuffer:scopedBuffer offset:0];
            __attribute__((objc_precise_lifetime)) id<MTLSamplerState> state = makeSampler(device, 206);
            require(state != nil, @"buffer-owned sampler allocation failed");
            bufferTarget = state;
            [encoder setSamplerState:state atIndex:7];
            state = nil;
            encoder = nil;
            if (ownerAssertions) require(bufferTarget != nil, @"argument buffer did not own its sampler");
            scopedBuffer = nil;
        }
        if (ownerAssertions) require(bufferTarget == nil, @"released argument buffer retained its sampler");

        printf("ARGUMENT_SAMPLER_SCOPE_PASS backend=%s owner_assertions=%s\n",
            reference ? "reference" : "bundle", ownerAssertions ? "asserted" : "skipped");
    }
    return 0;
}
