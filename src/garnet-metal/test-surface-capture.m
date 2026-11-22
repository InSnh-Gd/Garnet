// Real IOSurface import, GPU read and production completion capture on an isolated VM bundle.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <fcntl.h>
#include <unistd.h>

static void fail(NSString *why) {
    fprintf(stderr, "SURFACE_CAPTURE_FAIL %s\n", why.UTF8String);
    exit(1);
}
static NSArray *captures(NSString *directory) {
    NSString *prefix = [NSString stringWithFormat:@"surface-%d-", getpid()];
    NSMutableArray *files = [NSMutableArray array];
    for (NSString *name in [[NSFileManager defaultManager] contentsOfDirectoryAtPath:directory error:NULL])
        if ([name hasPrefix:prefix] && [name hasSuffix:@".json"]) [files addObject:[directory stringByAppendingPathComponent:name]];
    return files;
}
int main(int argc, const char **argv) { @autoreleasepool {
    if (argc != 2 || geteuid() != 0) return 2;
    alarm(30); setvbuf(stdout, NULL, _IONBF, 0);
    const char *flag = "/private/tmp/rdna4m-surfaces";
    if (access(flag, F_OK) == 0) fail(@"test needs an absent capture flag");
    setenv("RDNA4M_ALLOW", "1", 1); setenv("RDNA4M_IOS_PATH", "b", 1);
    setenv("MVK_CONFIG_USE_MTLHEAP", "0", 1);
    NSError *error = nil;
    if (![[NSBundle bundleWithPath:@(argv[1])] loadAndReturnError:&error]) fail(error.description);
    id<MTLDevice> device = [[objc_getClass("GarnetDevice") alloc] init];
    ((void (*)(id, SEL))objc_msgSend)(device, sel_registerName("initLimits"));
    id<MTLCommandQueue> queue = [device newCommandQueue];
    const NSUInteger width = 330, height = 32, stride = 2816;
    IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef)@{
        (id)kIOSurfaceWidth: @(width), (id)kIOSurfaceHeight: @(height),
        (id)kIOSurfaceBytesPerElement: @8, (id)kIOSurfaceBytesPerRow: @(stride),
        (id)kIOSurfaceAllocSize: @(stride * height), (id)kIOSurfacePixelFormat: @0x52476841 });
    if (!surface || !queue) fail(@"surface/queue unavailable");
    IOSurfaceLock(surface, 0, NULL);
    uint8_t *base = IOSurfaceGetBaseAddress(surface);
    memset(base, 0xA5, stride * height);
    const uint16_t color[] = {0x3400, 0x3800, 0x3A00, 0x3C00};
    for (NSUInteger y = 0; y < height; ++y) for (NSUInteger x = 0; x < width; ++x)
        memcpy(base + y * stride + x * 8, color, 8);
    IOSurfaceUnlock(surface, 0, NULL);
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:width height:height mipmapped:NO];
    td.storageMode = MTLStorageModeManaged; td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    id<MTLTexture> texture = [device newTextureWithDescriptor:td iosurface:surface plane:0];
    if (!texture) fail(@"surface import refused");
    NSString *directory = @"/private/tmp/rdna4m/0";
    for (unsigned enabled = 0; enabled < 2; ++enabled) {
        if (enabled) {
            int fd = open(flag, O_CREAT | O_EXCL | O_WRONLY, 0644);
            if (fd < 0) fail(@"capture flag creation failed");
            close(fd); usleep(1100000);
        }
        id<MTLBuffer> output = [device newBufferWithLength:stride * height options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        if (enabled) {
            MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = texture;
            pass.colorAttachments[0].loadAction = MTLLoadActionClear;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            pass.colorAttachments[0].clearColor = MTLClearColorMake(.125, .25, .5, 1);
            id<MTLRenderCommandEncoder> render = [cb renderCommandEncoderWithDescriptor:pass];
            [render endEncoding];
        }
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
            sourceSize:MTLSizeMake(width,height,1) toBuffer:output destinationOffset:0
            destinationBytesPerRow:stride destinationBytesPerImage:stride * height];
        [blit endEncoding]; [cb commit]; [cb waitUntilCompleted];
        if (cb.status != MTLCommandBufferStatusCompleted || cb.error) fail(cb.error.description ?: @"GPU read failed");
        for (NSUInteger y = 0; y < height; ++y)
            if (memcmp((uint8_t *)output.contents + y * stride, base + y * stride, width * 8)) fail(@"GPU pixel mismatch");
        const uint16_t gpuColor[] = {0x3000, 0x3400, 0x3800, 0x3C00};
        if (enabled && memcmp(base, gpuColor, sizeof gpuColor)) fail(@"GPU clear did not reach the surface before capture");
        if (!enabled && captures(directory).count) fail(@"capture ran without opt-in");
    }
    NSArray *files = nil;
    for (unsigned i = 0; i < 200; ++i) { files = captures(directory); if (files.count) break; usleep(10000); }
    unlink(flag);
    if (files.count != 1) fail(@"expected one production completion snapshot");
    NSString *metaPath = files[0];
    NSDictionary *meta = [NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:metaPath] options:0 error:NULL];
    NSData *pixels = [NSData dataWithContentsOfFile:[[metaPath stringByDeletingPathExtension] stringByAppendingPathExtension:@"raw"]];
    const NSUInteger bytes = (height - 1) * stride + width * 8;
    if ([meta[@"width"] unsignedLongValue] != width || [meta[@"bytes_per_row"] unsignedLongValue] != stride ||
        [meta[@"pixel_format"] unsignedLongValue] != MTLPixelFormatRGBA16Float || pixels.length != bytes ||
        memcmp(pixels.bytes, base, bytes)) fail(@"saved metadata/pixels differ from IOSurface");
    printf("SURFACE_CAPTURE_PASS default_off=1 completion=1 format=RGBA16Float width=%lu stride=%lu bytes=%lu exact\n",
        (unsigned long)width, (unsigned long)stride, (unsigned long)bytes);
    CFRelease(surface); _exit(0);
} }
