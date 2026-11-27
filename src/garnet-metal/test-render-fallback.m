// Execute cold draws, hot-swap, and a partial repaint through the production bundle.
// Run on an isolated MoltenVK VM bundle; this does not test the native display transport.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <unistd.h>

@interface NSObject (FallbackTest)
- (id)rdna4Real;
@end

static void fail(NSString *message) __attribute__((noreturn));
static void fail(NSString *message) {
    fprintf(stderr, "RENDER_FALLBACK_FAIL %s\n", message.UTF8String);
    _exit(1);
}

static void finish(id<MTLCommandBuffer> commands) {
    [commands commit];
    [commands waitUntilCompleted];
    if (commands.status != MTLCommandBufferStatusCompleted || commands.error)
        fail(commands.error.description ?: @"command buffer failed");
}

static void draw(id<MTLCommandQueue> queue, id<MTLTexture> texture,
                 id<MTLRenderPipelineState> pipeline, BOOL clear, BOOL partial) {
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = texture;
    pass.colorAttachments[0].loadAction = clear ? MTLLoadActionClear : MTLLoadActionLoad;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(17.0 / 255, 51.0 / 255, 119.0 / 255, 1);
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
    if (!encoder) fail(@"render encoder unavailable");
    if (pipeline) {
        [encoder setRenderPipelineState:pipeline];
        [encoder setViewport:(MTLViewport){0, 0, 32, 32, 0, 1}];
        [encoder setScissorRect:(MTLScissorRect){0, 0, partial ? 8 : 32, partial ? 8 : 32}];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    }
    [encoder endEncoding];
    finish(commands);
}

static NSUInteger check(id<MTLDevice> device, id<MTLCommandQueue> queue,
                        id<MTLTexture> texture, BOOL partial, const char *stage) {
    id<MTLBuffer> output = [device newBufferWithLength:32 * 32 * 4 options:MTLResourceStorageModeShared];
    if (!output) fail(@"readback buffer unavailable");
    memset(output.contents, 0xA5, output.length);
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
    [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
              sourceSize:MTLSizeMake(32, 32, 1) toBuffer:output destinationOffset:0
       destinationBytesPerRow:128 destinationBytesPerImage:4096];
    [blit endEncoding];
    finish(commands);
    const uint8_t *bytes = output.contents;
    NSUInteger bad = 0, magenta = 0;
    for (NSUInteger y = 0; y < 32; ++y) for (NSUInteger x = 0; x < 32; ++x) {
        const uint8_t *pixel = bytes + (y * 32 + x) * 4;
        const uint8_t original[4] = {17, 51, 119, 255}, painted[4] = {64, 128, 192, 255};
        const uint8_t *expected = partial && x < 8 && y < 8 ? painted : original;
        for (unsigned c = 0; c < 4; ++c) {
            if (abs((int)pixel[c] - expected[c]) > 1) { ++bad; break; }
        }
        if (pixel[0] == 255 && pixel[1] == 0 && pixel[2] == 255) ++magenta;
    }
    printf("RENDER_FALLBACK_PIXELS stage=%s bad=%lu magenta=%lu pixels=1024\n",
           stage, (unsigned long)bad, (unsigned long)magenta);
    return bad;
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        if (argc != 3 || geteuid() != 0) {
            fputs("Usage: sudo test-render-fallback VM_TEST_BUNDLE ISOLATED_CACHE_DIRECTORY\n", stderr);
            return 2;
        }
        alarm(30);
        setvbuf(stdout, NULL, _IONBF, 0);
        setenv("RDNA4M_ALLOW", "1", 1);
        setenv("RDNA4M_TEST_INPROC", "1", 1);
        setenv("RDNA4M_TEST_FALLBACK_AS_WS", "1", 1);
        setenv("RDNA4M_TEST_USERCACHE", argv[2], 1);
        setenv("RDNA4M_TEST_SIDE_DIR", argv[2], 1);
        setenv("MVK_CONFIG_LOG_LEVEL", "2", 1);
        setenv("MVK_CONFIG_USE_MTLHEAP", "0", 1);
        NSError *error = nil;
        if (![[NSBundle bundleWithPath:@(argv[1])] loadAndReturnError:&error]) fail(error.description);
        id<MTLDevice> device = [[objc_getClass("GarnetDevice") alloc] init];
        ((void (*)(id, SEL))objc_msgSend)(device, sel_registerName("initLimits"));
        id<MTLCommandQueue> queue = [device newCommandQueue];
        if (!device || !queue) fail(@"device or queue unavailable");
        NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
            "vertex float4 fallback_repaint_vs(uint i [[vertex_id]]) {\n"
            "float2 p[3] = {float2(-1,-1),float2(3,-1),float2(-1,3)};return float4(p[i],0,1); }\n"
            "fragment float4 fallback_repaint_fs() {return float4(64.0/255,128.0/255,192.0/255,1);}\n"
            "fragment float4 fallback_partial_miss_fs() {return float4(192.0/255,64.0/255,128.0/255,1);}\n";
        id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
        if (!library) fail(error.description);
        MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
        descriptor.vertexFunction = [library newFunctionWithName:@"fallback_repaint_vs"];
        descriptor.fragmentFunction = [library newFunctionWithName:@"fallback_repaint_fs"];
        descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
            MTLPixelFormatRGBA8Unorm width:32 height:32 mipmapped:NO];
        td.storageMode = MTLStorageModePrivate;
        td.usage = MTLTextureUsageRenderTarget;
        id<MTLTexture> texture = [device newTextureWithDescriptor:td];
        if (!texture) fail(@"texture allocation failed");
        draw(queue, texture, nil, YES, NO);
        if (check(device, queue, texture, NO, "initial")) fail(@"initial pixels wrong");
        // Isolate a real cache miss; forced fallback intentionally disables the watcher.
        setenv("RDNA4M_TEST_INPROC", "0", 1);
        id<MTLRenderPipelineState> cold = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if (!cold || [(id)cold rdna4Real]) fail(@"cold placeholder not exercised");
        draw(queue, texture, cold, NO, NO);
        NSUInteger bad = check(device, queue, texture, NO, "cold-load");
        setenv("RDNA4M_TEST_INPROC", "1", 1);
        if (![device newRenderPipelineStateWithDescriptor:descriptor error:&error]) fail(error.description);
        double deadline = NSProcessInfo.processInfo.systemUptime + 5;
        while (![(id)cold rdna4Real] && NSProcessInfo.processInfo.systemUptime < deadline) usleep(1000);
        if (![(id)cold rdna4Real]) fail(@"placeholder did not hot-swap");
        // A compositor only repaints new damage; it does not redraw the whole surface after hot-swap.
        draw(queue, texture, cold, NO, YES);
        bad += check(device, queue, texture, YES, "hot-swap-partial-repaint");
        if (bad) fail(@"cold draw corrupted pixels retained after a partial repaint");
        // Single-stage spvcache hit (cached VS) + single-stage miss (uncached FS) must enter fallback cleanly (no Code=49).
        setenv("RDNA4M_TEST_INPROC", "0", 1);
        MTLRenderPipelineDescriptor *partialMiss = [MTLRenderPipelineDescriptor new];
        partialMiss.vertexFunction = descriptor.vertexFunction;
        partialMiss.fragmentFunction = [library newFunctionWithName:@"fallback_partial_miss_fs"];
        partialMiss.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        error = nil;
        id<MTLRenderPipelineState> halfCold = [device newRenderPipelineStateWithDescriptor:partialMiss error:&error];
        if (!halfCold || error) fail(error.description ?: @"single-stage hit + miss fallback returned nil");
        puts("RENDER_FALLBACK_PASS cold_preserved=1024 hot_repaint=64 preserved=960 partial_miss=ok native_display_transport=untested");
        _exit(0);
    }
}
