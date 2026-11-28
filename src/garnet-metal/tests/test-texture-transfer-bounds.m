// Exercise production validation before any CPU copy or Vulkan allocation.
#include "GarnetDevice.m"

static unsigned imageCreates, bufferCreates;
static uint8_t hostBytes[128];
static void require(BOOL ok, const char *why) {
    if (!ok) { fprintf(stderr, "TEXTURE_TRANSFER_BOUNDS_FAIL %s\n", why); exit(1); }
}

@interface BoundsBuffer : RDNA4Buffer
@property NSUInteger fixtureLength, fixtureOffset;
@end
@implementation BoundsBuffer
- (void *)contents { return hostBytes; }
- (NSUInteger)length { return _fixtureLength; }
- (VkDeviceSize)rdna4VkOffset { return _fixtureOffset; }
@end

@interface BoundsTexture : RDNA4Texture
@property BOOL hostFixture;
@end
@implementation BoundsTexture
- (BOOL)rdna4IsIOS { return _hostFixture; }
@end

@interface RDNA4Texture (TransferBoundsFixture)
- (void)configureBoundsFixture:(BOOL)is3D;
@end
@implementation RDNA4Texture (TransferBoundsFixture)
- (void)configureBoundsFixture:(BOOL)is3D {
    _fmt = rdna4_fmt(MTLPixelFormatRGBA8Unorm);
    _w = 4; _h = 3; _depth = is3D ? 3 : 1; _levels = 1;
    _pf = MTLPixelFormatRGBA8Unorm;
    _tdk = is3D ? RDNA4TD_K_3D : RDNA4TD_K_2D; _is3D = is3D;
    _ibase = hostBytes; _ibpr = 16;
}
@end

static VkResult VKAPI_CALL createImage(VkDevice device, const VkImageCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkImage *image) {
    (void)device; (void)info; (void)allocator; *image = VK_NULL_HANDLE;
    ++imageCreates; return VK_ERROR_OUT_OF_DEVICE_MEMORY;
}
static VkResult VKAPI_CALL createBuffer(VkDevice device, const VkBufferCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkBuffer *buffer) {
    (void)device; (void)info; (void)allocator; *buffer = VK_NULL_HANDLE;
    ++bufferCreates; return VK_ERROR_OUT_OF_DEVICE_MEMORY;
}
static void VKAPI_CALL formatProperties(VkPhysicalDevice device, VkFormat format,
    VkFormatProperties *properties) {
    (void)device; (void)format;
    *properties = (VkFormatProperties){ .optimalTilingFeatures =
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
        VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT };
}
static VkResult VKAPI_CALL imageProperties(VkPhysicalDevice device, VkFormat format,
    VkImageType type, VkImageTiling tiling, VkImageUsageFlags usage,
    VkImageCreateFlags flags, VkImageFormatProperties *properties) {
    (void)device; (void)format; (void)type; (void)tiling; (void)usage; (void)flags;
    *properties = (VkImageFormatProperties){ .maxExtent = {32, 24, 16},
        .maxMipLevels = 6, .maxArrayLayers = 12, .sampleCounts = VK_SAMPLE_COUNT_1_BIT };
    return VK_SUCCESS;
}

static void constructorCase(const char *mode) {
    BoundsBuffer *buffer = [BoundsBuffer new]; buffer.fixtureLength = sizeof hostBytes;
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
        MTLPixelFormatRGBA8Unorm width:4 height:3 mipmapped:NO];
    descriptor.storageMode = MTLStorageModeShared;
    NSUInteger offset = 0, pitch = 16;
    if (!strcmp(mode, "buffer-offset-wrap")) offset = NSUIntegerMax - 15;
    else if (!strcmp(mode, "buffer-row-wrap")) pitch = (NSUIntegerMax / 2 + 1);
    else if (!strcmp(mode, "buffer-width-wrap")) { descriptor.width = (NSUIntegerMax / 4 + 1); pitch = 16; }
    else if (!strcmp(mode, "buffer-vkoffset-wrap")) buffer.fixtureOffset = NSUIntegerMax - 15, offset = 16;
    else if (!strcmp(mode, "buffer-backend-limit")) descriptor.width = 20000, pitch = 80000, buffer.fixtureLength = 240000;
    else if (!strcmp(mode, "buffer-final-row-control")) pitch = 32, buffer.fixtureLength = 80;
    else require(NO, "unknown constructor case");
    NSError *error = nil;
    RDNA4Texture *texture = [[RDNA4Texture alloc] initWithDevice:nil descriptor:descriptor
        buffer:buffer offset:offset bytesPerRow:pitch error:&error];
    require(!texture && error, "mock image allocation must refuse after validation");
    require(imageCreates == (!strcmp(mode, "buffer-final-row-control") ? 1u : 0u),
        "invalid backing reached Vulkan, or valid final-row extent was rejected");
}

static void planCase(const char *mode) {
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
        MTLPixelFormatRGBA8Unorm width:16 height:16 mipmapped:NO];
    descriptor.storageMode = MTLStorageModeShared;
    if (!strcmp(mode, "plan-zero")) descriptor.width = 0;
    else if (!strcmp(mode, "plan-truncated-width")) descriptor.width = (NSUInteger)UINT32_MAX + 1;
    else if (!strcmp(mode, "plan-truncated-levels")) descriptor.mipmapLevelCount = (NSUInteger)UINT32_MAX + 2;
    else if (!strcmp(mode, "plan-format-width")) descriptor.width = 33;
    else if (!strcmp(mode, "plan-format-height")) descriptor.height = 25;
    else if (!strcmp(mode, "plan-format-control")) descriptor.width = 32, descriptor.height = 24;
    else require(NO, "unknown image plan case");
    NSError *error = nil; rdna4_heap_image_plan plan;
    BOOL control = !strcmp(mode, "plan-format-control");
    BOOL accepted = rdna4_heap_image_plan_make(descriptor, NO, &plan, &error);
    require(accepted == control && (control ? !error : error != nil),
        "shared constructor/heap plan accepted invalid geometry or rejected exact format limits");
    if (control) require(plan.create.extent.width == 32 && plan.create.extent.height == 24,
        "published image extent changed");
}

static void hostControl(BOOL singleRow) {
    BoundsTexture *texture = [BoundsTexture new];
    [texture configureBoundsFixture:NO]; texture.hostFixture = YES;
    uint8_t external[64], expected[128], readback[64];
    memset(hostBytes, 0x3c, sizeof hostBytes); memcpy(expected, hostBytes, sizeof expected);
    for (unsigned i = 0; i < sizeof external; ++i) external[i] = (uint8_t)(i * 7 + 13);
    NSUInteger pitch = singleRow ? NSUIntegerMax : 12;
    MTLRegion region = MTLRegionMake2D(1, 1, 2, singleRow ? 1 : 2);
    [texture replaceRegion:region mipmapLevel:0 withBytes:external bytesPerRow:pitch];
    for (NSUInteger y = 0; y < region.size.height; ++y)
        memcpy(expected + (1 + y) * 16 + 4, external + y * pitch, 8);
    require(!memcmp(expected, hostBytes, sizeof expected), "valid partial CPU upload or padding differs");
    memset(readback, 0xcd, sizeof readback);
    [texture getBytes:readback bytesPerRow:pitch fromRegion:region mipmapLevel:0];
    for (NSUInteger y = 0; y < region.size.height; ++y)
        require(!memcmp(readback + y * pitch, external + y * pitch, 8), "valid CPU readback differs");
    if (!singleRow) for (unsigned i = 0; i < sizeof readback; ++i)
        if (i >= 20 || (i >= 8 && i < 12)) require(readback[i] == 0xcd, "valid readback padding changed");
}

static void transferCase(const char *mode) {
    BOOL host = !strncmp(mode, "host-", 5);
    BOOL imageOverflow = strstr(mode, "image-wrap") != NULL;
    BoundsTexture *texture = [BoundsTexture new];
    [texture configureBoundsFixture:imageOverflow]; texture.hostFixture = host;
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    uint8_t *mapping = mmap(NULL, page * 3, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);
    require(mapping != MAP_FAILED && !mprotect(mapping + page, page, PROT_READ | PROT_WRITE), "guarded mapping");
    uint8_t *external = mapping + page, before[128];
    memset(hostBytes, 0x3c, sizeof hostBytes); memset(external, 0x7b, page);
    memcpy(before, hostBytes, sizeof before);
    NSUInteger pitch = imageOverflow ? 16 : NSUIntegerMax - 15;
    NSUInteger imagePitch = imageOverflow ? NSUIntegerMax - 47 : 0;
    MTLRegion region = MTLRegionMake3D(0, 0, 0, 4, 3, imageOverflow ? 3 : 1);
    if (strstr(mode, "replace")) {
        [texture replaceRegion:region mipmapLevel:0 slice:0 withBytes:external
            bytesPerRow:pitch bytesPerImage:imagePitch];
        require(!memcmp(before, hostBytes, sizeof before), "refused upload modified texture backing");
    } else {
        [texture getBytes:external bytesPerRow:pitch bytesPerImage:imagePitch
            fromRegion:region mipmapLevel:0 slice:0];
        for (size_t i = 0; i < page; ++i)
            require(external[i] == 0x7b, "refused readback modified destination");
    }
    require(!bufferCreates, "overflowing CPU layout reached staging allocation");
    munmap(mapping, page * 3);
}

int main(int argc, const char **argv) { @autoreleasepool {
    if (argc != 2) return 2;
    alarm(10); setvbuf(stdout, NULL, _IONBF, 0);
    RDNA4R.tried = RDNA4R.ok = YES;
    RDNA4R.lim.maxImageDimension2D = 16384;
    RDNA4R.lim.maxImageDimension3D = 2048;
    vkCreateImage = createImage; vkCreateBuffer = createBuffer;
    vkGetPhysicalDeviceFormatProperties = formatProperties;
    vkGetPhysicalDeviceImageFormatProperties = imageProperties;
    if (!strcmp(argv[1], "host-transfer-control")) hostControl(NO);
    else if (!strcmp(argv[1], "host-single-row-control")) hostControl(YES);
    else if (!strncmp(argv[1], "buffer-", 7)) constructorCase(argv[1]);
    else if (!strncmp(argv[1], "plan-", 5)) planCase(argv[1]);
    else if (!strncmp(argv[1], "host-", 5) || !strncmp(argv[1], "staging-", 8)) transferCase(argv[1]);
    else return 2;
    printf("TEXTURE_TRANSFER_BOUNDS_PASS case=%s simulated_vulkan=1 gpu_execution=0\n", argv[1]);
    return 0;
} }
