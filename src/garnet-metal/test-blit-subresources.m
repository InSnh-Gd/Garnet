#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <unistd.h>

#ifdef RDNA4_BLIT_AUDIT
extern unsigned rdna4_audit_invalid_image_copies(void);
extern unsigned rdna4_audit_image_copy_count(void);
extern BOOL rdna4_audit_image_copy(unsigned index, uint32_t fields[10]);
extern unsigned rdna4_audit_submission_count(void);
extern unsigned rdna4_audit_invalid_attachments(void);
extern BOOL rdna4_audit_cube_array(void);
@interface NSObject (RDNA4BlitAudit)
- (BOOL)rdna4Finish:(NSError **)error;
@end
#endif

static void fail(NSString *message) __attribute__((noreturn));
static void fail(NSString *message) {
    fprintf(stderr, "BLIT_SUBRESOURCES_FAIL %s\n", message.UTF8String);
    _exit(1);
}

static void finish(id<MTLCommandBuffer> commands) {
#ifdef RDNA4_BLIT_AUDIT
    NSError *error = nil;
    if (![(id)commands rdna4Finish:&error] || error || rdna4_audit_invalid_image_copies() || rdna4_audit_invalid_attachments() || rdna4_audit_submission_count())
        fail([NSString stringWithFormat:@"RADV preparation: %@", error]);
#else
    [commands commit];
    [commands waitUntilCompleted];
    if (commands.status != MTLCommandBufferStatusCompleted || commands.error)
        fail([NSString stringWithFormat:@"command status=%lu error=%@", (unsigned long)commands.status, commands.error]);
#endif
}

static NSUInteger slices(id<MTLTexture> texture) {
    NSUInteger faces = texture.textureType == MTLTextureTypeCube || texture.textureType == MTLTextureTypeCubeArray ? 6 : 1;
    return texture.arrayLength * faces;
}

static MTLSize dimensions(id<MTLTexture> texture, NSUInteger level) {
    return MTLSizeMake(MAX((NSUInteger)1, texture.width >> level), MAX((NSUInteger)1, texture.height >> level),
                      MAX((NSUInteger)1, texture.depth >> level));
}

static uint32_t value(NSUInteger slice, NSUInteger level, NSUInteger x, NSUInteger y, NSUInteger z) {
    return 0x12340000u + (uint32_t)(slice * 4096 + level * 256 + z * 64 + y * 16 + x);
}

static id<MTLTexture> makeTexture(id<MTLDevice> device, MTLTextureType type, NSUInteger width, NSUInteger height,
                                  NSUInteger depth, NSUInteger count, MTLPixelFormat format) {
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor new];
    descriptor.textureType = type;
    descriptor.pixelFormat = format;
    descriptor.width = width;
    descriptor.height = height;
    descriptor.depth = depth;
    descriptor.arrayLength = count;
    descriptor.storageMode = MTLStorageModePrivate;
    descriptor.usage = MTLTextureUsageShaderRead;
    if (type == MTLTextureTypeCubeArray) descriptor.usage |= MTLTextureUsageRenderTarget;
    NSUInteger largest = MAX(MAX(width, height), depth), levels = 1;
    while (largest > 1) { largest >>= 1; levels++; }
    descriptor.mipmapLevelCount = levels;
    return [device newTextureWithDescriptor:descriptor];
}

static void initialize(id<MTLDevice> device, id<MTLCommandQueue> queue, id<MTLTexture> texture, BOOL source) {
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    id<MTLBlitCommandEncoder> encoder = [commands blitCommandEncoder];
    for (NSUInteger slice = 0; slice < slices(texture); ++slice) {
        for (NSUInteger level = 0; level < texture.mipmapLevelCount; ++level) {
            MTLSize size = dimensions(texture, level);
            NSUInteger pitch = 256, image = pitch * size.height;
            id<MTLBuffer> upload = [device newBufferWithLength:image * size.depth options:MTLResourceStorageModeShared];
            if (!upload || !upload.contents) fail(@"upload allocation");
            memset(upload.contents, 0, upload.length);
            for (NSUInteger z = 0; z < size.depth; ++z) for (NSUInteger y = 0; y < size.height; ++y)
                for (NSUInteger x = 0; x < size.width; ++x) {
                    uint32_t *row = (uint32_t *)((uint8_t *)upload.contents + z * image + y * pitch);
                    row[x] = source ? value(slice, level, x, y, z) : 0xa5a5a5a5u;
                }
            [encoder copyFromBuffer:upload sourceOffset:0 sourceBytesPerRow:pitch sourceBytesPerImage:image
                sourceSize:size toTexture:texture destinationSlice:slice destinationLevel:level destinationOrigin:MTLOriginMake(0,0,0)];
        }
    }
    [encoder endEncoding];
    finish(commands);
}

static void sampleCubes(id<MTLDevice> device, id<MTLCommandQueue> queue, id<MTLTexture> texture, BOOL reference) {
    NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void sample_cubes(texturecube_array<float> image [[texture(0)]], device uint *out [[buffer(0)]], uint i [[thread_position_in_grid]]) {\n"
        " if(i>=48) return; constexpr sampler s(coord::normalized, filter::nearest, mip_filter::nearest);\n"
        " const float3 direction[6]={float3(1,0,0),float3(-1,0,0),float3(0,1,0),float3(0,-1,0),float3(0,0,1),float3(0,0,-1)};\n"
        " uint4 c=uint4(image.sample(s,direction[i%6],(i/6)%2,level(float(i/12)))*255.0f+0.5f);\n"
        " out[i+1]=c.x|(c.y<<8)|(c.z<<16)|(c.w<<24); }\n";
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
    id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"sample_cubes"] error:&error];
    if (!pipeline) fail([NSString stringWithFormat:@"cube-array sampling pipeline: %@", error]);
    uint32_t sentinel[50];
    for (unsigned i = 0; i < 50; ++i) sentinel[i] = 0xa5a5a5a5u;
    id<MTLBuffer> output = [device newBufferWithBytes:sentinel length:sizeof sentinel options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [commands computeCommandEncoder];
    if (!output || !encoder) fail(@"cube-array sampling resources");
    [encoder setComputePipelineState:pipeline];
    [encoder setTexture:texture atIndex:0];
    [encoder setBuffer:output offset:0 atIndex:0];
    [encoder dispatchThreads:MTLSizeMake(48,1,1) threadsPerThreadgroup:MTLSizeMake(16,1,1)];
    [encoder endEncoding];
    finish(commands);
#ifdef RDNA4_BLIT_AUDIT
    (void)reference;
    printf("BLIT_SUBRESOURCES_AUDIT_PASS mode=sample-cube cube_array_enabled=1 submissions=0\n");
#else
    const uint32_t *actual = output.contents;
    if (actual[0] != sentinel[0] || actual[49] != sentinel[49]) fail(@"cube-array sampling guards");
    for (unsigned i = 0; i < 48; ++i) {
        NSUInteger mip = i / 12, slice = i % 12, center = MAX((NSUInteger)1, texture.width >> mip) / 2;
        uint32_t expected = value(slice, mip, center, center, 0);
        if (actual[i+1] != expected) fail([NSString stringWithFormat:@"cube-array sample %u actual=%#x expected=%#x", i, actual[i+1], expected]);
    }
    printf("BLIT_SUBRESOURCES_EXECUTION_PASS mode=sample-cube samples=48 cubes=2 faces=6 mips=4 reference=%u\n", reference);
#endif
}

int main(int argc, const char *argv[]) {
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        puts("Usage: sudo test-blit-subresources BUNDLE|--reference MODE [--prepare]\n"
             "Modes: whole-array whole-upsize whole-downsize range-array whole-3d range-3d whole-cube range-cube alias-array sample-cube render-cube\n"
             "       bad-slice bad-level bad-count bad-size bad-format bad-overlap bad-origin\n"
             "Copies private textures and checks every mip, slice, depth plane and readback padding. Audit builds use --prepare without submission.");
        return 0;
    }
    if (argc < 3 || argc > 4 || (argc == 4 && strcmp(argv[3], "--prepare"))) return 2;
    const char *mode = argv[2];
    BOOL reference = !strcmp(argv[1], "--reference"), invalid = !strncmp(mode, "bad-", 4);
    BOOL prepare = argc == 4;
#ifdef RDNA4_BLIT_AUDIT
    if (!prepare || reference) return 2;
#else
    if (prepare) return 2;
#endif
    const char *modes[] = {"whole-array", "whole-upsize", "whole-downsize", "range-array", "whole-3d", "range-3d",
        "whole-cube", "range-cube", "alias-array", "sample-cube", "render-cube", "bad-slice", "bad-level", "bad-count", "bad-size", "bad-format", "bad-overlap", "bad-origin"};
    BOOL recognized = NO;
    for (unsigned i = 0; i < sizeof(modes) / sizeof(*modes); ++i) recognized |= !strcmp(mode, modes[i]);
    if (!recognized || (reference && invalid) || (!reference && (argv[1][0] == '-' || geteuid() != 0))) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(60);
    @autoreleasepool {
        @try {
            id<MTLDevice> device;
            if (reference) device = MTLCreateSystemDefaultDevice();
            else {
                setenv("RDNA4M_ALLOW", "1", 1);
                setenv("RDNA4M_TEST_INPROC", "1", 1);
                setenv("MVK_CONFIG_LOG_LEVEL", "2", 1);
                setenv("MVK_CONFIG_USE_MTLHEAP", "0", 1);
                if (prepare) {
                    setenv("RADV_DARWIN_MOCK", "1", 1);
                    setenv("RADV_DARWIN_MOCK_GC", "gfx1200", 1);
                }
                NSError *error = nil;
                if (![[NSBundle bundleWithPath:@(argv[1])] loadAndReturnError:&error]) fail(error.description);
                device = [[objc_getClass("GarnetDevice") alloc] init];
                ((void (*)(id, SEL))objc_msgSend)(device, sel_registerName("initLimits"));
            }
            id<MTLCommandQueue> queue = [device newCommandQueue];
            if (!device || !queue) fail(@"device or queue unavailable");
            BOOL threeD = strstr(mode, "3d") != NULL, cube = strstr(mode, "cube") != NULL;
            BOOL renderCube = !strcmp(mode, "render-cube");
            BOOL range = !strncmp(mode, "range-", 6) || invalid || !strcmp(mode, "alias-array");
            BOOL alias = !strcmp(mode, "alias-array") || !strcmp(mode, "bad-overlap") || renderCube;
            MTLTextureType type = threeD ? MTLTextureType3D : cube ?
                (strcmp(mode, "whole-cube") ? MTLTextureTypeCubeArray : MTLTextureTypeCube) : MTLTextureType2DArray;
            NSUInteger sw = 8, sh = 8, sd = threeD ? 4 : 1, dw = 8, dh = 8, dd = sd;
            NSUInteger sourceArrays = threeD || cube ? 1 : 5, destinationArrays = threeD || cube ? 1 : 6;
            NSUInteger ss = 0, sl = 0, ds = 0, dl = 0, sliceCount = MIN(sourceArrays, destinationArrays), levelCount = 4;
            if (!strcmp(mode, "whole-upsize")) { dw = dh = 16; dl = 1; }
            if (!strcmp(mode, "whole-downsize")) { sw = sh = 16; sl = 1; }
            if (!strcmp(mode, "range-array")) { sw = sh = 16; ss = 1; sl = 1; ds = 2; sliceCount = 2; levelCount = 3; }
            if (!strcmp(mode, "range-3d")) { sw = 16; sh = 8; sd = 8; dw = 8; dh = 4; dd = 4; sl = 1; levelCount = 3; }
            if (!strcmp(mode, "whole-cube")) sliceCount = 6;
            if (!strcmp(mode, "range-cube")) { sourceArrays = 2; destinationArrays = 3; ss = 4; ds = 10; sliceCount = 5; levelCount = 3; }
            if (!strcmp(mode, "sample-cube") || renderCube) sourceArrays = destinationArrays = 2;
            if (alias) { ds = 3; sliceCount = 2; }
            if (!strcmp(mode, "bad-slice")) { ss = 4; sliceCount = 2; }
            if (!strcmp(mode, "bad-level")) { sl = 3; levelCount = 2; }
            if (!strcmp(mode, "bad-count")) sliceCount = NSUIntegerMax;
            if (!strcmp(mode, "bad-size")) sw = sh = 16;
            if (!strcmp(mode, "bad-overlap")) { ds = 1; sliceCount = 3; }
            id<MTLTexture> source = makeTexture(device, type, sw, sh, sd, sourceArrays, MTLPixelFormatRGBA8Unorm);
            id<MTLTexture> destination = alias ? source : makeTexture(device, type, dw, dh, dd, destinationArrays,
                !strcmp(mode, "bad-format") ? MTLPixelFormatBGRA8Unorm : MTLPixelFormatRGBA8Unorm);
            if (!source || !destination) fail(@"private texture creation");
#ifdef RDNA4_BLIT_AUDIT
            if (type == MTLTextureTypeCubeArray && !rdna4_audit_cube_array()) fail(@"cube-array image feature disabled");
#endif
            initialize(device, queue, source, YES);
            if (!alias) initialize(device, queue, destination, NO);
            if (!strcmp(mode, "sample-cube")) { sampleCubes(device, queue, source, reference); _exit(0); }
            id<MTLCommandBuffer> commands = [queue commandBuffer];
            id<MTLBlitCommandEncoder> encoder = nil;
            if (renderCube) {
                MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
                pass.colorAttachments[0].texture = source;
                pass.colorAttachments[0].slice = 7;
                pass.colorAttachments[0].level = 2;
                pass.colorAttachments[0].loadAction = MTLLoadActionClear;
                pass.colorAttachments[0].storeAction = MTLStoreActionStore;
                pass.colorAttachments[0].clearColor = MTLClearColorMake(16.0/255,32.0/255,48.0/255,64.0/255);
                id<MTLRenderCommandEncoder> render = [commands renderCommandEncoderWithDescriptor:pass];
                if (!render) fail(@"cube-array render target");
                [render endEncoding];
            } else {
                encoder = [commands blitCommandEncoder];
                if (!strcmp(mode, "bad-origin")) [encoder copyFromTexture:source sourceSlice:0 sourceLevel:0
                    sourceOrigin:MTLOriginMake(NSUIntegerMax,0,0) sourceSize:MTLSizeMake(8,8,1) toTexture:destination
                    destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
                else if (range) [encoder copyFromTexture:source sourceSlice:ss sourceLevel:sl toTexture:destination destinationSlice:ds
                    destinationLevel:dl sliceCount:sliceCount levelCount:levelCount];
                else [encoder copyFromTexture:source toTexture:destination];
                [encoder endEncoding];
            }
            if (invalid) {
#ifdef RDNA4_BLIT_AUDIT
                NSError *error = nil;
                if ([(id)commands rdna4Finish:&error] || !error || rdna4_audit_image_copy_count() || rdna4_audit_submission_count())
                    fail(@"invalid copy recorded or submitted");
#else
                [commands commit]; [commands waitUntilCompleted];
                if (commands.status != MTLCommandBufferStatusError || !commands.error) fail(@"invalid copy accepted");
#endif
                printf("BLIT_SUBRESOURCES_REJECTION_PASS mode=%s prepare=%u\n", mode, prepare);
                _exit(0);
            }
            finish(commands);
#ifdef RDNA4_BLIT_AUDIT
            if (renderCube) {
                if (rdna4_audit_image_copy_count()) fail(@"cube-array render unexpectedly copied an image");
                puts("BLIT_SUBRESOURCES_AUDIT_PASS mode=render-cube face=7 mip=2 attachments_valid=1 submissions=0");
                _exit(0);
            }
            if (rdna4_audit_image_copy_count() != sliceCount * levelCount) fail(@"RADV copy count");
            for (NSUInteger level = 0; level < levelCount; ++level) for (NSUInteger slice = 0; slice < sliceCount; ++slice) {
                MTLSize size = dimensions(source, sl + level);
                uint32_t actual[10], expected[] = { (uint32_t)(sl + level), (uint32_t)(ss + slice),
                    (uint32_t)(dl + level), (uint32_t)(ds + slice), (uint32_t)size.width, (uint32_t)size.height,
                    (uint32_t)size.depth, 1, 1, alias };
                if (!rdna4_audit_image_copy((unsigned)(level * sliceCount + slice), actual) || memcmp(actual, expected, sizeof actual))
                    fail(@"RADV copy subresource, extent or shared-image identity");
            }
            printf("BLIT_SUBRESOURCES_AUDIT_PASS mode=%s regions=%u layouts_valid=1 submissions=0\n",
                mode, rdna4_audit_image_copy_count());
            _exit(0);
#endif
            NSUInteger checked = 0, subresources = 0;
            for (NSUInteger slice = 0; slice < slices(destination); ++slice) {
                for (NSUInteger level = 0; level < destination.mipmapLevelCount; ++level) {
                    MTLSize size = dimensions(destination, level);
                    NSUInteger pitch = 256, image = pitch * size.height, guard = 256, length = image * size.depth + 2 * guard;
                    id<MTLBuffer> result = [device newBufferWithLength:length options:MTLResourceStorageModeShared];
                    if (!result || !result.contents) fail(@"readback buffer");
                    memset(result.contents, 0xCD, length);
                    commands = [queue commandBuffer]; encoder = [commands blitCommandEncoder];
                    [encoder copyFromTexture:destination sourceSlice:slice sourceLevel:level sourceOrigin:MTLOriginMake(0,0,0)
                        sourceSize:size toBuffer:result destinationOffset:guard destinationBytesPerRow:pitch destinationBytesPerImage:image];
                    [encoder endEncoding]; finish(commands);
                    const uint32_t *actual = result.contents;
                    for (NSUInteger word = 0; word < length / 4; ++word) {
                        uint32_t expected = 0xcdcdcdcdu;
                        if (word >= guard / 4 && word < (length - guard) / 4) {
                            NSUInteger offset = word * 4 - guard, z = offset / image, y = offset % image / pitch, x = offset % pitch / 4;
                            if (x < size.width) {
                                BOOL copied = slice >= ds && slice - ds < sliceCount && level >= dl && level - dl < levelCount;
                                expected = renderCube ? (slice == 7 && level == 2 ? 0x40302010u : value(slice, level, x, y, z)) :
                                    copied ? value(ss + slice - ds, sl + level - dl, x, y, z) :
                                    alias ? value(slice, level, x, y, z) : 0xa5a5a5a5u;
                            }
                        }
                        if (actual[word] != expected) fail([NSString stringWithFormat:
                            @"mode=%s slice=%lu level=%lu word=%lu actual=%#x expected=%#x", mode, (unsigned long)slice,
                            (unsigned long)level, (unsigned long)word, actual[word], expected]);
                        checked++;
                    }
                    subresources++;
                }
            }
            printf("BLIT_SUBRESOURCES_EXECUTION_PASS mode=%s subresources=%lu words=%lu reference=%u private_textures=1\n",
                mode, (unsigned long)subresources, (unsigned long)checked, reference);
            _exit(0);
        } @catch (NSException *exception) { fail(exception.description); }
    }
}
