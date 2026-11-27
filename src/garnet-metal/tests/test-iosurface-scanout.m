#ifdef RDNA4_IOS_SCANOUT_BUNDLE
#include "GarnetDevice.m"

@interface RDNA4Texture (ScanoutAliasTest)
- (VkDeviceMemory)scanoutAliasTestMemory;
@end
@implementation RDNA4Texture (ScanoutAliasTest)
- (VkDeviceMemory)scanoutAliasTestMemory { return _imem; }
@end

static PFN_vkCmdPipelineBarrier scanout_real_barrier;
static VkImage scanout_discard_image;
static VkBuffer scanout_discard_alias;
static unsigned scanout_discard_count;
static unsigned scanout_host_barriers;
static PFN_vkAllocateMemory scanout_real_allocate;
static unsigned scanout_snapshot_allocations;
static unsigned scanout_visible_allocations;
static VkResult VKAPI_CALL scanout_snapshot_allocate(VkDevice device, const VkMemoryAllocateInfo *info,
    const VkAllocationCallbacks *allocator, VkDeviceMemory *memory) {
    ++scanout_snapshot_allocations;
    if (RDNA4R.mp.memoryTypes[info->memoryTypeIndex].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        ++scanout_visible_allocations;
        *memory = VK_NULL_HANDLE;
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    return scanout_real_allocate(device, info, allocator, memory);
}
static void scanout_discard_barrier(VkCommandBuffer cmd, VkPipelineStageFlags src, VkPipelineStageFlags dst,
    VkDependencyFlags flags, uint32_t memoryCount, const VkMemoryBarrier *memory,
    uint32_t bufferCount, const VkBufferMemoryBarrier *buffers, uint32_t imageCount, const VkImageMemoryBarrier *images) {
    for (uint32_t i = 0; i < memoryCount; ++i)
        if ((dst & VK_PIPELINE_STAGE_HOST_BIT) && (memory[i].dstAccessMask & VK_ACCESS_HOST_READ_BIT)) ++scanout_host_barriers;
    for (uint32_t i = 0; i < imageCount; ++i) {
        if (images[i].image == scanout_discard_image && images[i].oldLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
            // Model a permitted UNDEFINED-layout discard at the Vulkan boundary, after any production snapshot.
            vkCmdFillBuffer(cmd, scanout_discard_alias, 0, 16384, 0xA5A5A5A5);
            VkMemoryBarrier visible = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
            scanout_real_barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                0, 1, &visible, 0, NULL, 0, NULL);
            ++scanout_discard_count;
        }
    }
    scanout_real_barrier(cmd, src, dst, flags, memoryCount, memory, bufferCount, buffers, imageCount, images);
}

static void scanout_test_fail(const char *why) __attribute__((noreturn));
static void scanout_test_fail(const char *why) {
    fprintf(stderr, "IOS_SCANOUT_FAIL %s\n", why);
    _exit(1);
}
static void scanout_test_run(id<MTLCommandBuffer> commands) {
    [commands commit];
    [commands waitUntilCompleted];
    if (commands.status != MTLCommandBufferStatusCompleted || commands.error)
        scanout_test_fail("command buffer failed");
}
static void scanout_test_pixels(const void *bytes, const uint8_t expected[4], const char *stage) {
    for (NSUInteger pixel = 0; pixel < 64 * 64; ++pixel)
        if (memcmp((const uint8_t *)bytes + pixel * 4, expected, 4)) scanout_test_fail(stage);
    printf("IOS_SCANOUT_PIXELS_PASS stage=%s pixels=4096 exact\n", stage);
}

int rdna4_test_iosurface_scanout(void) {
    id<MTLDevice> device = (id)[[GarnetDevice alloc] init];
    ((void (*)(id, SEL))objc_msgSend)(device, sel_registerName("initLimits"));
    id<MTLCommandQueue> queue = [device newCommandQueue];
    IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef)@{
        (id)kIOSurfaceWidth: @64, (id)kIOSurfaceHeight: @64,
        (id)kIOSurfaceBytesPerElement: @4, (id)kIOSurfaceBytesPerRow: @256,
        (id)kIOSurfaceAllocSize: @16384, (id)kIOSurfacePixelFormat: @((uint32_t)0x42475241) });
    if (!device || !queue || !surface) scanout_test_fail("surface/queue allocation");
    const uint8_t host[4] = { 17, 51, 119, 255 }, drawn[4] = { 160, 96, 32, 255 };
    IOSurfaceLock(surface, 0, NULL);
    for (NSUInteger pixel = 0; pixel < 64 * 64; ++pixel)
        memcpy((uint8_t *)IOSurfaceGetBaseAddress(surface) + pixel * 4, host, 4);
    IOSurfaceUnlock(surface, 0, NULL);
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
        MTLPixelFormatBGRA8Unorm width:64 height:64 mipmapped:NO];
    descriptor.storageMode = MTLStorageModeManaged;
    descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    RDNA4Texture *texture = (id)[device newTextureWithDescriptor:descriptor iosurface:surface plane:0];
    if (!texture || ![texture rdna4IOSLinear]) scanout_test_fail("linear import required; fallback is not coverage");
    RDNA4Buffer *slot = (id)[device newBufferWithLength:16384 options:MTLResourceStorageModeShared];
    if (!slot || ![slot contents]) scanout_test_fail("slot allocation");
    VkBuffer alias = VK_NULL_HANDLE;
    VkExternalMemoryBufferCreateInfo external = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT };
    VkBufferCreateInfo bufferInfo = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &external,
        .size = 16384, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    if (vkCreateBuffer(RDNA4R.dev, &bufferInfo, NULL, &alias) != VK_SUCCESS ||
        vkBindBufferMemory(RDNA4R.dev, alias, [texture scanoutAliasTestMemory], 0) != VK_SUCCESS)
        scanout_test_fail("scanout source alias allocation");
    scanout_discard_image = [texture vkImage];
    scanout_discard_alias = alias;
    scanout_real_barrier = vkCmdPipelineBarrier;
    vkCmdPipelineBarrier = scanout_discard_barrier;
    memset([slot contents], 0xA5, 16384);
    MTLTextureDescriptor *privateDescriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
        MTLPixelFormatBGRA8Unorm width:64 height:64 mipmapped:NO];
    privateDescriptor.storageMode = MTLStorageModePrivate;
    id<MTLTexture> privateCopy = [device newTextureWithDescriptor:privateDescriptor];
    if (!privateCopy) scanout_test_fail("private read destination allocation");
    BOOL pooled = RDNA4P.on;
    RDNA4P.on = NO;
    scanout_real_allocate = vkAllocateMemory;
    vkAllocateMemory = scanout_snapshot_allocate;
    @autoreleasepool {
        id<MTLCommandBuffer> abandoned = [queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [abandoned blitCommandEncoder];
        if (!blit) scanout_test_fail("abandoned blit encoder allocation");
        [blit copyFromTexture:(id)texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
            sourceSize:MTLSizeMake(64,64,1) toTexture:privateCopy destinationSlice:0
            destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
        [blit endEncoding];
    }
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
    if (!blit) scanout_test_fail("first submitted blit encoder allocation");
    [blit copyFromTexture:(id)texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
        sourceSize:MTLSizeMake(64,64,1) toTexture:privateCopy destinationSlice:0
        destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
    [blit endEncoding];
    if (scanout_snapshot_allocations || scanout_discard_count)
        scanout_test_fail("encoding changed the linear image before submission");
    scanout_test_run(commands);
    vkAllocateMemory = scanout_real_allocate;
    RDNA4P.on = pooled;
    if (scanout_visible_allocations || scanout_snapshot_allocations != 1)
        scanout_test_fail("first-use snapshot must survive visible-pool exhaustion with one device-only allocation");
    if (scanout_discard_count != 1) scanout_test_fail("UNDEFINED discard boundary not exercised exactly once");
    vkCmdPipelineBarrier = scanout_real_barrier;
    IOSurfaceLock(surface, kIOSurfaceLockReadOnly | kIOSurfaceLockAvoidSync, NULL);
    scanout_test_pixels(IOSurfaceGetBaseAddress(surface), host, "host-first-read");
    IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly | kIOSurfaceLockAvoidSync, NULL);
    if (!scanout_host_barriers) scanout_test_fail("first-use image restoration has no host visibility barrier");
    VkImageLayout afterRead = [texture layout];
    commands = [queue commandBuffer];
    blit = [commands blitCommandEncoder];
    [blit copyFromTexture:privateCopy sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
        sourceSize:MTLSizeMake(64,64,1) toBuffer:(id)slot destinationOffset:0
        destinationBytesPerRow:256 destinationBytesPerImage:16384];
    [blit endEncoding];
    scanout_test_run(commands);
    scanout_test_pixels([slot contents], host, "initial-image-read");

    // Use the production image writeback and buffer-copy helper, without a display lease or present.
    RDNA4S.pw = RDNA4S.ph = 64;
    RDNA4S.pitch = 256;
    RDNA4S.buf[0] = [slot vkBuffer];
    memset([slot contents], 0xA5, 16384);
    commands = [queue commandBuffer];
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = (id)texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(32.0/255, 96.0/255, 160.0/255, 1);
    id<MTLRenderCommandEncoder> render = [commands renderCommandEncoderWithDescriptor:pass];
    if (!render) scanout_test_fail("render encoder");
    [render endEncoding];
    VkCommandBuffer vk = [(RDNA4CommandBuffer *)commands vk];
    rdna4_ios_writeback(vk, texture);
    VkImageLayout beforeAliasRead = [texture layout];
    rdna4df_plan_t plan = { .slot = 0, .base = -1, .inst = 0 };
    rdna4_full_barrier(vk);
    rdna4s_record_copy(vk, alias, [texture rdna4IOSOffset], &plan);
    rdna4_full_barrier(vk);
    scanout_test_run(commands);
    scanout_test_pixels([slot contents], drawn, "render-to-scanout-buffer-alias");
    IOSurfaceLock(surface, kIOSurfaceLockReadOnly | kIOSurfaceLockAvoidSync, NULL);
    scanout_test_pixels(IOSurfaceGetBaseAddress(surface), drawn, "render-to-host-alias");
    IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly | kIOSurfaceLockAvoidSync, NULL);
    vkDestroyBuffer(RDNA4R.dev, alias, NULL);
    RDNA4S.buf[0] = VK_NULL_HANDLE;
    CFRelease(surface);
    if (afterRead != VK_IMAGE_LAYOUT_GENERAL || beforeAliasRead != VK_IMAGE_LAYOUT_GENERAL) {
        fprintf(stderr, "IOS_SCANOUT_LAYOUT_FAIL after-read=%d before-alias-read=%d expected=1,1\n",
                afterRead, beforeAliasRead);
        return 1;
    }
    puts("IOS_SCANOUT_PASS linear=1 layouts=1,1 abandoned_encoding=1 forced_initial_discard=1 visible_pool_exhausted=1 host_visibility=1 native_display_transport=untested");
    return 0;
}
#else
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <unistd.h>

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        if (argc == 2 && !strcmp(argv[1], "--help")) {
            puts("Usage: sudo test-iosurface-scanout TEST_BUNDLE\n"
                 "Requires the test-only bundle built from this file with RDNA4_IOS_SCANOUT_BUNDLE. "
                 "Checks production IOSurface writeback and scanout-copy helpers without a display lease.");
            return 0;
        }
        if (argc != 2 || argv[1][0] == '-' || geteuid() != 0) return 2;
        alarm(60);
        setvbuf(stdout, NULL, _IONBF, 0);
        setenv("RDNA4M_ALLOW", "1", 1);
        setenv("MVK_CONFIG_LOG_LEVEL", "2", 1);
        setenv("MVK_CONFIG_USE_MTLHEAP", "0", 1);
        NSBundle *bundle = [NSBundle bundleWithPath:@(argv[1])];
        NSError *error = nil;
        if (![bundle loadAndReturnError:&error]) {
            fprintf(stderr, "IOS_SCANOUT_FAIL load: %s\n", error.description.UTF8String);
            return 1;
        }
        void *handle = dlopen(bundle.executablePath.UTF8String, RTLD_NOW | RTLD_LOCAL);
        int (*run)(void) = handle ? (int (*)(void))dlsym(handle, "rdna4_test_iosurface_scanout") : NULL;
        if (!run) { fputs("IOS_SCANOUT_FAIL test entry missing\n", stderr); return 1; }
        _exit(run());
    }
}
#endif
