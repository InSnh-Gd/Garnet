// Capture the actual production encoder's Vulkan calls; no GPU is opened.
#include "GarnetDevice.m"

static unsigned copies, failures, cases;
static VkBufferImageCopy captured;
@interface LayoutCommand : RDNA4CommandBuffer
@property NSError *failure;
@property unsigned intentCount;
@end
@implementation LayoutCommand
- (void)rdna4Fail:(NSString *)reason { _failure = rdna4_err(1, reason); }
- (NSError *)rdna4Error { return _failure; }
- (void)rdna4RetainArgument:(id)object { (void)object; }
- (void)rdna4ArgumentIntent:(id)intent { (void)intent; ++_intentCount; }
- (VkCommandBuffer)vk { return (VkCommandBuffer)(uintptr_t)1; }
@end

@interface LayoutBuffer : RDNA4Buffer
@property NSUInteger extent, bindingOffset;
@end
@implementation LayoutBuffer
- (NSUInteger)length { return _extent; }
- (VkDeviceSize)rdna4VkOffset { return _bindingOffset; }
- (VkBuffer)vkBuffer { return (VkBuffer)(uintptr_t)2; }
@end

@interface RDNA4Texture (BlitLayoutFixture)
- (void)configureLayout3D:(BOOL)is3D;
@end
@implementation RDNA4Texture (BlitLayoutFixture)
- (void)configureLayout3D:(BOOL)is3D {
    _fmt = rdna4_fmt(MTLPixelFormatRGBA8Unorm); _pf = MTLPixelFormatRGBA8Unorm;
    _w = 4; _h = 3; _depth = is3D ? 2 : 1; _levels = 1;
    _tdk = is3D ? RDNA4TD_K_3D : RDNA4TD_K_2D; _is3D = is3D;
    _layout = VK_IMAGE_LAYOUT_GENERAL;
}
@end
@interface RDNA4BlitEncoder (BlitLayoutFixture)
- (void)configureLayoutCommand:(LayoutCommand *)command ended:(BOOL)ended;
@end
@implementation RDNA4BlitEncoder (BlitLayoutFixture)
- (void)configureLayoutCommand:(LayoutCommand *)command ended:(BOOL)ended {
    _cb = command; _ended = ended;
}
@end

static void VKAPI_CALL barrier(VkCommandBuffer command, VkPipelineStageFlags src,
    VkPipelineStageFlags dst, VkDependencyFlags flags, uint32_t nm, const VkMemoryBarrier *m,
    uint32_t nb, const VkBufferMemoryBarrier *b, uint32_t ni, const VkImageMemoryBarrier *i) {
    (void)command; (void)src; (void)dst; (void)flags; (void)nm; (void)m;
    (void)nb; (void)b; (void)ni; (void)i;
}
static void VKAPI_CALL upload(VkCommandBuffer command, VkBuffer buffer, VkImage image,
    VkImageLayout layout, uint32_t count, const VkBufferImageCopy *copy) {
    (void)command; (void)buffer; (void)image; (void)layout; (void)count;
    ++copies; captured = *copy;
}
static void VKAPI_CALL download(VkCommandBuffer command, VkImage image, VkImageLayout layout,
    VkBuffer buffer, uint32_t count, const VkBufferImageCopy *copy) {
    upload(command, buffer, image, layout, count, copy);
}

int main(void) { @autoreleasepool {
    vkCmdPipelineBarrier = barrier; vkCmdCopyBufferToImage = upload; vkCmdCopyImageToBuffer = download;
    const char *names[] = {"short-buffer", "past-offset", "short-row", "fractional-row",
        "truncated-row", "short-image", "fractional-image", "vkoffset-wrap", "ended", "prior-error",
        "tight-control", "padding-control", "default-row-control"};
    for (unsigned direction = 0; direction < 2; ++direction) for (unsigned mode = 0; mode < 13; ++mode) {
        copies = 0; captured = (VkBufferImageCopy){0};
        LayoutCommand *command = [LayoutCommand new];
        LayoutBuffer *buffer = [LayoutBuffer new]; buffer.extent = 4096;
        RDNA4Texture *texture = [RDNA4Texture new];
        BOOL is3D = mode == 5 || mode == 6 || mode == 11 || mode == 12;
        [texture configureLayout3D:is3D];
        RDNA4BlitEncoder *encoder = [RDNA4BlitEncoder new];
        [encoder configureLayoutCommand:command ended:mode == 8];
        if (mode == 9) [command rdna4Fail:@"earlier failure"];
        NSUInteger offset = 16, row = 16, image = is3D ? 48 : 0;
        if (mode == 0) buffer.extent = 32;
        if (mode == 1) offset = 4096;
        if (mode == 2) row = 12;
        if (mode == 3) row = 17;
        if (mode == 4) row = ((NSUInteger)UINT32_MAX + 1) * 4;
        if (mode == 5) image = 16;
        if (mode == 6) image = 49;
        if (mode == 7) buffer.bindingOffset = NSUIntegerMax - 15;
        if (mode == 11) row = 32, image = 128;
        if (mode == 12) row = 0, image = 128;
        MTLSize size = MTLSizeMake(4, 3, is3D ? 2 : 1);
        if (!direction) [encoder copyFromBuffer:buffer sourceOffset:offset sourceBytesPerRow:row sourceBytesPerImage:image sourceSize:size
            toTexture:texture destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
        else [encoder copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:size
            toBuffer:buffer destinationOffset:offset destinationBytesPerRow:row destinationBytesPerImage:image];
        BOOL control = mode >= 10;
        BOOL ok = control ? copies == 1 && !command.failure : !copies && command.failure != nil;
        ok = ok && command.intentCount == (control && direction ? 1u : 0u);
        if (control) ok = ok && captured.bufferOffset == offset &&
            captured.bufferRowLength == (mode == 11 ? 8u : 4u) &&
            captured.bufferImageHeight == (mode == 11 ? 4u : mode == 12 ? 8u : 0u);
        ++cases;
        printf("BLIT_BUFFER_LAYOUT_CASE_%s direction=%s case=%s copies=%u\n",
            ok ? "PASS" : "FAIL", direction ? "download" : "upload", names[mode], copies);
        if (!ok) ++failures;
    }
    printf("BLIT_BUFFER_LAYOUT cases=%u failures=%u simulated_vulkan=1 gpu_execution=0\n", cases, failures);
    return failures ? 1 : 0;
} }
