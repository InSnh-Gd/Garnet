// Production construction, descriptor publication, barriers and backing ownership; no GPU opens.
#include "GarnetDevice.m"

static unsigned checks, created, destroyed, published, barriers, retained;
static BOOL failView, expectedWrite;
static VkFormatFeatureFlags formatFeatures;
static VkBufferView lastView;
static id fixtureDevice;
static uint8_t bytes[1024];
static void require(BOOL ok, const char *why) {
    ++checks;
    if (!ok) { fprintf(stderr, "TEXTURE_BUFFER_RUNTIME_FAIL %s\n", why); exit(1); }
}
@interface TexelBufferFixture : RDNA4Buffer
@property NSUInteger testLength, testOffset;
@property BOOL testReadOnly, testAliasable, testPrivate;
@property (strong) id testDevice;
@end
@implementation TexelBufferFixture
- (id)device { return _testDevice; }
- (NSUInteger)length { return _testLength; }
- (VkDeviceSize)rdna4VkOffset { return _testOffset; }
- (VkBuffer)vkBuffer { return (VkBuffer)(uintptr_t)0x1234; }
- (void *)contents { return _testPrivate ? NULL : bytes; }
- (NSUInteger)resourceOptions { return _testPrivate ? MTLResourceStorageModePrivate : MTLResourceStorageModeShared; }
- (NSUInteger)allocatedSize { return 2048; }
- (BOOL)rdna4HostReadOnly { return _testReadOnly; }
- (BOOL)isAliasable { return _testAliasable; }
@end
@interface TexelCommandFixture : RDNA4CommandBuffer
@property (strong) NSString *failure;
@end
@implementation TexelCommandFixture
- (id)device { return fixtureDevice; }
- (void)rdna4Fail:(NSString *)reason { _failure = reason; }
- (void)rdna4RetainEncoded:(id)object { require(object != nil, "retain a real resource"); ++retained; }
- (void)rdna4RetainArgument:(id)object { require(object != nil, "retain a blit resource"); }
- (NSError *)rdna4Error { return _failure ? rdna4_err(1, _failure) : nil; }
@end
@interface TexelBlitFixture : RDNA4BlitEncoder
@property id sourceBuffer, destinationBuffer;
@property NSUInteger sourceOffset, destinationOffset, copiedBytes, copies;
- (void)useCommand:(TexelCommandFixture *)command;
@end
@implementation TexelBlitFixture
- (void)useCommand:(TexelCommandFixture *)command { _cb = command; }
- (void)copyFromBuffer:(id)source sourceOffset:(NSUInteger)so toBuffer:(id)destination destinationOffset:(NSUInteger)doff size:(NSUInteger)size {
    _sourceBuffer = source; _destinationBuffer = destination;
    _sourceOffset = so; _destinationOffset = doff; _copiedBytes = size; ++_copies;
}
@end
static void VKAPI_CALL properties(VkPhysicalDevice pd, VkFormat format, VkFormatProperties *out) {
    (void)pd; (void)format; *out = (VkFormatProperties){.bufferFeatures = formatFeatures};
}
static VkResult VKAPI_CALL createView(VkDevice device, const VkBufferViewCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkBufferView *out) {
    (void)device; (void)allocator;
    require(info && info->buffer == (VkBuffer)(uintptr_t)0x1234 && info->offset == 512 &&
        info->range == 28 && info->format == VK_FORMAT_R32_SFLOAT,
        "real buffer view includes the underlying Vulkan offset and exact logical range");
    ++created; *out = (VkBufferView)(uintptr_t)(0x9000 + created);
    if (failView) return VK_ERROR_OUT_OF_HOST_MEMORY;
    lastView = *out; return VK_SUCCESS;
}
static void VKAPI_CALL destroyView(VkDevice device, VkBufferView view,
    const VkAllocationCallbacks *allocator) {
    (void)device; (void)allocator; require(view != VK_NULL_HANDLE, "destroy an acquired view"); ++destroyed;
}
static void VKAPI_CALL update(VkDevice device, uint32_t count, const VkWriteDescriptorSet *writes,
    uint32_t copies, const VkCopyDescriptorSet *copy) {
    (void)device; (void)copy;
    require(count == 1 && !copies && writes[0].descriptorCount == 1 &&
        writes[0].descriptorType == (expectedWrite ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER :
            VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER) && writes[0].pTexelBufferView &&
        writes[0].pTexelBufferView[0] == lastView && !writes[0].pImageInfo,
        "publish the matching texel descriptor, never an image descriptor");
    ++published;
}
static void VKAPI_CALL barrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst,
    VkDependencyFlags flags, uint32_t memoryCount, const VkMemoryBarrier *memory,
    uint32_t bufferCount, const VkBufferMemoryBarrier *buffer, uint32_t imageCount,
    const VkImageMemoryBarrier *image) {
    (void)cb; (void)src; (void)dst; (void)flags; (void)memory; (void)image;
    require(!memoryCount && bufferCount == 1 && !imageCount && buffer->buffer == (VkBuffer)(uintptr_t)0x1234 &&
        buffer->offset == 512 && buffer->size == 28 &&
        buffer->srcQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED && buffer->dstQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED,
        "texture preparation barriers cover the shared buffer bytes without a VkImage");
    ++barriers;
}
static MTLTextureDescriptor *descriptor(void) {
    return [MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:MTLPixelFormatR32Float
        width:7 resourceOptions:MTLResourceStorageModeShared usage:MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite];
}
static TexelBufferFixture *backing(void) {
    TexelBufferFixture *b = [TexelBufferFixture new];
    b.testLength = sizeof bytes; b.testOffset = 256; b.testDevice = fixtureDevice; return b;
}
static RDNA4Texture *construct(TexelBufferFixture *b, MTLTextureDescriptor *d, NSUInteger off) {
    NSError *error = nil;
    RDNA4Texture *t = [[RDNA4Texture alloc] initWithDevice:fixtureDevice descriptor:d buffer:b
        offset:off bytesPerRow:0 error:&error];
    require(t ? !error : error != nil, "creation success/error are paired"); return t;
}
static void reflectionComponents(void) {
    RDNA4sId ids[5] = {0};
    ids[2] = (RDNA4sId){.op=25, .elem=1, .dim=5, .sampled=1};
    ids[3] = (RDNA4sId){.op=32, .elem=2};
    ids[4] = (RDNA4sId){.op=59, .elem=3, .sc=0, .set=0, .bind=32};
    RDNA4sMod module = {.bound=4, .id=ids}; RDNA4sRefl *r = calloc(1, sizeof *r);
    require(r != NULL, "reflection allocation");
    for (unsigned storage=0; storage<2; ++storage) for (unsigned component=0; component<3; ++component) {
        ids[1] = (RDNA4sId){.op=component ? 21 : 22, .width=32, .signedness=component==2};
        ids[2].sampled=storage ? 2 : 1;
        require(!rdna4s_reflect(&module,2,r) && r->bind[0][32].texelComponentKinds==(1u<<component) &&
            r->bind[0][32].type==(storage ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER),
            "real reflector preserves all six typed texel bindings");
    }
    free(r);
}
int main(void) {
    @autoreleasepool {
        fixtureDevice = [NSObject new]; RDNA4R.ok = YES;
        RDNA4R.robustTexelBuffers = YES;
        RDNA4R.lim.minTexelBufferOffsetAlignment = 256; RDNA4R.lim.maxTexelBufferElements = 1024;
        formatFeatures = VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT;
        vkGetPhysicalDeviceFormatProperties = properties; vkCreateBufferView = createView;
        vkDestroyBufferView = destroyView; vkUpdateDescriptorSets = update; vkCmdPipelineBarrier = barrier;
        reflectionComponents();
        VkBufferCreateInfo ordinary = rdna4_heap_buffer_create_info(1024, NULL);
        VkExternalMemoryBufferCreateInfo external = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
        VkBufferCreateInfo imported = rdna4_heap_buffer_create_info(1024, &external);
        VkBufferUsageFlags texel = VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;
        require((ordinary.usage & texel) == texel && imported.usage == ordinary.usage && imported.pNext == &external,
            "ordinary, heap and imported buffers use the same complete usage contract");
        for (unsigned test = 0; test < 22; ++test) {
            @autoreleasepool {
                TexelBufferFixture *b = backing(); MTLTextureDescriptor *d = descriptor(); NSUInteger off = 256;
                switch (test) {
                    case 0: d.width = 0; break;
                    case 1: d.width = NSUIntegerMax; break;
                    case 2: d.height = 2; break;
                    case 3: d.depth = 2; break;
                    case 4: d.arrayLength = 2; break;
                    case 5: d.sampleCount = 4; break;
                    case 6: d.mipmapLevelCount = 2; break;
                    case 7: d.usage |= MTLTextureUsageRenderTarget; break;
                    case 8: d.pixelFormat = MTLPixelFormatA8Unorm; break;
                    case 9: d.pixelFormat = MTLPixelFormatDepth32Float; break;
                    case 10: b.testDevice = nil; break;
                    case 11: b.testAliasable = YES; break;
                    case 12: b.testLength = 283; break;
                    case 13: b.testReadOnly = YES; break;
                    case 14: off = 255; break;
                    case 15: b.testOffset = NSUIntegerMax - 255; break;
                    case 16: b.testOffset = 1; break;
                    case 17: d.width = 1025; b.testLength = 8192; break;
                    case 18: formatFeatures = VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT; break;
                    case 19: formatFeatures = VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT; break;
                    case 20: RDNA4R.lim.maxTexelBufferElements = 0; break;
                    case 21: RDNA4R.robustTexelBuffers = NO; break;
                }
                unsigned before = created;
                require(!construct(b, d, off) && created == before, "invalid request refuses before Vulkan view allocation");
                formatFeatures = VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT;
                RDNA4R.lim.maxTexelBufferElements = 1024;
                RDNA4R.robustTexelBuffers = YES;
            }
        }
        failView = YES; unsigned beforeDestroyed = destroyed;
        require(!construct(backing(), descriptor(), 256) && destroyed == beforeDestroyed,
            "view creation failure does not destroy an undefined output handle"); failView = NO;
        __weak TexelBufferFixture *weakBacking;
        @autoreleasepool {
            TexelBufferFixture *b = backing(); weakBacking = b;
            RDNA4Texture *t = construct(b, descriptor(), 256); require(t != nil, "retry succeeds");
            require(t.textureType == MTLTextureTypeTextureBuffer && [t rdna4IsTexel] && ![t rdna4IsIOS] &&
                ![t vkImage] && ![t vkView] && [t rdna4TexelBuffer] == b && [t buffer] == b &&
                t.width == 7 && t.height == 1 && t.depth == 1, "properties preserve direct buffer backing");
            require([t bufferBytesPerRow] == 28, "zero row pitch reports the tightly packed buffer row");
            require([t allocatedSize] == 2048, "view reports the backing allocation without counting a shadow image");
            b = nil; require(weakBacking != nil, "texture retains its backing");
            memset(bytes, 0xa5, sizeof bytes); uint32_t value = 0x12345678, result = 0;
            [t replaceRegion:MTLRegionMake1D(2, 1) mipmapLevel:0 withBytes:&value bytesPerRow:0];
            [t getBytes:&result bytesPerRow:0 fromRegion:MTLRegionMake1D(2, 1) mipmapLevel:0];
            require(result == value && !memcmp(bytes + 256 + 8, &value, 4) && bytes[263] == 0xa5 && bytes[268] == 0xa5,
                "CPU access shares the original buffer and preserves adjacent bytes");
            [t replaceRegion:MTLRegionMake1D(7, 1) mipmapLevel:0 withBytes:&value bytesPerRow:0];
            require(bytes[284] == 0xa5, "CPU region rejection preserves guard");
            TexelCommandFixture *cb = [TexelCommandFixture new]; RDNA4StageState state = {0}; state.tex[1] = t;
            for (unsigned write = 0; write < 2; ++write) {
                expectedWrite = write != 0;
                RDNA4PB binding = {.binding = write ? 481 : 33, .count = 1,
                    .type = write ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
                    .texelComponentKinds = 1u << RDNA4NS_COMP_FLOAT};
                require([cb rdna4FillSet:(VkDescriptorSet)(uintptr_t)1 bindings:&binding count:1 state:&state stage:"test"] &&
                    !cb.failure, "real FillSet accepts typed buffer view");
                __block unsigned visited = 0;
                rdna4_each_tex(&binding, 1, &state, fixtureDevice, ^(RDNA4Texture *texture, VkImageLayout layout, BOOL writable) {
                    require(texture == t && writable == expectedWrite, "stage visits texel resources with correct write intent");
                    rdna4_tex_to((VkCommandBuffer)(uintptr_t)1, texture, layout); ++visited;
                });
                require(visited == 1, "one exact preparation visit");
                unsigned oldPublished = published; state.tex[1] = nil;
                require(![cb rdna4FillSet:(VkDescriptorSet)(uintptr_t)1 bindings:&binding count:1 state:&state stage:"test"] &&
                    cb.failure && published == oldPublished, "missing texel refuses before descriptor publication");
                cb.failure = nil; state.tex[1] = t;
                for (unsigned kinds=0; kinds<8; ++kinds) if (kinds != 1) {
                    binding.texelComponentKinds = (uint8_t)kinds;
                    require(![cb rdna4FillSet:(VkDescriptorSet)(uintptr_t)1 bindings:&binding count:1 state:&state stage:"test"] &&
                        cb.failure && published == oldPublished, "wrong, missing or mixed numeric classes refuse before publication");
                    cb.failure = nil;
                }
            }
            require(published == 2 && barriers == 2 && retained == 2, "exact descriptor/barrier/retention counts");
            TexelBlitFixture *blit = [TexelBlitFixture new]; [blit useCommand:cb];
            TexelBufferFixture *other = backing();
            [blit copyFromBuffer:other sourceOffset:16 sourceBytesPerRow:12 sourceBytesPerImage:0
                sourceSize:MTLSizeMake(3,1,1) toTexture:t destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(1,0,0)];
            require(!cb.failure && blit.copies==1 && blit.sourceBuffer==other && blit.destinationBuffer==weakBacking &&
                blit.sourceOffset==16 && blit.destinationOffset==260 && blit.copiedBytes==12,
                "buffer-to-texel routes the exact logical range through the shared buffer copy and owner path");
            [blit copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(2,0,0)
                sourceSize:MTLSizeMake(2,1,1) toBuffer:other destinationOffset:32 destinationBytesPerRow:8 destinationBytesPerImage:0];
            require(!cb.failure && blit.copies==2 && blit.sourceBuffer==weakBacking && blit.destinationBuffer==other &&
                blit.sourceOffset==264 && blit.destinationOffset==32 && blit.copiedBytes==8,
                "texel-to-buffer uses backing offset plus selected texel origin");
            [blit copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(1,0,0)
                sourceSize:MTLSizeMake(2,1,1) toTexture:t destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(3,0,0)];
            require(!cb.failure && blit.copies==3 && blit.sourceBuffer==weakBacking && blit.destinationBuffer==weakBacking &&
                blit.sourceOffset==260 && blit.destinationOffset==268 && blit.copiedBytes==8,
                "texel-to-texel copy does not treat two absent VkImages as one image");
            [blit copyFromBuffer:other sourceOffset:16 sourceBytesPerRow:8 sourceBytesPerImage:0
                sourceSize:MTLSizeMake(2,1,1) toTexture:t destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(6,0,0)];
            require(cb.failure && blit.copies==3, "out-of-range texel blit refuses before buffer dispatch");
        }
        require(!weakBacking && destroyed == created - 1, "last texture releases its view and backing once");
        @autoreleasepool {
            TexelBufferFixture *b = backing(); b.testPrivate = YES;
            MTLTextureDescriptor *d = descriptor(); d.usage = MTLTextureUsageUnknown;
            RDNA4Texture *t = construct(b, d, 256);
            require(t && t.storageMode == MTLStorageModePrivate && [t rdna4TexelWritable] && ![t rdna4IsIOS],
                "Private buffer and unknown usage preserve backing properties without a host mapping");
            uint8_t before[sizeof bytes]; memcpy(before, bytes, sizeof bytes); uint32_t data = 9;
            [t replaceRegion:MTLRegionMake1D(0,1) mipmapLevel:0 withBytes:&data bytesPerRow:0];
            [t getBytes:&data bytesPerRow:0 fromRegion:MTLRegionMake1D(0,1) mipmapLevel:0];
            require(!memcmp(before,bytes,sizeof bytes) && data==9, "Private CPU access refuses without accessing shadow storage");
        }
        @autoreleasepool {
            TexelBufferFixture *b = backing(); b.testReadOnly = YES;
            MTLTextureDescriptor *d = descriptor(); d.usage = MTLTextureUsageShaderRead;
            formatFeatures = VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT;
            RDNA4Texture *t = construct(b, d, 256); require(t != nil, "read-only backing needs no storage-format feature");
            uint8_t before[sizeof bytes]; memcpy(before, bytes, sizeof bytes); uint32_t data = 9;
            [t replaceRegion:MTLRegionMake1D(0, 1) mipmapLevel:0 withBytes:&data bytesPerRow:0];
            require(!memcmp(before, bytes, sizeof bytes), "read-only CPU backing cannot be written");
            TexelCommandFixture *cb = [TexelCommandFixture new]; RDNA4StageState state = {0}; state.tex[0] = t;
            RDNA4PB binding = {.binding = 480, .count = 1, .type = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,
                .texelComponentKinds = 1u << RDNA4NS_COMP_FLOAT};
            require(![cb rdna4FillSet:(VkDescriptorSet)(uintptr_t)1 bindings:&binding count:1 state:&state stage:"test"] &&
                published == 2, "read-only or read-only-usage texture cannot publish a storage descriptor");
        }
        printf("TEXTURE_BUFFER_RUNTIME_PASS checks=%u views=%u released=%u gpu_execution=0\n", checks, created, destroyed);
        RDNA4R.ok = NO; fixtureDevice = nil;
    }
    return 0;
}
