#include "GarnetDevice.m"

static unsigned copies, ends;
static char recorded[32];
static size_t recordedLength;

static void require(BOOL condition, const char *message) {
    if (!condition) { fprintf(stderr, "NOCOPY_WRITEBACK_FAIL %s\n", message); exit(1); }
}

static void record(char operation) {
    require(recordedLength + 1 < sizeof recorded, "recording overflow");
    recorded[recordedLength++] = operation;
    recorded[recordedLength] = 0;
}

@interface WritebackBuffer : RDNA4Buffer
@end
@implementation WritebackBuffer
- (VkBuffer)vkBuffer { return (VkBuffer)(uintptr_t)2; }
@end

@interface RDNA4Texture (WritebackFixture)
- (void)configureWritebackBuffer:(RDNA4Buffer *)buffer memory:(void *)memory display:(BOOL)display;
- (void)configureIconSurface:(IOSurfaceRef)surface;
- (void)configureIconAnchor;
- (void)configureA8Buffer:(RDNA4Buffer *)buffer memory:(void *)memory width:(size_t)width height:(size_t)height;
- (void)configureLinearSurface;
- (void)configureMenuSurface:(IOSurfaceRef)surface memory:(void *)base image:(VkImage)image;
@end
@implementation RDNA4Texture (WritebackFixture)
- (void)configureWritebackBuffer:(RDNA4Buffer *)buffer memory:(void *)memory display:(BOOL)display {
    _hostBuf = buffer; _ibase = memory; _ibpr = 15360;
    _fmt = rdna4_fmt(MTLPixelFormatBGRA8Unorm); _pf = MTLPixelFormatBGRA8Unorm;
    _w = 3840; _h = 2160; _depth = 1; _levels = 1; _samples = 1;
    _tdk = RDNA4TD_K_2D; _layout = VK_IMAGE_LAYOUT_GENERAL; _disp = display;
}
- (void)configureIconSurface:(IOSurfaceRef)surface {
    _ios = (IOSurfaceRef)CFRetain(surface);
    _ibase = IOSurfaceGetBaseAddress(surface); _ibpr = IOSurfaceGetBytesPerRow(surface);
    _fmt = rdna4_fmt(MTLPixelFormatRGBA16Float); _pf = MTLPixelFormatRGBA16Float;
    _w = IOSurfaceGetWidth(surface); _h = IOSurfaceGetHeight(surface);
    _depth = 1; _levels = 1; _samples = 1; _tdk = RDNA4TD_K_2D;
    _ibuf = (VkBuffer)(uintptr_t)2; _img = (VkImage)(uintptr_t)3;
}
- (void)configureIconAnchor {
    _fmt = rdna4_fmt(MTLPixelFormatRG16Float); _pf = MTLPixelFormatRG16Float;
    _w = 46; _h = 74; _depth = 1; _levels = 1; _samples = 1; _tdk = RDNA4TD_K_2D;
}
- (void)configureA8Buffer:(RDNA4Buffer *)buffer memory:(void *)memory width:(size_t)width height:(size_t)height {
    _hostBuf = buffer; _hoff = 256; _ibase = (uint8_t *)memory + _hoff; _ibpr = 256;
    _fmt = rdna4_fmt(MTLPixelFormatA8Unorm); _pf = MTLPixelFormatA8Unorm;
    _w = width; _h = height; _depth = 1; _levels = 1; _samples = 1;
    _tdk = RDNA4TD_K_2D; _img = (VkImage)(uintptr_t)3;
}
- (void)configureLinearSurface { _iosLinear = YES; }
- (void)configureMenuSurface:(IOSurfaceRef)surface memory:(void *)base image:(VkImage)image {
    _ios = (IOSurfaceRef)CFRetain(surface);
    _ibase = base; _ibpr = 15360; _fmt = rdna4_fmt(MTLPixelFormatBGRA8Unorm); _pf = MTLPixelFormatBGRA8Unorm;
    _w = 3840; _h = 60; _depth = 1; _levels = 1; _samples = 1; _tdk = RDNA4TD_K_2D;
    _ibuf = (VkBuffer)base; _img = image; _iosLinear = YES;
}
@end

@interface RDNA4CommandBuffer (WritebackFixture)
- (void)configureWritebackFixture;
- (NSUInteger)writebackSegmentCount;
- (void)retireWritebackFixture;
- (void)setTraceFixtureID:(uint64_t)identifier;
- (BOOL)hasIncompleteDisplayInput;
- (BOOL)defersScheduledPublication;
@end
@implementation RDNA4CommandBuffer (WritebackFixture)
- (void)configureWritebackFixture {
    pthread_mutex_init(&_smu, NULL);
    _dslot = -1; _vk = (VkCommandBuffer)(uintptr_t)1;
    _refs = [NSMutableArray array]; _iot = [NSMutableArray array]; _iow = [NSMutableArray array];
    _curSegment = [RDNA4CommandSegment new]; _curSegment->vk = _vk;
    _segments = [NSMutableArray arrayWithObject:_curSegment];
}
- (NSUInteger)writebackSegmentCount { return _segments.count; }
- (void)setTraceFixtureID:(uint64_t)identifier { _cbid = identifier; }
- (BOOL)hasIncompleteDisplayInput { return _displayIncomplete; }
- (BOOL)defersScheduledPublication { return _scheduleAfterPublish; }
- (void)retireWritebackFixture {
    RDNA4CommandSegment *segment = _segments[0];
    segment->vk = VK_NULL_HANDLE;
    segment->submitted = YES;
    segment->done = YES;
    [self rdna4SignalSegment:segment];
}
@end

static void VKAPI_CALL barrier(VkCommandBuffer command, VkPipelineStageFlags source,
    VkPipelineStageFlags destination, VkDependencyFlags flags, uint32_t memoryCount,
    const VkMemoryBarrier *memory, uint32_t bufferCount, const VkBufferMemoryBarrier *buffers,
    uint32_t imageCount, const VkImageMemoryBarrier *images) {
    (void)command; (void)source; (void)flags; (void)memoryCount; (void)memory;
    (void)bufferCount; (void)buffers; (void)imageCount; (void)images;
    record(destination == VK_PIPELINE_STAGE_HOST_BIT ? 'H' : 'B');
}

static void VKAPI_CALL download(VkCommandBuffer command, VkImage image, VkImageLayout layout,
    VkBuffer buffer, uint32_t count, const VkBufferImageCopy *regions) {
    (void)command; (void)image;
    require(layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && count == 1 &&
        buffer == (VkBuffer)(uintptr_t)2 && regions[0].bufferOffset == 0 &&
        regions[0].bufferRowLength == 3840 && regions[0].imageExtent.width == 3840 &&
        regions[0].imageExtent.height == 2160, "write-back changed desktop row geometry");
    ++copies; record('W');
}

static VkResult VKAPI_CALL endCommand(VkCommandBuffer command) {
    (void)command; ++ends; record('E'); return VK_SUCCESS;
}

static VkResult VKAPI_CALL allocateCommands(VkDevice device, const VkCommandBufferAllocateInfo *info,
    VkCommandBuffer *command) {
    (void)device; (void)info; *command = (VkCommandBuffer)(uintptr_t)3;
    record('A'); return VK_SUCCESS;
}

static VkResult VKAPI_CALL beginCommand(VkCommandBuffer command, const VkCommandBufferBeginInfo *info) {
    (void)command; (void)info; return VK_SUCCESS;
}

@interface DropTraceCommand : RDNA4CommandBuffer
@end
@implementation DropTraceCommand
- (void)rdna4EnsureScheduledWithError:(NSError *)error { require(error != nil, "drop was scheduled without an error"); }
@end

@interface DropTraceQueue : NSObject
@property(nonatomic, strong) dispatch_queue_t rdna4CompletionQueue;
@property(nonatomic, strong) NSError *terminalError;
@end
@implementation DropTraceQueue
- (void)commandBufferDidComplete:(id)command startTime:(uint64_t)start completionTime:(uint64_t)end error:(NSError *)error {
    (void)command; (void)start; (void)end;
    self.terminalError = error;
}
- (void)rdna4DidComplete:(id)command { (void)command; }
@end

static VkResult VKAPI_CALL rejectFence(VkDevice device, const VkFenceCreateInfo *info,
        const VkAllocationCallbacks *allocator, VkFence *fence) {
    (void)device; (void)info; (void)allocator; (void)fence;
    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
}

static int dropTrace(BOOL encode) {
    int diagnostic[2];
    require(pipe(diagnostic) == 0, "create drop trace sink");
    RDNA4CB.fd = diagnostic[1];
    atomic_store(&RDNA4CB.on, 1);
    vkEndCommandBuffer = endCommand;
    vkCmdPipelineBarrier = barrier;
    vkCreateFence = rejectFence;
    DropTraceQueue *queue = [DropTraceQueue new];
    queue.rdna4CompletionQueue = dispatch_queue_create("org.garnet.test.drop-trace", DISPATCH_QUEUE_SERIAL);
    DropTraceCommand *command = [DropTraceCommand new];
    [command configureWritebackFixture];
    [command setTraceFixtureID:131267];
    if (encode) [command rdna4Fail:@"allocation was refused while encoding"];
    [command rdna4PrepareForSubmission];
    (void)[command rdna4AdvanceWithQueue:(id)queue];
    dispatch_sync(queue.rdna4CompletionQueue, ^{});
    require(queue.terminalError.code == (encode ? 66 : 52), "wrong pre-submit failure");
    atomic_store(&RDNA4CB.on, 0);
    RDNA4CB.fd = -1;
    close(diagnostic[1]);
    char trace[512] = {0};
    require(read(diagnostic[0], trace, sizeof trace - 1) > 0, "read drop trace");
    close(diagnostic[0]);
    printf("DROP_TRACE %s", trace);
    require(!strncmp(trace, "D ", 2) && strstr(trace, encode ? " cb=131267 phase=encode code=66 vk=-13\n" :
        " cb=131267 phase=pre-submit code=52 vk=-2\n") && !strstr(trace, "\nS ") && !strstr(trace, "\nF ") && !strstr(trace, "\nW "),
        "pre-submit failure trace omitted its phase or reported GPU work");
    return 0;
}

static size_t iconWidth = 268, iconHeight = 268;
static const size_t iconPixelBytes = 8;
static uint8_t *iconHost, *iconImage;
static size_t iconPitch;

static void iconTransfer(BOOL upload, const VkBufferImageCopy *region, uint32_t count) {
    require(count == 1 && region->bufferOffset == 0 &&
        region->bufferRowLength * iconPixelBytes == iconPitch &&
        region->imageExtent.width == iconWidth && region->imageExtent.height == iconHeight &&
        region->imageExtent.depth == 1, "icon transfer changed IOSurface geometry");
    const size_t rowBytes = iconWidth * iconPixelBytes;
    for (size_t row = 0; row < iconHeight; ++row) {
        uint8_t *host = iconHost + row * iconPitch;
        uint8_t *image = iconImage + row * rowBytes;
        if (upload) memcpy(image, host, rowBytes);
        else memcpy(host, image, rowBytes);
    }
    record(upload ? 'U' : 'W');
}

static void VKAPI_CALL iconUpload(VkCommandBuffer command, VkBuffer buffer, VkImage image,
        VkImageLayout layout, uint32_t count, const VkBufferImageCopy *regions) {
    (void)command;
    require(buffer == (VkBuffer)(uintptr_t)2 && image == (VkImage)(uintptr_t)3 &&
        layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, "invalid icon upload source or destination");
    iconTransfer(YES, regions, count);
}

static void VKAPI_CALL iconDownload(VkCommandBuffer command, VkImage image, VkImageLayout layout,
        VkBuffer buffer, uint32_t count, const VkBufferImageCopy *regions) {
    (void)command;
    require(buffer == (VkBuffer)(uintptr_t)2 && image == (VkImage)(uintptr_t)3 &&
        layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, "invalid icon write-back source or destination");
    iconTransfer(NO, regions, count);
}

static VkAttachmentDescription iconAttachment;

static VkResult VKAPI_CALL iconPass(VkDevice device, const VkRenderPassCreateInfo *info,
        const VkAllocationCallbacks *allocator, VkRenderPass *pass) {
    (void)device; (void)allocator;
    require(info->attachmentCount == 1 || info->attachmentCount == 2, "unexpected icon render attachments");
    iconAttachment = info->pAttachments[info->attachmentCount - 1];
    *pass = (VkRenderPass)(uintptr_t)1;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL iconFramebuffer(VkDevice device, const VkFramebufferCreateInfo *info,
        const VkAllocationCallbacks *allocator, VkFramebuffer *framebuffer) {
    (void)device; (void)info; (void)allocator;
    *framebuffer = (VkFramebuffer)(uintptr_t)1;
    return VK_SUCCESS;
}

static void VKAPI_CALL iconBeginClear(VkCommandBuffer command, const VkRenderPassBeginInfo *info,
        VkSubpassContents contents) {
    (void)command; (void)contents;
    require(iconAttachment.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR && iconAttachment.storeOp == VK_ATTACHMENT_STORE_OP_STORE,
        "icon clear no longer clears and stores the attachment");
    require(!info->renderArea.offset.x && !info->renderArea.offset.y &&
        info->renderArea.extent.width <= iconWidth && info->renderArea.extent.height <= iconHeight,
        "icon clear render area invalid");
    for (size_t row = 0; row < info->renderArea.extent.height; ++row)
        memset(iconImage + row * iconWidth * iconPixelBytes, 0, info->renderArea.extent.width * iconPixelBytes);
}

static void VKAPI_CALL iconEndClear(VkCommandBuffer command) { (void)command; }

static int iconAttachmentClear(BOOL partial) {
    iconWidth = 64; iconHeight = 128; iconPitch = 512;
    IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef)@{
        (id)kIOSurfaceWidth: @64, (id)kIOSurfaceHeight: @128,
        (id)kIOSurfaceBytesPerElement: @8, (id)kIOSurfaceBytesPerRow: @512,
    });
    require(surface && IOSurfaceLock(surface, 0, NULL) == kIOReturnSuccess, "lock icon attachment backing");
    iconHost = IOSurfaceGetBaseAddress(surface);
    iconImage = malloc(iconPitch * iconHeight);
    require(iconHost && iconImage && IOSurfaceGetBytesPerRow(surface) == iconPitch, "allocate icon attachment backing");
    const uint8_t preserved[8] = {0x00, 0x34, 0x00, 0x38, 0x00, 0x3a, 0x00, 0x3c}, cleared[8] = {0};
    for (size_t pixel = 0; pixel < iconWidth * iconHeight; ++pixel)
        memcpy(iconHost + pixel * iconPixelBytes, preserved, sizeof preserved);
    memset(iconImage, 0x55, iconPitch * iconHeight);
    RDNA4Texture *texture = [RDNA4Texture new], *anchor = [RDNA4Texture new];
    [texture configureIconSurface:surface];
    [anchor configureIconAnchor];
    RDNA4CommandBuffer *command = [RDNA4CommandBuffer new];
    [command configureWritebackFixture];
    vkCmdPipelineBarrier = barrier; vkCmdCopyBufferToImage = iconUpload; vkCmdCopyImageToBuffer = iconDownload;
    vkCreateRenderPass = iconPass; vkCreateFramebuffer = iconFramebuffer;
    vkCmdBeginRenderPass = iconBeginClear; vkCmdEndRenderPass = iconEndClear;
    vkEndCommandBuffer = endCommand;
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    if (partial) {
        pass.colorAttachments[0].texture = (id<MTLTexture>)anchor;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    }
    MTLRenderPassColorAttachmentDescriptor *attachment = pass.colorAttachments[partial ? 1 : 0];
    attachment.texture = (id<MTLTexture>)texture;
    attachment.loadAction = MTLLoadActionClear;
    attachment.storeAction = MTLStoreActionStore;
    RDNA4RenderEncoder *encoder = [[RDNA4RenderEncoder alloc] initWithCommandBuffer:command descriptor:pass];
    require(encoder != nil, "create icon attachment clear encoder");
    [encoder endEncoding];
    NSError *error = nil;
    require([command rdna4Finish:&error] && !error, "finish icon attachment clear and write-back");
    for (size_t row = 0; row < iconHeight; ++row)
        for (size_t col = 0; col < iconWidth; ++col) {
            BOOL inside = !partial || (row < 74 && col < 46);
            require(!memcmp(iconHost + (row * iconWidth + col) * iconPixelBytes, inside ? cleared : preserved, sizeof cleared),
                inside ? "icon render clear missed pixels" : "partial icon attachment clear overwrote untouched IOSurface pixels");
        }
    require(partial ? iconAttachment.initialLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && strchr(recorded, 'U')
                    : iconAttachment.initialLayout == VK_IMAGE_LAYOUT_UNDEFINED && !strchr(recorded, 'U'),
        "icon attachment clear lost preservation or added an unnecessary upload");
    free(iconImage);
    IOSurfaceUnlock(surface, 0, NULL);
    CFRelease(surface);
    printf("ICON_ATTACHMENT_CLEAR_PASS pixels=%zu partial=%d simulated_vulkan=1\n", iconWidth * iconHeight, partial);
    return 0;
}

static int iconRoundTrip(size_t pitch) {
    const size_t rowBytes = iconWidth * iconPixelBytes;
    const size_t allocSize = pitch * iconHeight + 128;
    NSDictionary *props = @{
        (__bridge NSString *)kIOSurfaceWidth: @(iconWidth),
        (__bridge NSString *)kIOSurfaceHeight: @(iconHeight),
        (__bridge NSString *)kIOSurfaceBytesPerElement: @(iconPixelBytes),
        (__bridge NSString *)kIOSurfaceBytesPerRow: @(pitch),
        (__bridge NSString *)kIOSurfaceAllocSize: @(allocSize),
    };
    IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef)props);
    require(surface && IOSurfaceGetBytesPerRow(surface) == pitch &&
        IOSurfaceGetAllocSize(surface) >= allocSize, "allocate IOSurface with logged or padded stride");
    require(IOSurfaceLock(surface, 0, NULL) == kIOReturnSuccess, "lock icon surface");
    iconPitch = pitch; iconHost = IOSurfaceGetBaseAddress(surface);
    iconImage = malloc(rowBytes * iconHeight);
    require(iconHost && iconImage, "allocate icon backing");
    memset(iconHost, 0x6d, allocSize);
    for (size_t row = 0; row < iconHeight; ++row) {
        memset(iconHost + row * pitch, row < iconHeight / 2 ? 0x11 : 0x33, rowBytes / 2);
        memset(iconHost + row * pitch + rowBytes / 2, row < iconHeight / 2 ? 0x22 : 0x44, rowBytes / 2);
    }
    RDNA4Texture *texture = [RDNA4Texture new];
    [texture configureIconSurface:surface];
    RDNA4CommandBuffer *command = [RDNA4CommandBuffer new];
    [command configureWritebackFixture];
    vkCmdPipelineBarrier = barrier; vkCmdCopyBufferToImage = iconUpload; vkCmdCopyImageToBuffer = iconDownload;
    rdna4_ios_upload(command, texture);
    for (size_t row = 0; row < iconHeight; ++row)
        for (size_t col = 0; col < rowBytes; ++col) {
            uint8_t expected = (row < iconHeight / 2 ? 0x11 : 0x33) + (col < rowBytes / 2 ? 0 : 0x11);
            require(iconImage[row * rowBytes + col] == expected, "icon upload scrambled a quadrant");
            iconImage[row * rowBytes + col] = expected ^ 0xff;
        }
    require(rdna4_ios_writeback([command vk], texture), "icon write-back refused");
    require(strstr(recorded, "U") && strstr(recorded, "WH"), "icon upload/write-back or host barrier missing");
    for (size_t row = 0; row < iconHeight; ++row) {
        require(!memcmp(iconHost + row * pitch, iconImage + row * rowBytes, rowBytes), "icon write-back scrambled a quadrant");
        for (size_t col = rowBytes; col < pitch; ++col)
            require(iconHost[row * pitch + col] == 0x6d, "write-back changed row padding");
    }
    for (size_t index = pitch * iconHeight; index < allocSize; ++index)
        require(iconHost[index] == 0x6d, "write-back changed allocation tail");
    free(iconImage);
    IOSurfaceUnlock(surface, 0, NULL);
    CFRelease(surface);
    printf("ICON_COPY_PASS size=268x268 stride=%zu simulated_vulkan=1 gpu_execution=0\n", pitch);
    return 0;
}

static VkImageLayout a8ExecutedLayout;
static uint8_t a8Host[16384], a8Image[256 * 58];
static void VKAPI_CALL a8Barrier(VkCommandBuffer command, VkPipelineStageFlags source,
    VkPipelineStageFlags destination, VkDependencyFlags flags, uint32_t memoryCount,
    const VkMemoryBarrier *memory, uint32_t bufferCount, const VkBufferMemoryBarrier *buffers,
    uint32_t imageCount, const VkImageMemoryBarrier *images) {
    (void)command; (void)source; (void)destination; (void)flags; (void)memoryCount;
    (void)memory; (void)bufferCount; (void)buffers;
    for (uint32_t index = 0; index < imageCount; ++index) {
        require(images[index].oldLayout == VK_IMAGE_LAYOUT_UNDEFINED || images[index].oldLayout == a8ExecutedLayout,
            "A8 upload starts from the layout of an unsubmitted command");
        a8ExecutedLayout = images[index].newLayout;
    }
}

static void VKAPI_CALL a8Upload(VkCommandBuffer command, VkBuffer buffer, VkImage image,
        VkImageLayout layout, uint32_t count, const VkBufferImageCopy *regions) {
    (void)command;
    require(buffer == (VkBuffer)(uintptr_t)2 && image == (VkImage)(uintptr_t)3 && count == 1 &&
        regions[0].bufferOffset == 256 && regions[0].bufferRowLength == 256, "A8 upload changed backing offset or stride");
    require(regions[0].imageExtent.width <= 256 && regions[0].imageExtent.height <= 58 && regions[0].imageExtent.depth == 1,
        "A8 upload extent exceeds the fixture");
    require(layout == a8ExecutedLayout, "A8 upload omitted its transition after an unsubmitted command");
    for (uint32_t row = 0; row < regions[0].imageExtent.height; ++row)
        memcpy(a8Image + row * 256, a8Host + 256 + row * 256, regions[0].imageExtent.width);
}

static void VKAPI_CALL a8DiscardedUpload(VkCommandBuffer command, VkBuffer buffer, VkImage image,
        VkImageLayout layout, uint32_t count, const VkBufferImageCopy *regions) {
    (void)command; (void)buffer; (void)image; (void)layout; (void)count; (void)regions;
}

static int a8UploadAfterAbandonedCommand(BOOL sampled) {
    const size_t width = sampled ? 253 : 231, height = sampled ? 49 : 58;
    RDNA4Texture *texture = [RDNA4Texture new];
    [texture configureA8Buffer:[WritebackBuffer new] memory:a8Host width:width height:height];
    RDNA4CommandBuffer *abandoned = [RDNA4CommandBuffer new];
    [abandoned configureWritebackFixture];
    vkCmdPipelineBarrier = barrier;
    vkCmdCopyBufferToImage = a8DiscardedUpload;
    rdna4_ios_use(abandoned, texture, YES, NO);
    if (sampled) rdna4_tex_to([abandoned vk], texture, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    for (size_t row = 0; row < height; ++row)
        for (size_t col = 0; col < width; ++col)
            a8Host[256 + row * 256 + col] = (uint8_t)(39 * row + 77 * col);
    RDNA4CommandBuffer *submitted = [RDNA4CommandBuffer new];
    [submitted configureWritebackFixture];
    vkCmdPipelineBarrier = a8Barrier;
    vkCmdCopyBufferToImage = a8Upload;
    rdna4_ios_use(submitted, texture, YES, NO);
    rdna4_tex_to([submitted vk], texture, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    for (size_t row = 0; row < height; ++row)
        for (size_t col = 0; col < width; ++col)
            require(a8Image[row * 256 + col] == (uint8_t)(39 * row + 77 * col), "A8 upload pixels differ from CPU source");
    printf("A8_UPLOAD_PASS size=%zux%zu pixels=%zu simulated_vulkan=1\n", width, height, width * height);
    return 0;
}

@interface MenuCommand : RDNA4CommandBuffer
@end
static unsigned menuFailures;
@implementation MenuCommand
- (void)rdna4EnsureScheduledWithError:(NSError *)error { require(!error, "menu command failed to submit"); }
- (void)rdna4FenceWait:(RDNA4CommandSegment *)segment queue:(RDNA4CommandQueue *)queue {
    (void)segment; (void)queue;
}
- (void)rdna4DeliverTerminalFailure:(NSError *)error queue:(RDNA4CommandQueue *)queue
        unresolved:(BOOL)unresolved fault:(VkResult)fault {
    (void)queue; (void)fault;
    require(error != nil && !unresolved, "pre-submit snapshot allocation failure was not reported");
    menuFailures++;
}
@end

static uint8_t menuSurfaces[2][15360 * 60];
#define menuPixels (menuSurfaces[0])
static VkImageLayout menuLayouts[2];
static unsigned menuSnapshots, menuReads;
static int menuAllocationLimit = -1;
static NSMutableDictionary<NSValue *, NSMutableArray *> *menuCommands;

static size_t menuIndex(VkImage image) {
    const uintptr_t handle = (uintptr_t)image;
    require(handle == 3 || handle == 4, "unexpected menu image handle");
    return handle - 3;
}

static void menuRecord(VkCommandBuffer command, void (^execute)(void)) {
    NSValue *key = [NSValue valueWithPointer:command];
    if (!menuCommands[key]) menuCommands[key] = [NSMutableArray array];
    [menuCommands[key] addObject:[execute copy]];
}

static VkResult VKAPI_CALL menuAllocateCommands(VkDevice device, const VkCommandBufferAllocateInfo *info, VkCommandBuffer *command) {
    (void)device; (void)info;
    static uintptr_t next = 3;
    *command = (VkCommandBuffer)next++;
    return VK_SUCCESS;
}

static void VKAPI_CALL menuFreeCommands(VkDevice device, VkCommandPool pool, uint32_t count, const VkCommandBuffer *commands) {
    (void)device; (void)pool;
    for (uint32_t index = 0; index < count; ++index)
        [menuCommands removeObjectForKey:[NSValue valueWithPointer:commands[index]]];
}

static VkResult VKAPI_CALL menuFence(VkDevice device, const VkFenceCreateInfo *info,
        const VkAllocationCallbacks *allocator, VkFence *fence) {
    (void)device; (void)info; (void)allocator; *fence = (VkFence)(uintptr_t)7; return VK_SUCCESS;
}

static void VKAPI_CALL menuDestroyFence(VkDevice device, VkFence fence, const VkAllocationCallbacks *allocator) {
    (void)device; (void)fence; (void)allocator;
}

static VkResult VKAPI_CALL menuSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo *submits, VkFence fence) {
    (void)queue; (void)fence;
    for (uint32_t batch = 0; batch < count; ++batch)
        for (uint32_t index = 0; index < submits[batch].commandBufferCount; ++index)
            for (void (^execute)(void) in menuCommands[[NSValue valueWithPointer:submits[batch].pCommandBuffers[index]]]) execute();
    return VK_SUCCESS;
}

static void VKAPI_CALL menuBarrier(VkCommandBuffer command, VkPipelineStageFlags source,
    VkPipelineStageFlags destination, VkDependencyFlags flags, uint32_t memoryCount,
    const VkMemoryBarrier *memory, uint32_t bufferCount, const VkBufferMemoryBarrier *buffers,
    uint32_t imageCount, const VkImageMemoryBarrier *images) {
    (void)source; (void)destination; (void)flags; (void)memoryCount; (void)memory; (void)bufferCount; (void)buffers;
    for (uint32_t index = 0; index < imageCount; ++index) {
        const VkImageMemoryBarrier transition = images[index];
        menuRecord(command, ^{
            const size_t surfaceIndex = menuIndex(transition.image);
            require(transition.oldLayout == VK_IMAGE_LAYOUT_UNDEFINED || transition.oldLayout == menuLayouts[surfaceIndex],
                "menu command used an image layout recorded by an abandoned command");
            if (transition.oldLayout == VK_IMAGE_LAYOUT_UNDEFINED) memset(menuSurfaces[surfaceIndex], 0, sizeof menuPixels);
            menuLayouts[surfaceIndex] = transition.newLayout;
        });
    }
}

static VkResult VKAPI_CALL menuAllocateBuffer(VkDevice device, const VkBufferCreateInfo *info,
        const VkAllocationCallbacks *allocator, VkBuffer *buffer) {
    (void)device; (void)allocator;
    require(info->size == sizeof menuPixels, "menu snapshot length differs from archive geometry");
    if (!menuAllocationLimit) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    if (menuAllocationLimit > 0) --menuAllocationLimit;
    *buffer = (VkBuffer)malloc(info->size);
    return *buffer ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}

static void VKAPI_CALL menuDestroyBuffer(VkDevice device, VkBuffer buffer, const VkAllocationCallbacks *allocator) {
    (void)device; (void)allocator;
    if (buffer != (VkBuffer)menuSurfaces[0] && buffer != (VkBuffer)menuSurfaces[1]) free((void *)buffer);
}

static void VKAPI_CALL menuDestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks *allocator) {
    (void)device; (void)image; (void)allocator;
}

static void VKAPI_CALL menuBufferRequirements(VkDevice device, VkBuffer buffer, VkMemoryRequirements *out) {
    (void)device; (void)buffer; *out = (VkMemoryRequirements){ sizeof menuPixels, 4096, 1 };
}

static VkResult VKAPI_CALL menuAllocateMemory(VkDevice device, const VkMemoryAllocateInfo *info,
        const VkAllocationCallbacks *allocator, VkDeviceMemory *memory) {
    (void)device; (void)info; (void)allocator; *memory = (VkDeviceMemory)(uintptr_t)8; return VK_SUCCESS;
}

static void VKAPI_CALL menuFreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks *allocator) {
    (void)device; (void)memory; (void)allocator;
}

static VkResult VKAPI_CALL menuBindBuffer(VkDevice device, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset) {
    (void)device; (void)buffer; (void)memory; (void)offset; return VK_SUCCESS;
}

static void VKAPI_CALL menuCopy(VkCommandBuffer command, VkBuffer source, VkBuffer destination,
        uint32_t count, const VkBufferCopy *regions) {
    require(count == 1 && (source == (VkBuffer)menuSurfaces[0] || source == (VkBuffer)menuSurfaces[1]) &&
        !regions->srcOffset && !regions->dstOffset &&
        regions->size == sizeof menuPixels, "wrong menu snapshot geometry");
    menuRecord(command, ^{ memcpy((void *)destination, (const void *)source, sizeof menuPixels); menuSnapshots++; });
}

static void VKAPI_CALL menuRestore(VkCommandBuffer command, VkBuffer buffer, VkImage image,
        VkImageLayout layout, uint32_t count, const VkBufferImageCopy *regions) {
    require(count == 1 && !regions->bufferOffset && regions->bufferRowLength == 3840 &&
        regions->imageExtent.width == 3840 && regions->imageExtent.height == 60, "wrong menu restore geometry");
    menuRecord(command, ^{
        const size_t index = menuIndex(image);
        require(menuLayouts[index] == layout, "menu restore lacked a layout transition");
        memcpy(menuSurfaces[index], (const void *)buffer, sizeof menuPixels);
    });
}

static void VKAPI_CALL menuDownload(VkCommandBuffer command, VkImage image, VkImageLayout layout,
        VkBuffer buffer, uint32_t count, const VkBufferImageCopy *regions) {
    require(count == 1 && !regions->bufferOffset && regions->bufferRowLength == 3840 &&
        regions->imageExtent.width == 3840 && regions->imageExtent.height == 60, "wrong menu read geometry");
    menuRecord(command, ^{
        const size_t index = menuIndex(image);
        require(menuLayouts[index] == layout && layout != VK_IMAGE_LAYOUT_UNDEFINED, "submitted menu blit omitted its initial image transition");
        memcpy((void *)buffer, menuSurfaces[index], sizeof menuPixels);
        menuReads++;
    });
}

static MenuCommand *menuReadback(NSArray<RDNA4Texture *> *textures, NSArray<RDNA4Buffer *> *outputs) {
    MenuCommand *command = [MenuCommand new];
    [command configureWritebackFixture];
    id<MTLBlitCommandEncoder> encoder = (id)[[RDNA4BlitEncoder alloc] initWithCommandBuffer:command blit:0];
    require(encoder != nil, "create menu blit encoder");
    for (NSUInteger index = 0; index < textures.count; ++index)
        [encoder copyFromTexture:(id)textures[index] sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(3840, 60, 1) toBuffer:(id)outputs[index] destinationOffset:0
           destinationBytesPerRow:15360 destinationBytesPerImage:0];
    [encoder endEncoding];
    [command rdna4PrepareForSubmission];
    return command;
}

static RDNA4Texture *menuTexture(size_t index) {
    IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef)@{
        (id)kIOSurfaceWidth: @3840, (id)kIOSurfaceHeight: @60,
        (id)kIOSurfaceBytesPerElement: @4, (id)kIOSurfaceBytesPerRow: @15360,
    });
    require(surface != NULL, "allocate menu surface");
    RDNA4Texture *texture = [RDNA4Texture new];
    [texture configureMenuSurface:surface memory:menuSurfaces[index] image:(VkImage)(uintptr_t)(3 + index)];
    CFRelease(surface);
    return texture;
}

static void menuInstallVulkan(void) {
    menuCommands = [NSMutableDictionary dictionary];
    RDNA4R.ok = YES;
    pthread_mutex_init(&RDNA4R.qlock, NULL);
    RDNA4R.mp.memoryTypeCount = 1;
    RDNA4R.mp.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    RDNA4Caps = rdna4dc_fallback(); atomic_store(&rdna4_caps_ready, 1);
    vkAllocateCommandBuffers = menuAllocateCommands; vkFreeCommandBuffers = menuFreeCommands;
    vkBeginCommandBuffer = beginCommand; vkEndCommandBuffer = endCommand;
    vkCreateFence = menuFence; vkDestroyFence = menuDestroyFence; vkQueueSubmit = menuSubmit;
    vkCreateBuffer = menuAllocateBuffer; vkDestroyBuffer = menuDestroyBuffer; vkBindBufferMemory = menuBindBuffer;
    vkDestroyImage = menuDestroyImage;
    vkGetBufferMemoryRequirements = menuBufferRequirements;
    vkAllocateMemory = menuAllocateMemory; vkFreeMemory = menuFreeMemory;
    vkCmdPipelineBarrier = menuBarrier; vkCmdCopyBuffer = menuCopy;
    vkCmdCopyBufferToImage = menuRestore; vkCmdCopyImageToBuffer = menuDownload;
}

static int menuAfterAbandonedCommand(BOOL retry, BOOL sharedScratch) {
    menuInstallVulkan();
    RDNA4Texture *texture = menuTexture(0);
    @autoreleasepool {
        MenuCommand *abandoned = [MenuCommand new];
        [abandoned configureWritebackFixture];
        rdna4_ios_use(abandoned, texture, NO, YES);
        rdna4_tex_to([abandoned vk], texture, VK_IMAGE_LAYOUT_GENERAL);
        [menuCommands removeAllObjects];
    }

    RDNA4Buffer *output = [[RDNA4Buffer alloc] initWithDevice:nil length:sizeof menuPixels
        options:MTLResourceStorageModePrivate | RDNA4_OPT_NOZERO error:NULL];
    require(output != nil, "allocate menu readback destination");
    if (sharedScratch) {
        RDNA4Buffer *second = [[RDNA4Buffer alloc] initWithDevice:nil length:sizeof menuPixels
            options:MTLResourceStorageModePrivate | RDNA4_OPT_NOZERO error:NULL];
        require(second != nil, "allocate second menu readback destination");
        memset(menuSurfaces[0], 0x7a, sizeof menuPixels);
        memset(menuSurfaces[1], 0x26, sizeof menuPixels);
        MenuCommand *submitted = menuReadback(@[texture, menuTexture(1)], @[output, second]);
        menuAllocationLimit = 1;
        (void)[submitted rdna4AdvanceWithQueue:nil];
        require(!menuFailures && menuReads == 2 && menuSnapshots == 2 && !menuAllocationLimit,
            "two linear imports exceeded one snapshot allocation");
        for (size_t index = 0; index < sizeof menuPixels; ++index) {
            require(((const uint8_t *)[output vkBuffer])[index] == 0x7a, "first menu pixels changed during scratch reuse");
            require(((const uint8_t *)[second vkBuffer])[index] == 0x26, "second menu pixels changed during scratch reuse");
        }
        [submitted retireWritebackFixture];
        puts("MENU_SCRATCH_PASS surfaces=2 snapshot_allocations=1 bytes_per_surface=921600 simulated_vulkan=1");
        return 0;
    }
    if (retry) {
        MenuCommand *failed = menuReadback(@[texture], @[output]);
        menuAllocationLimit = 0;
        (void)[failed rdna4AdvanceWithQueue:nil];
        menuAllocationLimit = -1;
        require(menuFailures == 1 && !menuReads && !menuSnapshots, "failed menu initialization submitted partial work");
        [menuCommands removeAllObjects];
    }

    for (unsigned round = 1; round <= 2; ++round) {
        for (size_t index = 0; index < sizeof menuPixels; ++index)
            menuPixels[index] = (uint8_t)(round * 29 + index * 77 + index / 15360);
        MenuCommand *submitted = menuReadback(@[texture], @[output]);
        (void)[submitted rdna4AdvanceWithQueue:nil];
        require(menuReads == round && menuSnapshots == 1, "menu read did not initialize exactly once on submission");
        const uint8_t *readback = (const uint8_t *)[output vkBuffer];
        for (size_t index = 0; index < sizeof menuPixels; ++index)
            require(readback[index] == (uint8_t)(round * 29 + index * 77 + index / 15360), "menu read lost imported pixels");
        [submitted retireWritebackFixture];
        [menuCommands removeAllObjects];
    }
    printf("MENU_FIRST_USE_PASS size=3840x60 reads=2 snapshots=1 pre_submit_failures=%u simulated_vulkan=1\n", menuFailures);
    return 0;
}

static void VKAPI_CALL repaintUpload(VkCommandBuffer command, VkBuffer buffer, VkImage image,
        VkImageLayout layout, uint32_t count, const VkBufferImageCopy *regions) {
    require(count == 1 && !regions->bufferOffset && regions->bufferRowLength == 3840 &&
        !regions->imageOffset.x && !regions->imageOffset.y && regions->imageExtent.width == 3840 &&
        regions->imageExtent.height <= 60, "wrong repaint upload geometry");
    const size_t bytes = regions->imageExtent.height * 15360;
    menuRecord(command, ^{
        const size_t index = menuIndex(image);
        require(menuLayouts[index] == layout, "repaint upload lacks a layout transition");
        memcpy(menuSurfaces[index], (const void *)buffer, bytes);
    });
}

static void VKAPI_CALL repaintCopy(VkCommandBuffer command, VkImage source, VkImageLayout sourceLayout,
        VkImage destination, VkImageLayout destinationLayout, uint32_t count, const VkImageCopy *regions) {
    require(count == 1 && !regions->srcOffset.x && !regions->srcOffset.y && !regions->dstOffset.x &&
        !regions->dstOffset.y && regions->extent.width == 3840 && regions->extent.height == 60,
        "wrong repaint image copy geometry");
    menuRecord(command, ^{
        const size_t from = menuIndex(source), to = menuIndex(destination);
        require(menuLayouts[from] == sourceLayout && menuLayouts[to] == destinationLayout,
            "repaint image copy lacks a layout transition");
        memcpy(menuSurfaces[to], menuSurfaces[from], sizeof menuPixels);
    });
}

static int menuBlitRepaint(const char *mode) {
    const BOOL partial = !strcmp(mode, "blit-partial"), badSource = !strcmp(mode, "blit-incomplete-source");
    const BOOL earlierRead = !strcmp(mode, "blit-earlier-read");
    const BOOL textureCopy = badSource || !strcmp(mode, "blit-full-texture");
    const BOOL recovers = !partial && !badSource && !earlierRead;
    setprogname("WindowServer");
    menuInstallVulkan();
    vkCmdCopyBufferToImage = repaintUpload;
    vkCmdCopyImage = repaintCopy;
    RDNA4Texture *destination = menuTexture(0), *source = menuTexture(1);
    const IOSurfaceID destinationID = IOSurfaceGetID([destination rdna4IOSRef]);
    RDNA4S.incompleteSources = [NSMutableIndexSet indexSetWithIndex:badSource ? IOSurfaceGetID([source rdna4IOSRef]) : destinationID];
    for (RDNA4Texture *texture in @[destination, source]) {
        [texture rdna4IOSLinearDidSubmit];
        menuLayouts[menuIndex([texture vkImage])] = VK_IMAGE_LAYOUT_GENERAL;
    }
    memset(menuSurfaces[0], 0x31, sizeof menuPixels);
    memset(menuSurfaces[1], 0x7b, sizeof menuPixels);
    RDNA4Buffer *input = [[RDNA4Buffer alloc] initWithDevice:nil length:sizeof menuPixels
        options:MTLResourceStorageModePrivate | RDNA4_OPT_NOZERO error:NULL];
    RDNA4Buffer *output = [[RDNA4Buffer alloc] initWithDevice:nil length:sizeof menuPixels
        options:MTLResourceStorageModePrivate | RDNA4_OPT_NOZERO error:NULL];
    require(input && output, "allocate repaint buffers");
    memset((void *)[input vkBuffer], 0x7b, sizeof menuPixels);

    MenuCommand *writer = [MenuCommand new];
    [writer configureWritebackFixture];
    id<MTLBlitCommandEncoder> encoder = (id)[[RDNA4BlitEncoder alloc] initWithCommandBuffer:writer blit:0];
    const MTLOrigin origin = MTLOriginMake(0, 0, 0);
    const MTLSize size = MTLSizeMake(3840, partial ? 59 : 60, 1);
    if (earlierRead)
        [encoder copyFromTexture:(id)destination sourceSlice:0 sourceLevel:0 sourceOrigin:origin sourceSize:size
                        toBuffer:(id)output destinationOffset:0 destinationBytesPerRow:15360 destinationBytesPerImage:0];
    if (textureCopy)
        [encoder copyFromTexture:(id)source sourceSlice:0 sourceLevel:0 sourceOrigin:origin sourceSize:size
                       toTexture:(id)destination destinationSlice:0 destinationLevel:0 destinationOrigin:origin];
    else
        [encoder copyFromBuffer:(id)input sourceOffset:0 sourceBytesPerRow:15360 sourceBytesPerImage:0 sourceSize:size
                      toTexture:(id)destination destinationSlice:0 destinationLevel:0 destinationOrigin:origin];
    [encoder endEncoding];
    [writer rdna4PrepareForSubmission];
    require(![writer rdna4Error], "encode repaint blit");
    require([RDNA4S.incompleteSources containsIndex:destinationID] == !badSource,
        "encoding published a repaint before submission");
    (void)[writer rdna4AdvanceWithQueue:nil];
    require(!menuFailures, "repaint submission failed");
    [writer retireWritebackFixture];
    [menuCommands removeAllObjects];

    MenuCommand *reader = menuReadback(@[destination], @[output]);
    (void)[reader rdna4AdvanceWithQueue:nil];
    require(!menuFailures && menuReads == (earlierRead ? 2u : 1u), "repaint read did not submit");
    const uint8_t *pixels = (const uint8_t *)[output vkBuffer];
    for (size_t index = 0; index < sizeof menuPixels; ++index)
        require(pixels[index] == (index < size.height * 15360 ? 0x7b : 0x31), "repaint read returned wrong pixels");
    require([reader hasIncompleteDisplayInput] == !recovers, "blit repaint left the wrong IOSurface completeness state");
    [reader retireWritebackFixture];
    printf("BLIT_REPAINT_PASS mode=%s recovers=%d pixels=%zu simulated_vulkan=1\n", mode, recovers, sizeof menuPixels / 4);
    return 0;
}

static void VKAPI_CALL scanoutCopy(VkCommandBuffer command, VkBuffer source, VkBuffer destination,
        uint32_t count, const VkBufferCopy *regions) {
    (void)command;
    for (uint32_t index = 0; index < count; ++index)
        memcpy((uint8_t *)destination + regions[index].dstOffset,
            (const uint8_t *)source + regions[index].srcOffset, regions[index].size);
}

static int scanoutPlaneOffset(BOOL partial, BOOL overflow) {
    const size_t width = 3840, height = 2160, pitch = 15360, bytes = pitch * height;
    IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef)@{
        (id)kIOSurfaceWidth: @(width), (id)kIOSurfaceHeight: @(height),
        (id)kIOSurfaceBytesPerElement: @4, (id)kIOSurfaceBytesPerRow: @(pitch),
        (id)kIOSurfaceAllocSize: @(pitch + bytes), (id)kIOSurfacePixelFormat: @((uint32_t)0x42475241),
        (id)kIOSurfacePlaneInfo: @[@{
            (id)kIOSurfacePlaneWidth: @(width), (id)kIOSurfacePlaneHeight: @(height),
            (id)kIOSurfacePlaneBytesPerElement: @4, (id)kIOSurfacePlaneBytesPerRow: @(pitch),
            (id)kIOSurfacePlaneOffset: @(pitch), (id)kIOSurfacePlaneSize: @(bytes),
        }],
    });
    require(surface && IOSurfaceGetPlaneCount(surface) == 1 &&
        IOSurfaceLock(surface, 0, NULL) == kIOReturnSuccess, "cannot create one-plane scanout fixture");
    uint8_t *allocation = IOSurfaceGetBaseAddress(surface);
    uint8_t *plane = IOSurfaceGetBaseAddressOfPlane(surface, 0);
    require(plane == allocation + pitch, "fixture did not preserve the plane offset");
    uint8_t *previous = malloc(bytes), *slot = malloc(bytes);
    require(previous && slot, "cannot allocate scanout fixture slots");
    memset(allocation, 0xff, pitch);
    memset(previous, 0xee, bytes);
    memset(slot, 0xdd, bytes);
    for (size_t row = 0; row < height; ++row) memset(plane + row * pitch, (uint8_t)(row % 237), pitch);
    RDNA4S.pw = (uint32_t)width; RDNA4S.ph = (uint32_t)height; RDNA4S.pitch = (uint32_t)pitch;
    RDNA4S.buf[0] = (VkBuffer)previous; RDNA4S.buf[1] = (VkBuffer)slot;
    rdna4df_plan_t plan = { .slot = 1, .base = partial ? 0 : -1,
        .rect = {11, 0, 19, overflow ? (int32_t)height : 2} };
    vkCmdPipelineBarrier = barrier; vkCmdCopyBuffer = scanoutCopy;
    rdna4s_record_copy(NULL, (VkBuffer)allocation, (size_t)(plane - allocation), &plan);
    if (partial && !overflow) {
        for (size_t row = 0; row < 2; ++row)
            memcpy(previous + row * pitch + 44, plane + row * pitch + 44, 32);
    }
    const uint8_t *expected = partial && !overflow ? previous : plane;
    require(!memcmp(slot, expected, bytes), "scanout did not copy pixels from the IOSurface plane");
    IOSurfaceUnlock(surface, 0, NULL);
    CFRelease(surface);
    free(slot); free(previous);
    printf("SCANOUT_PLANE_OFFSET_PASS offset=%zu bytes=%zu partial=%d overflow=%d simulated_vulkan=1\n",
        pitch, bytes, partial, overflow);
    return 0;
}

int main(int argc, const char **argv) { @autoreleasepool {
    if (argc != 2) return 2;
    alarm(15);
    if (!strcmp(argv[1], "icon-tight")) return iconRoundTrip(2144);
    if (!strcmp(argv[1], "icon-padded")) return iconRoundTrip(2208);
    if (!strcmp(argv[1], "icon-partial-clear")) return iconAttachmentClear(YES);
    if (!strcmp(argv[1], "icon-full-clear")) return iconAttachmentClear(NO);
    if (!strcmp(argv[1], "trace-encode-drop")) return dropTrace(YES);
    if (!strcmp(argv[1], "trace-pre-submit-drop")) return dropTrace(NO);
    if (!strcmp(argv[1], "a8-abandoned-upload")) return a8UploadAfterAbandonedCommand(NO);
    if (!strcmp(argv[1], "a8-abandoned-sample")) return a8UploadAfterAbandonedCommand(YES);
    if (!strcmp(argv[1], "menu-abandoned-command")) return menuAfterAbandonedCommand(NO, NO);
    if (!strcmp(argv[1], "menu-init-retry")) return menuAfterAbandonedCommand(YES, NO);
    if (!strcmp(argv[1], "menu-shared-scratch")) return menuAfterAbandonedCommand(NO, YES);
    if (!strncmp(argv[1], "blit-", 5)) return menuBlitRepaint(argv[1]);
    if (!strcmp(argv[1], "scanout-offset-full")) return scanoutPlaneOffset(NO, NO);
    if (!strcmp(argv[1], "scanout-offset-partial")) return scanoutPlaneOffset(YES, NO);
    if (!strcmp(argv[1], "scanout-offset-overflow")) return scanoutPlaneOffset(YES, YES);
    BOOL linear = !strcmp(argv[1], "linear-segment");
    BOOL writable = linear || !strcmp(argv[1], "writable-segment");
    BOOL segment = writable || !strcmp(argv[1], "readonly-segment");
    BOOL hostAction = !strcmp(argv[1], "readonly-host-action");
    BOOL render = !strcmp(argv[1], "readonly-render");
    BOOL finish = !strcmp(argv[1], "readonly-finish");
    require(segment || hostAction || render || finish, "unknown case");
    const size_t length = 33177600;
    uint8_t *memory = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    require(memory != MAP_FAILED, "map desktop backing");
    memset(memory, 0x6d, length);
    if (!writable) require(!mprotect(memory, length, PROT_READ), "protect desktop backing");
    vkCmdPipelineBarrier = barrier; vkCmdCopyImageToBuffer = download; vkEndCommandBuffer = endCommand;
    vkAllocateCommandBuffers = allocateCommands; vkBeginCommandBuffer = beginCommand;
    WritebackBuffer *buffer = [WritebackBuffer new];
    [buffer rdna4CheckHostReadOnly:memory length:length];
    require([buffer rdna4HostReadOnly] == !writable, "real VM protection classification");
    RDNA4Texture *texture = [RDNA4Texture new];
    [texture configureWritebackBuffer:buffer memory:memory display:!writable];
    if (linear) [texture configureLinearSurface];
    require([texture rdna4HostReadOnly] == !writable, "buffer texture lost backing protection");
    RDNA4CommandBuffer *command = [RDNA4CommandBuffer new];
    [command configureWritebackFixture];
    int diagnostic[2];
    require(pipe(diagnostic) == 0, "create command-buffer log sink");
    RDNA4CB.fd = diagnostic[1];
    atomic_store(&RDNA4CB.on, 1);
    if (render) [command rdna4CBRead:texture];
    NSError *error = nil;
    BOOL accepted;
    if (render) {
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = (id<MTLTexture>)texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        RDNA4RenderEncoder *encoder = [[RDNA4RenderEncoder alloc] initWithCommandBuffer:command descriptor:pass];
        require(encoder != nil, "read-only pass must produce an error-bearing encoder");
        [encoder endEncoding];
        accepted = [command rdna4Finish:&error];
    } else {
        [command rdna4IOWritten:texture];
        if (hostAction) accepted = [command rdna4EncodeOrderedHostAction:^BOOL(NSError **actionError) {
            (void)actionError; require(NO, "host action ran after refused write-back"); return NO;
        } error:&error];
        else if (segment) accepted = [command rdna4EndCurrentSegment:&error];
        else accepted = [command rdna4Finish:&error];
    }
    if (writable) {
        require(RDNA4CB.bytes == 0, "unretired write-back reported completion");
        require(accepted && !error && ![command rdna4Error] && copies == (linear ? 0u : 1u) && ends == 1,
            "writable desktop write-back refused or not ended");
        require([command defersScheduledPublication], "IOSurface write did not defer scheduled publication");
        require(!strcmp(recorded, linear ? "HBHE" : "BWHBHE"), "host visibility must precede segment completion");
        [command retireWritebackFixture];
    } else {
        require(!accepted && error && [command rdna4Error], "refused write-back was acknowledged as successful");
        require(!copies && !ends && ![command rdna4IsFrame], "failed work reached recording or frame publication");
        require([command writebackSegmentCount] == 1, "refused write-back enqueued a later segment");
    }
    atomic_store(&RDNA4CB.on, 0);
    RDNA4CB.fd = -1;
    close(diagnostic[1]);
    char trace[1024] = {0};
    ssize_t traceBytes = read(diagnostic[0], trace, sizeof trace - 1);
    close(diagnostic[0]);
    require(traceBytes >= 0, "read command-buffer log");
    if (!writable) require(strncmp(trace, "W ", 2) && !strstr(trace, "\nW "), "refused write-back reported completion");
    if (writable) {
        require(traceBytes > 0 && !strncmp(trace, "W ", 2) && strstr(trace, linear ? " copies=0 linear=1\n" : " copies=1 linear=0\n"),
            "retired write-back did not log the completed host visibility operation");
    } else if (render) {
        char backing[96];
        snprintf(backing, sizeof backing, "tex=%p base=%p", (__bridge void *)texture, (void *)memory);
        const char *readTrace = strstr(trace, " use=read\n");
        const char *refused = strstr(trace, " use=attachment-refused\n");
        require(traceBytes > 0 && strstr(trace, backing) && strstr(trace, "size=3840x2160 pitch=15360"),
            "surface trace did not identify the texture and backing range");
        require(readTrace && refused && readTrace < refused,
            "read and refused attachment were not distinguished from completed writes");
    } else {
        require(strstr(trace, " use=write\n") && strstr(trace, " use=writeback-refused\n"),
            "write-back refusal could not be attributed to an encoded write");
    }
    for (size_t index = 0; index < length; ++index)
        require(memory[index] == 0x6d, "original client mapping changed");
    munmap(memory, length);
    printf("NOCOPY_WRITEBACK_PASS case=%s bytes=33177600 simulated_vulkan=1 gpu_execution=0\n", argv[1]);
    return 0;
} }
