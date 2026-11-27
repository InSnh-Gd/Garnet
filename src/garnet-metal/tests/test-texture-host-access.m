#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#import <objc/runtime.h>
#include <sys/mman.h>
#include <unistd.h>

static void check(BOOL okay, const char *why) {
    if (!okay) { fprintf(stderr, "TEXTURE_HOST_ACCESS_FAIL %s\n", why); exit(1); }
}

static void gpuRead(id<MTLDevice> device, id<MTLTexture> texture,
                    const uint8_t *expected, NSUInteger stride, NSUInteger bpp) {
    NSUInteger size = stride * texture.height;
    id<MTLBuffer> output = [device newBufferWithLength:size options:MTLResourceStorageModeShared];
    check(output && output.contents, "readback allocation");
    memset(output.contents, 0xCD, size);
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
    check(blit != nil, "readback encoder");
    [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
        sourceSize:MTLSizeMake(texture.width,texture.height,1) toBuffer:output destinationOffset:0
        destinationBytesPerRow:stride destinationBytesPerImage:size];
    [blit endEncoding]; [command commit]; [command waitUntilCompleted];
    check(command.status == MTLCommandBufferStatusCompleted && !command.error, "GPU read failed");
    for (NSUInteger y = 0; y < texture.height; ++y)
        check(!memcmp((uint8_t *)output.contents + y * stride, expected + y * stride,
            texture.width * bpp), "GPU pixels differ from host pixels");
}

static void exercise(id<MTLDevice> device, id<MTLTexture> texture, uint8_t *base,
                     NSUInteger stride, NSUInteger bpp, BOOL readonly) {
    const MTLRegion patch = MTLRegionMake2D(3, 2, 4, 3);
    const NSUInteger length = stride * texture.height;
    NSMutableData *expected = [NSMutableData dataWithBytes:base length:length];
    uint8_t upload[96]; memset(upload, 0x7B, sizeof upload);
    // A read-only import is a driver policy test, not a Metal reference guarantee.
    [texture replaceRegion:patch mipmapLevel:0 withBytes:upload bytesPerRow:4 * bpp];
    if (!readonly) for (NSUInteger y = 2; y < 5; ++y)
        memset((uint8_t *)expected.mutableBytes + y * stride + 3 * bpp, 0x7B, 4 * bpp);
    check(!memcmp(base, expected.bytes, length), "CPU write changed wrong pixels or padding");
    NSMutableData *read = [NSMutableData dataWithLength:length];
    memset(read.mutableBytes, 0xCD, length);
    [texture getBytes:read.mutableBytes bytesPerRow:stride
        fromRegion:MTLRegionMake2D(0,0,texture.width,texture.height) mipmapLevel:0];
    for (NSUInteger y = 0; y < texture.height; ++y) {
        check(!memcmp((uint8_t *)read.bytes + y * stride, base + y * stride,
            texture.width * bpp), "CPU read differs");
        for (NSUInteger x = texture.width * bpp; x < stride; ++x)
            check(((uint8_t *)read.bytes)[y * stride + x] == 0xCD, "CPU read overwrote padding");
    }
    gpuRead(device, texture, base, stride, bpp);
}

static void exerciseIconWriteback(id<MTLDevice> device, id<MTLTexture> texture,
                                  IOSurfaceRef surface, NSUInteger stride) {
    const NSUInteger texelBytes = 8;
    const NSUInteger uploadStride = 2304;
    const NSUInteger uploadSize = uploadStride * texture.height;
    id<MTLBuffer> upload = [device newBufferWithLength:uploadSize options:MTLResourceStorageModeShared];
    check(upload && upload.contents, "icon upload allocation");
    uint8_t *uploadBytes = upload.contents;
    memset(uploadBytes, 0xFE, uploadSize);
    for (NSUInteger row = 0; row < texture.height; ++row) {
        for (NSUInteger column = 0; column < texture.width; ++column) {
            const uint16_t pixel[] = {0x3800 | (uint16_t)column, 0x3800 | (uint16_t)row,
                0x3800 | (uint16_t)((column >= 134) + 2 * (row >= 134)), 0x3C00};
            memcpy(uploadBytes + row * uploadStride + column * texelBytes, pixel, sizeof pixel);
        }
    }
    check(IOSurfaceLock(surface, 0, NULL) == 0, "icon surface lock");
    memset(IOSurfaceGetBaseAddress(surface), 0xA5, IOSurfaceGetAllocSize(surface));
    IOSurfaceUnlock(surface, 0, NULL);
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
    check(blit != nil, "icon upload encoder");
    [blit copyFromBuffer:upload sourceOffset:0 sourceBytesPerRow:uploadStride sourceBytesPerImage:uploadSize
              sourceSize:MTLSizeMake(texture.width,texture.height,1) toTexture:texture
        destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
    [blit endEncoding]; [command commit]; [command waitUntilCompleted];
    check(command.status == MTLCommandBufferStatusCompleted && !command.error, "icon write-back command failed");
    check(IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL) == 0, "icon read-back lock");
    const uint8_t *host = IOSurfaceGetBaseAddress(surface);
    for (NSUInteger row = 0; row < texture.height; ++row) {
        check(!memcmp(host + row * stride, uploadBytes + row * uploadStride, texture.width * texelBytes),
              "icon write-back pixels differ");
        for (NSUInteger column = texture.width * texelBytes; column < stride; ++column)
            check(host[row * stride + column] == 0xA5, "icon write-back overwrote row padding");
    }
    for (NSUInteger tail = stride * texture.height; tail < IOSurfaceGetAllocSize(surface); ++tail)
        check(host[tail] == 0xA5, "icon write-back overwrote allocation padding");
    IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
    printf("ICON_WRITEBACK_PASS geometry=268x268 stride=%lu rgba16f pixels=exact padding=preserved\n",
           (unsigned long)stride);
}

int main(int argc, const char **argv) { @autoreleasepool {
    if (argc != 3 || (strcmp(argv[2], "readonly") && strcmp(argv[2], "iosurface"))) return 2;
    alarm(45); setvbuf(stdout, NULL, _IONBF, 0);
    setenv("RDNA4M_ALLOW", "1", 1); setenv("RDNA4M_IOS_PATH", "b", 1);
    setenv("MVK_CONFIG_USE_MTLHEAP", "0", 1);
    NSBundle *bundle = [NSBundle bundleWithPath:@(argv[1])]; NSError *error = nil;
    check([bundle loadAndReturnError:&error], "bundle load");
    id<MTLDevice> device = [[objc_getClass("GarnetDevice") alloc] init];
    check(device != nil, "device allocation");
    BOOL readonly = !strcmp(argv[2], "readonly");
    if (readonly) {
        const size_t length = 65536;
        uint8_t *base = mmap(NULL,length,PROT_READ|PROT_WRITE,MAP_ANON|MAP_PRIVATE,-1,0);
        check(base != MAP_FAILED, "mapping");
        for (size_t i = 0; i < length; ++i) base[i] = (uint8_t)(i * 17 + (i >> 8) * 29 + 13);
        NSData *original = [NSData dataWithBytes:base length:length];
        check(mprotect(base,length,PROT_READ) == 0, "read-only protection");
        @autoreleasepool {
            id<MTLBuffer> buffer = [device newBufferWithBytesNoCopy:base length:length
                options:MTLResourceStorageModeShared deallocator:nil];
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
                MTLPixelFormatBGRA8Unorm width:64 height:8 mipmapped:NO];
            td.storageMode = MTLStorageModeShared; td.usage = MTLTextureUsageShaderRead;
            id<MTLTexture> root = [buffer newTextureWithDescriptor:td offset:256 bytesPerRow:256];
            check(root != nil, "buffer texture");
            id<MTLTexture> view = [root newTextureViewWithPixelFormat:MTLPixelFormatBGRA8Unorm];
            check(view != nil, "buffer texture view");
            exercise(device, root, base + 256, 256, 4, YES);
            exercise(device, view, base + 256, 256, 4, YES);
            check(!memcmp(base, original.bytes, length), "buffer contents or guards changed");
        }
        munmap(base,length);
        puts("TEXTURE_HOST_ACCESS_PASS mode=readonly roots=1 views=1 CPU_write=refused CPU_read=exact GPU_read=exact");
    } else {
        const struct { MTLPixelFormat pf; uint32_t fourcc; NSUInteger w,h,row,bpp; } formats[] = {
            {MTLPixelFormatBGRA8Unorm,0x42475241,1446,1250,5824,4},
            {MTLPixelFormatRGBA16Float,0x52476841,330,1507,2816,8},
            {MTLPixelFormatA8Unorm,0x41303038,1698,1248,1792,1},
            {MTLPixelFormatRGBA16Float,0x52476841,268,268,2144,8},
            {MTLPixelFormatRGBA16Float,0x52476841,268,268,2208,8}};
        const size_t formatCount = sizeof formats / sizeof formats[0];
        for (unsigned i = 0; i < formatCount; ++i) { @autoreleasepool {
            NSUInteger length = formats[i].row * formats[i].h;
            IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef)@{
                (id)kIOSurfaceWidth:@(formats[i].w), (id)kIOSurfaceHeight:@(formats[i].h),
                (id)kIOSurfaceBytesPerElement:@(formats[i].bpp), (id)kIOSurfaceBytesPerRow:@(formats[i].row),
                (id)kIOSurfaceAllocSize:@(length), (id)kIOSurfacePixelFormat:@(formats[i].fourcc)});
            check(surface != NULL, "IOSurface allocation");
            check(IOSurfaceGetWidth(surface) == formats[i].w && IOSurfaceGetHeight(surface) == formats[i].h &&
                  IOSurfaceGetBytesPerRow(surface) == formats[i].row && IOSurfaceGetAllocSize(surface) >= length,
                  "IOSurface geometry differs from fixture");
            IOSurfaceLock(surface,0,NULL); uint8_t *base = IOSurfaceGetBaseAddress(surface);
            memset(base,0x3C,length); IOSurfaceUnlock(surface,0,NULL);
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
                formats[i].pf width:formats[i].w height:formats[i].h mipmapped:NO];
            td.storageMode = MTLStorageModeManaged; td.usage = MTLTextureUsageShaderRead;
            id<MTLTexture> texture = [device newTextureWithDescriptor:td iosurface:surface plane:0];
            check(texture != nil, "IOSurface import");
            exercise(device,texture,base,formats[i].row,formats[i].bpp,NO);
            if (formats[i].w == 268) exerciseIconWriteback(device, texture, surface, formats[i].row);
            printf("TEXTURE_HOST_ACCESS_CASE_PASS format=%lu width=%lu height=%lu stride=%lu CPU_GPU=exact\n",
                (unsigned long)formats[i].pf,(unsigned long)formats[i].w,
                (unsigned long)formats[i].h,(unsigned long)formats[i].row);
            CFRelease(surface);
        } }
        printf("TEXTURE_HOST_ACCESS_PASS mode=iosurface formats=%zu CPU_GPU=exact padding=preserved\n", formatCount);
    }
    _exit(0);
} }
