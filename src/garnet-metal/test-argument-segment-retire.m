/*
 * Device-level segment retirement and Private-summary integration fixture.
 *
 * This includes the production device implementation, but replaces Vulkan
 * allocation and recording with narrow mocks. It allocates a zero-filled test
 * subclass without the production queue initializer so the command-buffer
 * lifecycle methods can be exercised without a Vulkan device or a GPU.
 *
 * The cases intentionally use the production helpers rather than a second
 * model of their rules:
 *   - rdna4_argument_summary_owners_encode() with an allocation fault,
 *   - rdna4CancelUnsubmittedArgumentIntents(),
 *   - rdna4SignalSegment().
 */
#include "GarnetDevice.m"

#include <stdatomic.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    size_t size;
    uint8_t *cpu;
} TestMemory;

typedef struct {
    VkDeviceSize size;
    TestMemory *memory;
} TestBuffer;

static unsigned checks;
static unsigned liveBuffers;
static unsigned liveMemory;
static unsigned allocationCalls;
static unsigned failedAllocationAt;
static unsigned commandBufferFrees;
static unsigned expectedCommandBufferFrees;
static unsigned descriptorPoolCreates;
static unsigned descriptorPoolDestroys;
static unsigned descriptorPoolSerial;
static unsigned descriptorAllocationFailuresRemaining;
static NSMutableSet<NSNumber *> *liveDescriptorPools;
static NSMutableSet<NSNumber *> *liveRenderPasses, *liveFramebuffers;
static unsigned renderPassSerial, framebufferSerial;
static unsigned renderPassDestroys, framebufferDestroys;
static VkFramebuffer lastRenderFramebuffer;

static VkResult VKAPI_CALL createRenderPass(VkDevice device,
    const VkRenderPassCreateInfo *info, const VkAllocationCallbacks *allocator,
    VkRenderPass *renderPass) {
    (void)device; (void)info; (void)allocator;
    *renderPass = (VkRenderPass)(uintptr_t)(0x300000u + ++renderPassSerial * 16u);
    [liveRenderPasses addObject:@((uintptr_t)*renderPass)];
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL createFramebuffer(VkDevice device,
    const VkFramebufferCreateInfo *info, const VkAllocationCallbacks *allocator,
    VkFramebuffer *framebuffer) {
    (void)device; (void)info; (void)allocator;
    *framebuffer = (VkFramebuffer)(uintptr_t)(0x400000u + ++framebufferSerial * 16u);
    [liveFramebuffers addObject:@((uintptr_t)*framebuffer)];
    return VK_SUCCESS;
}

static void require(BOOL value, const char *message) {
    ++checks;
    if (!value) {
        fprintf(stderr, "ARGUMENT_SEGMENT_RETIRE_FAIL %s\n", message);
        exit(1);
    }
}

static void VKAPI_CALL destroyRenderPass(VkDevice device,
    VkRenderPass renderPass, const VkAllocationCallbacks *allocator) {
    (void)device; (void)allocator;
    require([liveRenderPasses containsObject:@((uintptr_t)renderPass)],
        "render pass destroyed twice or was never created");
    [liveRenderPasses removeObject:@((uintptr_t)renderPass)];
    ++renderPassDestroys;
}

static void VKAPI_CALL destroyFramebuffer(VkDevice device,
    VkFramebuffer framebuffer, const VkAllocationCallbacks *allocator) {
    (void)device; (void)allocator;
    require([liveFramebuffers containsObject:@((uintptr_t)framebuffer)],
        "framebuffer destroyed twice or was never created");
    [liveFramebuffers removeObject:@((uintptr_t)framebuffer)];
    ++framebufferDestroys;
}

static void VKAPI_CALL beginRenderPass(VkCommandBuffer command,
    const VkRenderPassBeginInfo *info, VkSubpassContents contents) {
    (void)contents;
    require(command && [liveRenderPasses containsObject:@((uintptr_t)info->renderPass)] &&
            [liveFramebuffers containsObject:@((uintptr_t)info->framebuffer)],
        "render pass recorded with retired handles");
    lastRenderFramebuffer = info->framebuffer;
}

static VkResult VKAPI_CALL createBuffer(VkDevice device,
    const VkBufferCreateInfo *info, const VkAllocationCallbacks *allocator,
    VkBuffer *buffer) {
    (void)device;
    (void)allocator;
    if (!info || !buffer) return VK_ERROR_INITIALIZATION_FAILED;
    TestBuffer *created = calloc(1, sizeof *created);
    if (!created) return VK_ERROR_OUT_OF_HOST_MEMORY;
    created->size = info->size;
    *buffer = (VkBuffer)(uintptr_t)created;
    ++liveBuffers;
    return VK_SUCCESS;
}

static void VKAPI_CALL destroyBuffer(VkDevice device, VkBuffer buffer,
    const VkAllocationCallbacks *allocator) {
    (void)device;
    (void)allocator;
    if (!buffer) return;
    free((void *)(uintptr_t)buffer);
    --liveBuffers;
}

static void VKAPI_CALL bufferRequirements(VkDevice device, VkBuffer buffer,
    VkMemoryRequirements *requirements) {
    (void)device;
    TestBuffer *created = (TestBuffer *)(uintptr_t)buffer;
    require(created != NULL, "buffer requirements received a null buffer");
    *requirements = (VkMemoryRequirements){
        .size = (created->size + 4095u) & ~(VkDeviceSize)4095u,
        .alignment = 4096,
        .memoryTypeBits = 3,
    };
}

static VkResult VKAPI_CALL allocateMemory(VkDevice device,
    const VkMemoryAllocateInfo *info, const VkAllocationCallbacks *allocator,
    VkDeviceMemory *memory) {
    (void)device;
    (void)allocator;
    if (!info || !memory) return VK_ERROR_INITIALIZATION_FAILED;
    ++allocationCalls;
    if (failedAllocationAt && allocationCalls == failedAllocationAt) {
        failedAllocationAt = 0;
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    TestMemory *created = calloc(1, sizeof *created);
    if (!created) return VK_ERROR_OUT_OF_HOST_MEMORY;
    created->size = (size_t)info->allocationSize;
    if (info->memoryTypeIndex == 1) {
        created->cpu = calloc(1, created->size ? created->size : 1);
        if (!created->cpu) {
            free(created);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
    }
    *memory = (VkDeviceMemory)(uintptr_t)created;
    ++liveMemory;
    return VK_SUCCESS;
}

static void VKAPI_CALL freeMemory(VkDevice device, VkDeviceMemory memory,
    const VkAllocationCallbacks *allocator) {
    (void)device;
    (void)allocator;
    if (!memory) return;
    TestMemory *created = (TestMemory *)(uintptr_t)memory;
    free(created->cpu);
    free(created);
    --liveMemory;
}

static VkResult VKAPI_CALL bindMemory(VkDevice device, VkBuffer buffer,
    VkDeviceMemory memory, VkDeviceSize offset) {
    (void)device;
    TestBuffer *created = (TestBuffer *)(uintptr_t)buffer;
    require(created != NULL, "bind received a null buffer");
    require(offset == 0, "fixture only binds at offset zero");
    created->memory = (TestMemory *)(uintptr_t)memory;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL mapMemory(VkDevice device, VkDeviceMemory memory,
    VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags flags,
    void **pointer) {
    (void)device;
    (void)offset;
    (void)size;
    (void)flags;
    if (!pointer) return VK_ERROR_INITIALIZATION_FAILED;
    TestMemory *created = (TestMemory *)(uintptr_t)memory;
    *pointer = created ? created->cpu : NULL;
    return *pointer ? VK_SUCCESS : VK_ERROR_MEMORY_MAP_FAILED;
}

static void VKAPI_CALL freeCommandBuffers(VkDevice device, VkCommandPool pool,
    uint32_t count, const VkCommandBuffer *commands) {
    (void)device;
    (void)pool;
    require(count == 1 && commands && *commands,
        "a completed segment must free exactly one command buffer");
    ++commandBufferFrees;
}

static VkResult VKAPI_CALL createDescriptorPool(VkDevice device,
    const VkDescriptorPoolCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkDescriptorPool *pool) {
    (void)device;
    (void)allocator;
    if (!info || !pool) return VK_ERROR_INITIALIZATION_FAILED;
    VkDescriptorPool created = (VkDescriptorPool)(uintptr_t)
        (0x100000u + (++descriptorPoolSerial) * 16u);
    NSNumber *key = @((uintptr_t)created);
    require(![liveDescriptorPools containsObject:key],
        "descriptor pool mock address was reused");
    [liveDescriptorPools addObject:key];
    *pool = created;
    ++descriptorPoolCreates;
    return VK_SUCCESS;
}

static void VKAPI_CALL destroyDescriptorPool(VkDevice device,
    VkDescriptorPool pool, const VkAllocationCallbacks *allocator) {
    (void)device;
    (void)allocator;
    if (!pool) return;
    NSNumber *key = @((uintptr_t)pool);
    require([liveDescriptorPools containsObject:key],
        "descriptor pool was destroyed twice or was never created");
    [liveDescriptorPools removeObject:key];
    ++descriptorPoolDestroys;
}

static VkResult VKAPI_CALL allocateDescriptorSets(VkDevice device,
    const VkDescriptorSetAllocateInfo *info, VkDescriptorSet *sets) {
    (void)device;
    if (!info || !sets || !info->descriptorSetCount)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (descriptorAllocationFailuresRemaining) {
        --descriptorAllocationFailuresRemaining;
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    for (uint32_t i = 0; i < info->descriptorSetCount; ++i)
        sets[i] = (VkDescriptorSet)(uintptr_t)(0x200000u + i * 16u);
    return VK_SUCCESS;
}

static void signalSegment(
    RDNA4CommandBuffer *commandBuffer,
    RDNA4CommandSegment *segment) {
    ++expectedCommandBufferFrees;
    [commandBuffer rdna4SignalSegment:segment];
}

@interface RDNA4SegmentRetireCommandBuffer : RDNA4CommandBuffer
- (void)rdna4TestSetSegments:(NSMutableArray<RDNA4CommandSegment *> *)segments
                    current:(RDNA4CommandSegment *)current;
- (void)rdna4TestSetCurrentSegment:(RDNA4CommandSegment *)current
                            serial:(uint64_t)serial;
- (NSUInteger)rdna4TestLegacyRefCount;
- (VkDescriptorPool)rdna4TestDescriptorPool;
@end

@implementation RDNA4SegmentRetireCommandBuffer
- (void)rdna4TestSetSegments:(NSMutableArray<RDNA4CommandSegment *> *)segments
                    current:(RDNA4CommandSegment *)current {
    _segments = segments;
    _curSegment = current;
    _vk = current ? current->vk : VK_NULL_HANDLE;
    _dslot = -1;
    if (!_refs) _refs = [NSMutableArray array];
}
- (void)rdna4TestSetCurrentSegment:(RDNA4CommandSegment *)current
                            serial:(uint64_t)serial {
    _curSegment = current;
    _vk = current ? current->vk : VK_NULL_HANDLE;
    _segmentSerial = serial;
    _pool = VK_NULL_HANDLE;
}
- (NSUInteger)rdna4TestLegacyRefCount {
    return _refs.count;
}
- (VkDescriptorPool)rdna4TestDescriptorPool {
    return _pool;
}
@end

@interface RDNA4SegmentRetireSentinel : NSObject
@end

@implementation RDNA4SegmentRetireSentinel
@end

@interface RDNA4SegmentRetireHeap : RDNA4Heap {
    __weak id _member;
}
- (void)rdna4TestSetMember:(id)member;
@end

@implementation RDNA4SegmentRetireHeap
- (void)rdna4TestSetMember:(id)member { _member = member; }
- (NSArray *)rdna4LiveResources {
    id member = _member;
    return member ? @[member] : @[];
}
@end

@interface RDNA4RenderEncoder (EncoderRefsFixture)
- (void)rdna4SetBuffer:(id)buffer offset:(NSUInteger)offset
                 index:(NSUInteger)index frag:(BOOL)fragment;
- (void)rdna4SetTexture:(id)texture index:(NSUInteger)index
                   frag:(BOOL)fragment;
- (void)rdna4OwnResource:(id)resource kind:(uint32_t)kind
                fragment:(BOOL)fragment index:(NSUInteger)index;
- (void)rdna4SnapshotResources;
- (void)rdna4SyncResourceSegment;
- (void)rdna4BeginPass;
@end

@interface RDNA4EncoderRefsRenderEncoder : RDNA4RenderEncoder
- (void)rdna4TestConfigureWithCommandBuffer:
    (RDNA4SegmentRetireCommandBuffer *)commandBuffer
                                      drawCache:(BOOL)drawCache;
- (void)rdna4TestSetVertexBuffer:(id)buffer index:(NSUInteger)index;
- (void)rdna4TestSetFragmentBuffer:(id)buffer index:(NSUInteger)index;
- (void)rdna4TestSetVertexTexture:(id)texture index:(NSUInteger)index;
- (void)rdna4TestSetFragmentTexture:(id)texture index:(NSUInteger)index;
- (void)rdna4TestSetPipeline:(id)pipeline;
- (void)rdna4TestSetDepthState:(id)state;
- (void)rdna4TestOwnResource:(id)resource kind:(uint32_t)kind
                       index:(NSUInteger)index;
- (void)rdna4TestSnapshot;
- (void)rdna4TestSyncSegment;
- (NSUInteger)rdna4TestBindingCount;
- (void)rdna4TestPrimeDrawCachePipeline:(uint64_t)pipeline
                                 layout:(uint64_t)layout;
- (uint64_t)rdna4TestLastPipeline;
- (uint64_t)rdna4TestDrawCacheLayout;
- (BOOL)rdna4TestVertexSetValid;
- (VkRenderPass)rdna4TestCreateRenderHandlesCached:(BOOL)cached;
- (void)rdna4TestBeginPassWithFetch:(BOOL)fetch;
@end

@implementation RDNA4EncoderRefsRenderEncoder
- (void)rdna4TestConfigureWithCommandBuffer:
    (RDNA4SegmentRetireCommandBuffer *)commandBuffer
                                      drawCache:(BOOL)drawCache {
    _cb = commandBuffer;
    _resourceBindings = [RDNA4ResourceBindings new];
    _resourceSegmentSerial = [_cb rdna4SegmentSerial];
    _dcOn = drawCache;
    if (_dcOn) rdna4dc_enc_init(&[self rdna4EnsureScratch]->dc);
}
- (void)rdna4TestSetVertexBuffer:(id)buffer index:(NSUInteger)index {
    [self rdna4SetBuffer:buffer offset:0 index:index frag:NO];
}
- (void)rdna4TestSetFragmentBuffer:(id)buffer index:(NSUInteger)index {
    [self rdna4SetBuffer:buffer offset:0 index:index frag:YES];
}
- (void)rdna4TestSetVertexTexture:(id)texture index:(NSUInteger)index {
    [self rdna4SetTexture:texture index:index frag:NO];
}
- (void)rdna4TestSetFragmentTexture:(id)texture index:(NSUInteger)index {
    [self rdna4SetTexture:texture index:index frag:YES];
}
- (void)rdna4TestSetPipeline:(id)pipeline {
    [self setRenderPipelineState:pipeline];
}
- (void)rdna4TestSetDepthState:(id)state {
    [self setDepthStencilState:state];
}
- (void)rdna4TestOwnResource:(id)resource kind:(uint32_t)kind
                       index:(NSUInteger)index {
    [self rdna4OwnResource:resource kind:kind fragment:NO index:index];
}
- (void)rdna4TestSnapshot {
    [self rdna4SnapshotResources];
}
- (void)rdna4TestSyncSegment {
    [self rdna4SyncResourceSegment];
}
- (NSUInteger)rdna4TestBindingCount {
    return _resourceBindings.count;
}
- (void)rdna4TestPrimeDrawCachePipeline:(uint64_t)pipeline
                                 layout:(uint64_t)layout {
    RDNA4RenderScratch *sc = [self rdna4EnsureScratch];
    sc->dc.layout = layout;
    sc->dc.lastPipe = pipeline;
    sc->dc.st[0].valid = 1;
    sc->dc.st[1].valid = 1;
}
- (uint64_t)rdna4TestLastPipeline {
    return [self rdna4EnsureScratch]->dc.lastPipe;
}
- (uint64_t)rdna4TestDrawCacheLayout {
    return [self rdna4EnsureScratch]->dc.layout;
}
- (BOOL)rdna4TestVertexSetValid {
    return [self rdna4EnsureScratch]->dc.st[0].valid != 0;
}
- (VkRenderPass)rdna4TestCreateRenderHandlesCached:(BOOL)cached {
    _rp1 = rdna4_mk_rp(NULL, 0, 0, NO);
    _rp1c = cached;
    VkFramebufferCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = _rp1,
    };
    require(vkCreateFramebuffer(RDNA4R.dev, &info, NULL, &_fb) == VK_SUCCESS,
        "render handle fixture allocation");
    _baseHandles = [[RDNA4RenderHandles alloc]
        initWithRenderPass:cached ? VK_NULL_HANDLE : _rp1 framebuffer:_fb];
    return _rp1;
}
- (void)rdna4TestBeginPassWithFetch:(BOOL)fetch {
    _inPass = NO;
    _fetch = fetch;
    [self rdna4BeginPass];
}
@end

@interface RDNA4ComputeEncoder (EncoderRefsFixture)
- (void)rdna4SnapshotResources;
@end

@interface RDNA4EncoderRefsComputeEncoder : RDNA4ComputeEncoder
- (void)rdna4TestConfigureWithCommandBuffer:
    (RDNA4SegmentRetireCommandBuffer *)commandBuffer;
- (void)rdna4TestSetBuffer:(id)buffer index:(NSUInteger)index;
- (void)rdna4TestSetTexture:(id)texture index:(NSUInteger)index;
- (void)rdna4TestSetPipeline:(id)pipeline;
- (void)rdna4TestSnapshot;
- (NSUInteger)rdna4TestBindingCount;
@end

@implementation RDNA4EncoderRefsComputeEncoder
- (void)rdna4TestConfigureWithCommandBuffer:
    (RDNA4SegmentRetireCommandBuffer *)commandBuffer {
    _cb = commandBuffer;
    _resourceBindings = [RDNA4ResourceBindings new];
}
- (void)rdna4TestSetBuffer:(id)buffer index:(NSUInteger)index {
    [self setBuffer:buffer offset:0 atIndex:index];
}
- (void)rdna4TestSetTexture:(id)texture index:(NSUInteger)index {
    [self setTexture:texture atIndex:index];
}
- (void)rdna4TestSetPipeline:(id)pipeline {
    [self setComputePipelineState:pipeline];
}
- (void)rdna4TestSnapshot {
    [self rdna4SnapshotResources];
}
- (NSUInteger)rdna4TestBindingCount {
    return _resourceBindings.count;
}
@end

static __attribute__((ns_returns_retained)) RDNA4SegmentRetireCommandBuffer *newCommandBuffer(
    NSMutableArray<RDNA4CommandSegment *> *segments,
    RDNA4CommandSegment *current) {
    /*
     * -initWithQueue: would allocate a Vulkan segment. +alloc already
     * zero-initializes every inherited ivar; the setter supplies only the
     * segment state used by this lifecycle fixture.
     */
    RDNA4SegmentRetireCommandBuffer *commandBuffer =
        [RDNA4SegmentRetireCommandBuffer alloc];
    require(commandBuffer != nil, "command-buffer test object allocation");
    [commandBuffer rdna4TestSetSegments:segments current:current];
    return commandBuffer;
}

static void disposeCommandBuffer(
    RDNA4SegmentRetireCommandBuffer * __strong *commandBuffer) {
    if (!*commandBuffer) return;
    [*commandBuffer rdna4TestSetSegments:nil current:nil];
    *commandBuffer = nil;
}

// Weak retirement checks must not observe an autoreleased factory result.
static __attribute__((ns_returns_retained)) RDNA4Buffer *newBuffer(id device, NSUInteger length,
    MTLResourceOptions options, NSError **error) {
    return [[RDNA4Buffer alloc] initWithDevice:device
                                        length:length
                                       options:options
                                         error:error];
}

static RDNA4ArgumentWriteIntent *newIntent(RDNA4Buffer *buffer, id sampler) {
    NSSet *candidates = sampler ? [NSSet setWithObject:sampler] : [NSSet set];
    return rdna4_argument_sampler_intent(
        buffer, NSMakeRange(0, buffer.length), candidates, NO);
}

static __attribute__((ns_returns_retained)) RDNA4ArgumentSummaryOwnerAttempt *
newCandidateFreeSummaryAttempt(RDNA4Buffer *source,
    RDNA4ArgumentWriteIntent *intent, VkCommandBuffer command,
    uint64_t captureSerial, NSError **error) {
    RDNA4Buffer *output = newBuffer(source.device, sizeof(uint32_t),
        MTLResourceStorageModeShared, error);
    if (!output) return nil;
    RDNA4ArgumentSummaryTask *task = rdna4_argument_summary_task_new(
        source, 0, source.length, nil, 0, 0, output, 0, captureSerial,
        NULL, 0, 0, error);
    if (!task) return nil;
    RDNA4ArgumentSummaryAttempt *attempt = nil;
    if (!rdna4_argument_summary_encode(task, command, &attempt, error))
        return nil;

    RDNA4ArgumentSamplerOwners *owners =
        rdna4_argument_sampler_owners(source, NO);
    require(owners != nil, "candidate-free summary owner state");
    RDNA4ArgumentSummaryOwnerCapture *capture =
        [RDNA4ArgumentSummaryOwnerCapture new];
    capture->buffer = source;
    capture->task = task;
    capture->attempt = attempt;
    capture->tokens = @[];
    capture->tokenObjects = @[];
    capture->completedSerials = [NSSet set];
    capture->segmentSerials =
        [NSSet setWithObject:@(intent->serial)];
    capture->captureSerial = captureSerial;
    capture->hostGeneration = owners.hostGeneration;

    RDNA4ArgumentSummaryOwnerAttempt *wrapper =
        [RDNA4ArgumentSummaryOwnerAttempt new];
    wrapper->captures = @[capture];
    return wrapper;
}

static void setupMocks(void) {
    RDNA4R.tried = YES;
    RDNA4R.ok = YES;
    RDNA4R.dev = (VkDevice)(uintptr_t)1;
    RDNA4R.bufferAddress = NO;
    RDNA4P.on = NO;
    pthread_mutex_init(&RDNA4R.qlock, NULL);
    RDNA4R.mp.memoryTypeCount = 2;
    RDNA4R.mp.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    RDNA4R.mp.memoryTypes[1].propertyFlags =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    RDNA4Caps = rdna4dc_fallback();
    RDNA4Caps.buffer_length = UINT64_C(1) << 20;
    atomic_store(&rdna4_caps_ready, 1);
    RDNA4R.lim.maxStorageBufferRange = 1u << 20;
    RDNA4R.lim.minStorageBufferOffsetAlignment = 4;
    RDNA4R.lim.maxComputeWorkGroupCount[0] = 65535;
    RDNA4R.lim.maxComputeWorkGroupInvocations = 64;
    RDNA4R.lim.maxComputeWorkGroupSize[0] = 64;

    vkCreateBuffer = createBuffer;
    vkDestroyBuffer = destroyBuffer;
    vkGetBufferMemoryRequirements = bufferRequirements;
    vkAllocateMemory = allocateMemory;
    vkFreeMemory = freeMemory;
    vkBindBufferMemory = bindMemory;
    vkMapMemory = mapMemory;
    vkFreeCommandBuffers = freeCommandBuffers;
    vkCreateDescriptorPool = createDescriptorPool;
    vkDestroyDescriptorPool = destroyDescriptorPool;
    vkAllocateDescriptorSets = allocateDescriptorSets;
    liveDescriptorPools = [NSMutableSet set];
    liveRenderPasses = [NSMutableSet set];
    liveFramebuffers = [NSMutableSet set];
    vkCreateRenderPass = createRenderPass;
    vkDestroyRenderPass = destroyRenderPass;
    vkCreateFramebuffer = createFramebuffer;
    vkDestroyFramebuffer = destroyFramebuffer;
    vkCmdBeginRenderPass = beginRenderPass;
}

static void testCancellationPreservesSubmittedAndUnknown(void) {
    @autoreleasepool {
        NSError *error = nil;
        id device = [NSObject new];
        id sampler = [NSObject new];

        RDNA4Buffer *acceptedBuffer =
            newBuffer(device, 64, MTLResourceStorageModePrivate, &error);
        require(acceptedBuffer != nil && !error,
            "accepted Private buffer allocation");
        RDNA4ArgumentWriteIntent *acceptedIntent =
            newIntent(acceptedBuffer, nil);
        require(acceptedIntent != nil, "accepted intent creation");
        rdna4_argument_sampler_submit(acceptedIntent, 7);

        /*
         * A transfer with no previous owner state is not summary work.
         * Preserve that fastpath explicitly instead of manufacturing a GPU
         * dispatch with an empty dictionary.
         */
        @autoreleasepool {
            NSArray *encodedAttempts = nil;
            NSError *summaryError = nil;
            BOOL summaryEncoded = rdna4_argument_summary_owners_encode(
                @[acceptedIntent], (VkCommandBuffer)(uintptr_t)0x50,
                &encodedAttempts, &summaryError);
            RDNA4ArgumentSamplerOwners *acceptedOwners =
                rdna4_argument_sampler_owners(acceptedBuffer, NO);
            require(summaryEncoded && encodedAttempts.count == 0 &&
                    !summaryError && acceptedOwners.latestSummaryEpoch == 0,
                "candidate-free transfer scheduled summary work");
        }
        NSError *summaryError = nil;
        RDNA4ArgumentSummaryOwnerAttempt *acceptedSummaryAttempt =
            newCandidateFreeSummaryAttempt(acceptedBuffer, acceptedIntent,
                (VkCommandBuffer)(uintptr_t)0x50, 1, &summaryError);
        require(acceptedSummaryAttempt != nil && !summaryError,
            "candidate-free summary attempt creation");
        __weak id weakAcceptedAttempt = acceptedSummaryAttempt;
        NSArray *summaryAttempts = [[NSArray alloc] initWithObjects:acceptedSummaryAttempt, nil];
        acceptedSummaryAttempt = nil;

        RDNA4CommandSegment *accepted = [RDNA4CommandSegment new];
        accepted->submitted = YES;
        accepted->argumentSubmissionEpoch = 7;
        accepted->vk = (VkCommandBuffer)(uintptr_t)0x51;
        [accepted->argumentIntents addObject:acceptedIntent];
        accepted->argumentSummaryAttempts = summaryAttempts;
        summaryAttempts = nil;
        RDNA4SegmentRetireSentinel *acceptedRef =
            [RDNA4SegmentRetireSentinel new];
        RDNA4SegmentRetireSentinel *acceptedPool =
            [RDNA4SegmentRetireSentinel new];
        __weak id weakAcceptedRef = acceptedRef;
        __weak id weakAcceptedPool = acceptedPool;
        [accepted->argumentRefs addObject:acceptedRef];
        [accepted->argumentPools addObject:acceptedPool];
        acceptedRef = nil;
        acceptedPool = nil;

        RDNA4Buffer *unknownBuffer =
            newBuffer(device, 64, MTLResourceStorageModePrivate, &error);
        require(unknownBuffer != nil && !error,
            "unknown-retire Private buffer allocation");
        RDNA4ArgumentWriteIntent *unknownIntent =
            newIntent(unknownBuffer, sampler);
        require(unknownIntent != nil, "unknown-retire intent creation");
        RDNA4CommandSegment *unknown = [RDNA4CommandSegment new];
        unknown->retirementUnknown = YES;
        unknown->vk = (VkCommandBuffer)(uintptr_t)0x52;
        [unknown->argumentIntents addObject:unknownIntent];
        RDNA4SegmentRetireSentinel *unknownAttempt =
            [RDNA4SegmentRetireSentinel new];
        __weak id weakUnknownAttempt = unknownAttempt;
        unknown->argumentSummaryAttempts = [[NSArray alloc] initWithObjects:unknownAttempt, nil];
        unknownAttempt = nil;
        RDNA4SegmentRetireSentinel *unknownRef =
            [RDNA4SegmentRetireSentinel new];
        __weak id weakUnknownRef = unknownRef;
        [unknown->argumentRefs addObject:unknownRef];
        unknownRef = nil;

        RDNA4Buffer *futureBuffer =
            newBuffer(device, 64, MTLResourceStorageModePrivate, &error);
        require(futureBuffer != nil && !error,
            "future Private buffer allocation");
        RDNA4ArgumentWriteIntent *futureIntent =
            newIntent(futureBuffer, sampler);
        require(futureIntent != nil, "future intent creation");
        RDNA4CommandSegment *future = [RDNA4CommandSegment new];
        [future->argumentIntents addObject:futureIntent];
        RDNA4SegmentRetireSentinel *futureAttempt =
            [RDNA4SegmentRetireSentinel new];
        __weak id weakFutureAttempt = futureAttempt;
        future->argumentSummaryAttempts = [[NSArray alloc] initWithObjects:futureAttempt, nil];
        futureAttempt = nil;
        RDNA4SegmentRetireSentinel *futureRef =
            [RDNA4SegmentRetireSentinel new];
        __weak id weakFutureRef = futureRef;
        [future->argumentRefs addObject:futureRef];
        futureRef = nil;
        RDNA4SegmentRetireSentinel *futureCommandRef =
            [RDNA4SegmentRetireSentinel new];
        __weak id weakFutureCommandRef = futureCommandRef;
        [future->commandRefs addObject:futureCommandRef];
        futureCommandRef = nil;

        NSMutableArray<RDNA4CommandSegment *> *segments =
            [NSMutableArray arrayWithObjects:accepted, unknown, future, nil];
        RDNA4SegmentRetireCommandBuffer *commandBuffer =
            newCommandBuffer(segments, nil);
        __weak RDNA4SegmentRetireCommandBuffer *weakCommandBuffer =
            commandBuffer;

        [commandBuffer rdna4CancelUnsubmittedArgumentIntents];
        require(acceptedIntent->active, "submitted intent was abandoned");
        require(unknownIntent->active, "unknown-retirement intent was abandoned");
        require(!futureIntent->active, "future intent was not abandoned");
        require(accepted->argumentIntents.count == 1,
            "submitted segment lost its intent");
        require(unknown->argumentIntents.count == 1,
            "unknown-retirement segment lost its intent");
        require(future->argumentIntents.count == 0,
            "future segment retained its canceled intent");
        require(accepted->argumentSummaryAttempts.count == 1,
            "submitted segment lost its summary attempt");
        require(unknown->argumentSummaryAttempts.count == 1,
            "unknown-retirement segment lost its summary attempt");
        require(future->argumentSummaryAttempts == nil,
            "future segment retained its summary attempt");
        require(accepted->argumentRefs.count == 1 &&
                accepted->argumentPools.count == 1,
            "submitted segment snapshots were cleared by cancel");
        require(unknown->argumentRefs.count == 1,
            "unknown-retirement segment snapshot was cleared by cancel");
        require(future->argumentRefs.count == 0 &&
                future->argumentPools.count == 0 &&
                future->commandRefs.count == 0,
            "future segment snapshots were not cleared");
        require(unknown->vk == (VkCommandBuffer)(uintptr_t)0x52,
            "cancel freed a retirement-unknown command buffer");

        RDNA4ArgumentSamplerOwners *acceptedOwners =
            rdna4_argument_sampler_owners(acceptedBuffer, NO);
        RDNA4ArgumentSamplerOwners *unknownOwners =
            rdna4_argument_sampler_owners(unknownBuffer, NO);
        RDNA4ArgumentSamplerOwners *futureOwners =
            rdna4_argument_sampler_owners(futureBuffer, NO);
        require(acceptedOwners.writers.count == 1,
            "submitted owner was removed by cancel");
        require(unknownOwners.writers.count == 1,
            "unknown-retirement owner was removed by cancel");
        require(futureOwners.writers.count == 0,
            "future owner was not removed by cancel");

        signalSegment(commandBuffer, accepted);
        require(!acceptedIntent->active,
            "completed intent remained active after segment signal");
        require(acceptedOwners.writers.count == 0,
            "completed owner remained in writers");
        require(acceptedOwners.latestSummaryEpoch == 7,
            "completed segment summary was not published");
        require(accepted->argumentIntents.count == 0,
            "completed segment retained argument intents");
        require(accepted->argumentSummaryAttempts == nil,
            "completed segment retained summary attempts");
        require(accepted->argumentRefs.count == 0 &&
                accepted->argumentPools.count == 0,
            "completed segment retained snapshots or pools");
        require(accepted->vk == VK_NULL_HANDLE,
            "completed command buffer was not released");
        require(commandBufferFrees == 1,
            "completed segment did not free exactly one command buffer");
        require(weakAcceptedAttempt == nil,
            "completed segment leaked summary sentinel");
        require(weakAcceptedRef == nil,
            "completed segment leaked ref sentinel");
        require(weakAcceptedPool == nil,
            "completed segment leaked pool sentinel");
        require(weakUnknownAttempt != nil && weakUnknownRef != nil,
            "unknown-retirement sentinels were released early");
        require(weakFutureAttempt == nil && weakFutureRef == nil &&
                weakFutureCommandRef == nil,
            "future sentinels were retained after cancellation");
        require(unknown->vk == (VkCommandBuffer)(uintptr_t)0x52,
            "unknown-retirement command buffer was released early");

        disposeCommandBuffer(&commandBuffer);
        require(weakCommandBuffer == nil,
            "ARC command-buffer release did not destroy the fixture object");
        acceptedBuffer = nil;
        unknownBuffer = nil;
        futureBuffer = nil;
        device = nil;
        sampler = nil;
    }
}

static void testRealArgumentWriteReleasesBufferWhileCommandRetained(void) {
    @autoreleasepool {
        NSError *error = nil;
        id device = [NSObject new];
        id sampler = [NSObject new];
        require(rdna4_argument_resource_token(sampler, device, NO) != 0,
            "real argument-write sampler token");

        RDNA4Buffer *buffer =
            newBuffer(device, 64, MTLResourceStorageModePrivate, &error);
        require(buffer != nil && !error, "real argument-write Private buffer");
        __weak id weakBuffer = buffer;

        RDNA4CommandSegment *segment = [RDNA4CommandSegment new];
        segment->submitted = YES;
        segment->argumentSubmissionEpoch = 11;
        segment->vk = (VkCommandBuffer)(uintptr_t)0x53;
        NSMutableArray<RDNA4CommandSegment *> *segments =
            [NSMutableArray arrayWithObject:segment];
        RDNA4SegmentRetireCommandBuffer *commandBuffer =
            newCommandBuffer(segments, segment);
        NSMutableArray<id> *completedCommandBuffers =
            [NSMutableArray arrayWithObject:commandBuffer];

        RDNA4ArgumentSamplerOwners *owners = nil;
        @autoreleasepool {
            [commandBuffer rdna4ArgumentWrite:buffer
                                       range:NSMakeRange(0, buffer.length)
                                  candidates:[NSSet setWithObject:sampler]
                                    fillOnly:NO];
            require(segment->argumentIntents.count == 1,
                "real argument write did not create an intent");
            require(segment->argumentRefs.count == 1,
                "real argument write did not snapshot its buffer");
            RDNA4ArgumentWriteIntent *writtenIntent =
                segment->argumentIntents[0];
            /*
             * Device attaches the accepted submission epoch after vkQueueSubmit /
             * native execution succeeds (GarnetDevice.m submit path). Signal is a
             * completion callback, not the point that accepts the segment.
             */
            rdna4_argument_sampler_submit(
                writtenIntent, segment->argumentSubmissionEpoch);
            require(writtenIntent->submitted,
                "real argument write was not marked submitted");
            owners = rdna4_argument_sampler_owners(buffer, NO);
            require(owners.writers.count == 1,
                "real argument write did not register its writer");

            buffer = nil;
            require(weakBuffer != nil,
                "real argument-write buffer was released before segment signal");
            signalSegment(commandBuffer, segment);
        }
        require(segment->argumentIntents.count == 0,
            "real argument write retained its intent after signal");
        require(segment->argumentRefs.count == 0,
            "real argument write retained its buffer snapshot");
        require(owners.writers.count == 0,
            "real argument write retained its writer after signal");
        require(weakBuffer == nil,
            "real argument-write buffer survived signal while CB was retained");
        require(completedCommandBuffers.count == 1 &&
                completedCommandBuffers[0] == commandBuffer,
            "completed command history lost the command buffer");
        require(segment->vk == VK_NULL_HANDLE,
            "real argument-write command buffer was not released");

        disposeCommandBuffer(&commandBuffer);
        [completedCommandBuffers removeAllObjects];
        device = nil;
        sampler = nil;
    }
}

static void expectSummaryAllocationFailure(
    RDNA4ArgumentWriteIntent *pending,
    RDNA4Buffer *buffer, unsigned allocationOffset) {
    NSError *error = nil;
    NSArray *attempts = @[ @"stale-attempt" ];
    RDNA4ArgumentSamplerOwners *owners =
        rdna4_argument_sampler_owners(buffer, NO);
    require(owners != nil, "summary owner state missing");
    require(owners.writers.count == 1,
        "unexpected pending owner count before summary failure");
    uint64_t latest = owners.latestSummaryEpoch;
    uint64_t completed = owners.lastCompletedEpoch;
    failedAllocationAt = allocationCalls + allocationOffset;
    BOOL encoded = rdna4_argument_summary_owners_encode(
        @[pending], (VkCommandBuffer)(uintptr_t)0x71, &attempts, &error);
    require(!encoded, "summary allocation failure was accepted");
    require(error != nil, "summary allocation failure had no error");
    require(failedAllocationAt == 0,
        "summary allocation failure was not injected");
    require(attempts.count == 0,
        "failed summary exposed a partial attempt");
    require(pending->active, "failed summary abandoned its pending intent");
    require(owners.writers.count == 1,
        "failed summary removed the pending owner");
    require(owners.latestSummaryEpoch == latest,
        "failed summary advanced latestSummaryEpoch");
    require(owners.lastCompletedEpoch == completed,
        "failed summary changed lastCompletedEpoch");
}

static void testSummaryAllocationFailureAndFutureCancel(void) {
    @autoreleasepool {
        NSError *error = nil;
        id device = [NSObject new];
        id sampler = [NSObject new];
        RDNA4Buffer *buffer =
            newBuffer(device, 64, MTLResourceStorageModePrivate, &error);
        require(buffer != nil && !error, "summary test Private buffer");
        require(rdna4_argument_resource_token(sampler, device, NO) != 0,
            "summary test sampler token");

        RDNA4ArgumentWriteIntent *old = newIntent(buffer, sampler);
        require(old != nil, "old owner intent creation");
        rdna4_argument_sampler_submit(old, 1);
        rdna4_argument_sampler_complete(old);
        RDNA4ArgumentSamplerOwners *owners =
            rdna4_argument_sampler_owners(buffer, NO);
        require(owners != nil && owners.writers.count == 0 &&
                owners.possible.count == 1 &&
                owners.lastCompletedEpoch == 1,
            "old persistent owner was not established");

        RDNA4ArgumentWriteIntent *pending = newIntent(buffer, sampler);
        require(pending != nil, "pending summary intent creation");
        require(owners.writers.count == 1,
            "pending writer was not registered");

        /*
         * The first fault removes the dictionary allocation; the second
         * removes the output allocation after the dictionary succeeds. Both
         * paths must leave the existing owner state and the pending writer
         * untouched.
         */
        expectSummaryAllocationFailure(pending, buffer, 1);
        expectSummaryAllocationFailure(pending, buffer, 2);
        require(owners.possible.count == 1,
            "summary failure removed the old persistent owner");

        RDNA4CommandSegment *future = [RDNA4CommandSegment new];
        [future->argumentIntents addObject:pending];
        RDNA4SegmentRetireSentinel *attempt =
            [RDNA4SegmentRetireSentinel new];
        future->argumentSummaryAttempts = @[attempt];
        attempt = nil;
        NSMutableArray<RDNA4CommandSegment *> *segments =
            [NSMutableArray arrayWithObject:future];
        RDNA4SegmentRetireCommandBuffer *commandBuffer =
            newCommandBuffer(segments, nil);
        [commandBuffer rdna4CancelUnsubmittedArgumentIntents];
        require(!pending->active,
            "unaccepted summary writer was not canceled");
        require(owners.writers.count == 0,
            "cancel retained the unaccepted summary writer");
        require(owners.possible.count == 1,
            "cancel removed the old persistent owner");
        require(future->argumentSummaryAttempts == nil,
            "cancel retained the future summary attempt");

        disposeCommandBuffer(&commandBuffer);
        buffer = nil;
        device = nil;
        sampler = nil;
    }
}

static void testEncoderBindingsFollowSegments(void) {
    @autoreleasepool {
        NSError *error = nil;
        id device = [NSObject new];

        RDNA4CommandSegment *first = [RDNA4CommandSegment new];
        first->vk = (VkCommandBuffer)(uintptr_t)0x61;
        first->hasWork = YES;
        RDNA4CommandSegment *second = [RDNA4CommandSegment new];
        second->vk = (VkCommandBuffer)(uintptr_t)0x62;
        second->hasWork = YES;
        NSMutableArray<RDNA4CommandSegment *> *segments =
            [NSMutableArray arrayWithObjects:first, second, nil];
        RDNA4SegmentRetireCommandBuffer *commandBuffer =
            newCommandBuffer(segments, first);
        [commandBuffer rdna4TestSetCurrentSegment:first serial:1];

        RDNA4EncoderRefsRenderEncoder *render =
            [RDNA4EncoderRefsRenderEncoder alloc];
        [render rdna4TestConfigureWithCommandBuffer:commandBuffer
                                         drawCache:YES];
        require([render rdna4TestDrawCacheLayout] == 0,
            "fixture draw cache did not start empty");

        RDNA4Buffer *buffer =
            newBuffer(device, 64, MTLResourceStorageModePrivate, &error);
        require(buffer != nil && !error, "binding fixture buffer");
        __weak id weakBuffer = buffer;
        [render rdna4TestSetVertexBuffer:buffer index:3];
        require([render rdna4TestBindingCount] == 1,
            "vertex buffer was not a bounded active binding");
        require([commandBuffer rdna4TestLegacyRefCount] == 0,
            "vertex buffer leaked into the command-lifetime ref array");
        require(weakBuffer != nil,
            "active vertex buffer was not strongly owned by the encoder");

        [render rdna4TestSnapshot];
        require(first->commandRefs.count == 1,
            "vertex buffer was not snapshotted into the current segment");
        require([commandBuffer rdna4TestLegacyRefCount] == 0,
            "vertex buffer snapshot used command-lifetime refs");
        buffer = nil;
        require(weakBuffer != nil,
            "segment snapshot released its buffer before segment completion");
        [render rdna4TestSetVertexBuffer:nil index:3];
        require([render rdna4TestBindingCount] == 0,
            "clearing a vertex buffer left a historical active binding");
        require(weakBuffer != nil,
            "segment snapshot lost its buffer after the active slot was cleared");

        [render rdna4TestPrimeDrawCachePipeline:0x701 layout:0x801];
        [commandBuffer rdna4TestSetCurrentSegment:second serial:2];
        [render rdna4TestSyncSegment];
        require([render rdna4TestLastPipeline] == 0 &&
                [render rdna4TestDrawCacheLayout] == 0 &&
                ![render rdna4TestVertexSetValid],
            "draw cache survived a Vulkan command-buffer segment change");

        signalSegment(commandBuffer, first);
        require(first->commandRefs.count == 0,
            "completed segment retained command refs");
        require(weakBuffer == nil,
            "completed segment retained its encoded buffer");

        RDNA4RenderPipelineState *oldPipeline =
            [RDNA4RenderPipelineState alloc];
        __weak id weakOldPipeline = oldPipeline;
        [render rdna4TestSetPipeline:oldPipeline];
        require([render rdna4TestBindingCount] == 1,
            "render pipeline was not a bounded active binding");
        require([commandBuffer rdna4TestLegacyRefCount] == 0,
            "render pipeline leaked into command-lifetime refs");

        RDNA4RenderPipelineState *newPipeline =
            [RDNA4RenderPipelineState alloc];
        [render rdna4TestSetPipeline:newPipeline];
        oldPipeline = nil;
        require(weakOldPipeline == nil,
            "replaced render pipeline remained in encoder history");

        RDNA4Texture *texture = [RDNA4Texture alloc];
        __weak id weakTexture = texture;
        [render rdna4TestSetVertexTexture:texture index:5];
        [render rdna4TestSetFragmentTexture:texture index:6];
        texture = nil;
        require([render rdna4TestBindingCount] == 3,
            "fragment/vertex texture slots collided in the active set");
        require(weakTexture != nil,
            "texture was not strongly owned by its active slot");

        RDNA4DepthStencilState *depth = [RDNA4DepthStencilState alloc];
        __weak id weakDepth = depth;
        [render rdna4TestSetDepthState:depth];
        depth = nil;
        require([render rdna4TestBindingCount] == 4,
            "depth/stencil state was not tracked as an active binding");

        __weak id weakNewPipeline = newPipeline;
        [render rdna4TestSnapshot];
        require(second->commandRefs.count == 3,
            "draw resources were not deduplicated into the new segment");
        RDNA4RenderPipelineState *thirdPipeline =
            [RDNA4RenderPipelineState alloc];
        [render rdna4TestSetPipeline:thirdPipeline];
        newPipeline = nil;
        require(weakNewPipeline != nil,
            "replaced pipeline was not retained by the segment snapshot");
        [render rdna4TestSetVertexTexture:nil index:5];
        [render rdna4TestSetFragmentTexture:nil index:6];
        [render rdna4TestSetDepthState:nil];
        require([render rdna4TestBindingCount] == 1,
            "cleared render slots left historical active bindings");

        signalSegment(commandBuffer, second);
        require(second->commandRefs.count == 0,
            "second segment retained command refs");
        require(weakNewPipeline == nil && weakTexture == nil &&
                weakDepth == nil,
            "completed second segment leaked encoded resources");
        require(first->commandRefs.count == 0,
            "first segment history reappeared after the second completed");

        render = nil;
        thirdPipeline = nil;
        disposeCommandBuffer(&commandBuffer);
        device = nil;
    }
}

static void testAttachmentBindingsFollowSegments(void) {
    @autoreleasepool {
        RDNA4CommandSegment *segment = [RDNA4CommandSegment new];
        segment->vk = (VkCommandBuffer)(uintptr_t)0x67;
        segment->hasWork = YES;
        NSMutableArray<RDNA4CommandSegment *> *segments =
            [NSMutableArray arrayWithObject:segment];
        RDNA4SegmentRetireCommandBuffer *commandBuffer =
            newCommandBuffer(segments, segment);
        [commandBuffer rdna4TestSetCurrentSegment:segment serial:6];

        RDNA4EncoderRefsRenderEncoder *render =
            [RDNA4EncoderRefsRenderEncoder alloc];
        [render rdna4TestConfigureWithCommandBuffer:commandBuffer
                                         drawCache:NO];
        RDNA4Texture *attachment = [RDNA4Texture alloc];
        RDNA4Texture *resolve = [RDNA4Texture alloc];
        __weak id weakAttachment = attachment;
        __weak id weakResolve = resolve;

        [render rdna4TestOwnResource:attachment
                                kind:RDNA4_RESOURCE_ATTACHMENT
                               index:0];
        [render rdna4TestOwnResource:resolve
                                kind:RDNA4_RESOURCE_RESOLVE
                               index:0];
        attachment = nil;
        resolve = nil;
        require([render rdna4TestBindingCount] == 2,
            "attachment and resolve slots collided");
        require(weakAttachment != nil && weakResolve != nil,
            "attachment or resolve was not strongly owned by its active slot");
        require(segment->argumentRefs.count == 0,
            "attachment ownership used argument snapshot storage");

        [render rdna4TestSnapshot];
        require(segment->commandRefs.count == 2,
            "attachment and resolve were not segment-snapshotted");
        [render rdna4TestOwnResource:nil
                                kind:RDNA4_RESOURCE_ATTACHMENT
                               index:0];
        [render rdna4TestOwnResource:nil
                                kind:RDNA4_RESOURCE_RESOLVE
                               index:0];
        require([render rdna4TestBindingCount] == 0,
            "cleared attachment slots stayed in the active set");
        require(weakAttachment != nil && weakResolve != nil,
            "segment snapshot released attachment ownership early");

        signalSegment(commandBuffer, segment);
        require(segment->commandRefs.count == 0,
            "completed attachment segment retained command refs");
        require(weakAttachment == nil && weakResolve == nil,
            "completed attachment segment leaked resources");

        render = nil;
        disposeCommandBuffer(&commandBuffer);
    }
}

static void testComputeBindingsFollowSegments(void) {
    @autoreleasepool {
        NSError *error = nil;
        id device = [NSObject new];
        RDNA4CommandSegment *segment = [RDNA4CommandSegment new];
        segment->vk = (VkCommandBuffer)(uintptr_t)0x63;
        segment->hasWork = YES;
        NSMutableArray<RDNA4CommandSegment *> *segments =
            [NSMutableArray arrayWithObject:segment];
        RDNA4SegmentRetireCommandBuffer *commandBuffer =
            newCommandBuffer(segments, segment);
        [commandBuffer rdna4TestSetCurrentSegment:segment serial:3];

        RDNA4EncoderRefsComputeEncoder *compute =
            [RDNA4EncoderRefsComputeEncoder alloc];
        [compute rdna4TestConfigureWithCommandBuffer:commandBuffer];
        RDNA4Buffer *buffer =
            newBuffer(device, 64, MTLResourceStorageModePrivate, &error);
        require(buffer != nil && !error, "compute binding fixture buffer");
        __weak id weakBuffer = buffer;
        RDNA4Texture *texture = [RDNA4Texture alloc];
        __weak id weakTexture = texture;
        RDNA4ComputePipelineState *pipeline =
            [RDNA4ComputePipelineState alloc];
        __weak id weakPipeline = pipeline;

        [compute rdna4TestSetBuffer:buffer index:2];
        [compute rdna4TestSetTexture:texture index:7];
        [compute rdna4TestSetPipeline:pipeline];
        buffer = nil;
        texture = nil;
        pipeline = nil;
        require([compute rdna4TestBindingCount] == 3,
            "compute active bindings were not bounded");
        require([commandBuffer rdna4TestLegacyRefCount] == 0,
            "compute bindings leaked into command-lifetime refs");
        require(weakBuffer != nil && weakTexture != nil &&
                weakPipeline != nil,
            "compute active binding lost a resource");

        [compute rdna4TestSnapshot];
        require(segment->commandRefs.count == 3,
            "compute dispatch resources were not segment-snapshotted");
        RDNA4ComputePipelineState *replacement =
            [RDNA4ComputePipelineState alloc];
        [compute rdna4TestSetPipeline:replacement];
        require(weakPipeline != nil,
            "replaced compute pipeline was not kept by the segment snapshot");
        [compute rdna4TestSetBuffer:nil index:2];
        [compute rdna4TestSetTexture:nil index:7];
        require([compute rdna4TestBindingCount] == 1,
            "compute encoder retained cleared historical bindings");

        signalSegment(commandBuffer, segment);
        require(segment->commandRefs.count == 0,
            "compute segment retained command refs");
        require(weakBuffer == nil && weakTexture == nil &&
                weakPipeline == nil,
            "completed compute segment leaked encoded resources");

        compute = nil;
        replacement = nil;
        disposeCommandBuffer(&commandBuffer);
        device = nil;
    }
}

static void testDeclarationsFollowSegments(void) {
    @autoreleasepool {
        NSError *error = nil;
        RDNA4CommandSegment *first = [RDNA4CommandSegment new];
        first->vk = (VkCommandBuffer)(uintptr_t)0x68;
        RDNA4CommandSegment *second = [RDNA4CommandSegment new];
        second->vk = (VkCommandBuffer)(uintptr_t)0x69;
        NSMutableArray<RDNA4CommandSegment *> *segments =
            [NSMutableArray arrayWithObjects:first, second, nil];
        RDNA4SegmentRetireCommandBuffer *commandBuffer =
            newCommandBuffer(segments, first);
        RDNA4Buffer *buffer =
            newBuffer(nil, 64, MTLResourceStorageModePrivate, &error);
        RDNA4Buffer *member =
            newBuffer(nil, 64, MTLResourceStorageModePrivate, &error);
        require(buffer != nil && member != nil && !error,
            "declaration fixture buffers");
        RDNA4SegmentRetireHeap *heap = [RDNA4SegmentRetireHeap alloc];
        [heap rdna4TestSetMember:member];
        __weak id weakBuffer = buffer;
        __weak id weakMember = member;
        __weak id weakHeap = heap;

        @autoreleasepool {
            for (unsigned repeat = 0; repeat < 8; ++repeat) {
                [commandBuffer rdna4UseResource:buffer usage:MTLResourceUsageRead];
                [commandBuffer rdna4UseHeap:heap];
            }
        }
        require([commandBuffer rdna4Error] == nil &&
                first->commandRefs.count == 3,
            "repeated declarations did not deduplicate in their segment");
        require([commandBuffer rdna4TestLegacyRefCount] == 0,
            "declarations leaked into command-lifetime refs");

        [commandBuffer rdna4TestSetCurrentSegment:second serial:7];
        RDNA4ArgumentTables tables = {0};
        RDNA4ArgumentScanPrepared scan = {0};
        @autoreleasepool {
            NSSet *resources = [NSSet setWithObjects:buffer, heap, nil];
            require([commandBuffer rdna4PrepareArguments:&tables
                resources:resources writable:[NSSet set] scan:&scan
                textures:nil],
                "inactive-table declaration snapshot failed");
        }
        require(second->argumentRefs.count == 4 &&
                [second->argumentRefs containsObject:buffer] &&
                [second->argumentRefs containsObject:heap] &&
                [second->argumentRefs containsObject:member],
            "continued encoder did not snapshot declared buffer/heap members");
        unsigned registrySnapshots = 0;
        for (id object in second->argumentRefs)
            if ([object isKindOfClass:[NSSet class]]) {
                require([object count] == 2 && [object containsObject:buffer] &&
                        [object containsObject:member],
                    "segment buffer registry includes unrelated declarations");
                ++registrySnapshots;
            }
        require(registrySnapshots == 1,
            "segment did not retain exactly one immutable buffer registry");
        buffer = nil;
        member = nil;
        heap = nil;
        signalSegment(commandBuffer, first);
        require(weakBuffer != nil && weakMember != nil && weakHeap != nil,
            "first fence released declarations still used by a later segment");
        signalSegment(commandBuffer, second);
        require(weakBuffer == nil && weakMember == nil && weakHeap == nil,
            "completed declaration segments retained resources");
        require([commandBuffer rdna4TestLegacyRefCount] == 0,
            "declaration snapshots acquired command-lifetime history");
        disposeCommandBuffer(&commandBuffer);
    }
}

static void testRenderHandlesFollowSegments(void) {
    @autoreleasepool {
        RDNA4CommandSegment *first = [RDNA4CommandSegment new];
        first->vk = (VkCommandBuffer)(uintptr_t)0x71;
        RDNA4CommandSegment *second = [RDNA4CommandSegment new];
        second->vk = (VkCommandBuffer)(uintptr_t)0x72;
        RDNA4CommandSegment *third = [RDNA4CommandSegment new];
        third->vk = (VkCommandBuffer)(uintptr_t)0x73;
        NSMutableArray<RDNA4CommandSegment *> *segments =
            [NSMutableArray arrayWithObjects:first, second, third, nil];
        RDNA4SegmentRetireCommandBuffer *commandBuffer =
            newCommandBuffer(segments, first);
        RDNA4EncoderRefsRenderEncoder *render =
            [RDNA4EncoderRefsRenderEncoder alloc];
        [render rdna4TestConfigureWithCommandBuffer:commandBuffer drawCache:NO];
        VkRenderPass base = [render rdna4TestCreateRenderHandlesCached:NO];
        [render rdna4TestBeginPassWithFetch:NO];
        VkFramebuffer baseFramebuffer = lastRenderFramebuffer;
        require(first->commandRefs.count == 1,
            "initial pass did not snapshot its shared handle owner");
        signalSegment(commandBuffer, first);
        require([liveRenderPasses containsObject:@((uintptr_t)base)] &&
                [liveFramebuffers containsObject:@((uintptr_t)baseFramebuffer)],
            "first fence retired handles still owned by an active encoder");

        [commandBuffer rdna4TestSetCurrentSegment:second serial:8];
        [render rdna4TestBeginPassWithFetch:NO];
        require(lastRenderFramebuffer == baseFramebuffer &&
                second->commandRefs.count == 2,
            "continuation did not share the base framebuffer and own its pass");
        [render rdna4TestBeginPassWithFetch:YES];
        VkFramebuffer fetchFramebuffer = lastRenderFramebuffer;
        require(fetchFramebuffer != baseFramebuffer &&
                second->commandRefs.count == 3,
            "first fetch did not own its framebuffer/pass pair");

        [commandBuffer rdna4TestSetCurrentSegment:third serial:9];
        [render rdna4TestBeginPassWithFetch:YES];
        require(lastRenderFramebuffer == fetchFramebuffer &&
                third->commandRefs.count == 3,
            "later segment failed to share fetch handles and own its continuation");
        [render rdna4ReleaseRenderHandles];
        unsigned oldPassDestroys = renderPassDestroys;
        signalSegment(commandBuffer, second);
        require(renderPassDestroys == oldPassDestroys + 1 &&
                liveFramebuffers.count == 2,
            "second fence retired a shared pair or retained its continuation");
        signalSegment(commandBuffer, third);
        require(liveRenderPasses.count == 0 && liveFramebuffers.count == 0,
            "last consuming fence retained render handles while encoder/CB lived");
        require([commandBuffer rdna4TestLegacyRefCount] == 0,
            "render handles acquired command-buffer history");
        render = nil;
        disposeCommandBuffer(&commandBuffer);
    }
}

static void testRenderHandlesBoundedAcrossEncoders(void) {
    @autoreleasepool {
        NSMutableArray<RDNA4CommandSegment *> *segments = [NSMutableArray array];
        RDNA4SegmentRetireCommandBuffer *commandBuffer =
            newCommandBuffer(segments, nil);
        for (unsigned round = 0; round < 12; ++round) {
            RDNA4CommandSegment *segment = [RDNA4CommandSegment new];
            segment->vk = (VkCommandBuffer)(uintptr_t)(0x80u + round);
            [segments addObject:segment];
            [commandBuffer rdna4TestSetCurrentSegment:segment serial:10 + round];
            RDNA4EncoderRefsRenderEncoder *render =
                [RDNA4EncoderRefsRenderEncoder alloc];
            [render rdna4TestConfigureWithCommandBuffer:commandBuffer drawCache:NO];
            BOOL cached = round == 0;
            VkRenderPass base = [render rdna4TestCreateRenderHandlesCached:cached];
            [render rdna4TestBeginPassWithFetch:NO];
            [render rdna4ReleaseRenderHandles];
            require(liveFramebuffers.count == 1 && liveRenderPasses.count == 1,
                "encoder released its recorded handles before the fence");
            signalSegment(commandBuffer, segment);
            require(liveFramebuffers.count == 0 &&
                    liveRenderPasses.count == (cached ? 1u : 0u),
                "completed encoders accumulated render handles or destroyed cached pass");
            if (cached) vkDestroyRenderPass(RDNA4R.dev, base, NULL);
            render = nil;
        }
        disposeCommandBuffer(&commandBuffer);
    }
}

static void testRenderHandlesCancellationAndUnknownRetirement(void) {
    @autoreleasepool {
        RDNA4CommandSegment *unknown = [RDNA4CommandSegment new];
        unknown->vk = (VkCommandBuffer)(uintptr_t)0x91;
        unknown->retirementUnknown = YES;
        RDNA4CommandSegment *future = [RDNA4CommandSegment new];
        future->vk = (VkCommandBuffer)(uintptr_t)0x92;
        NSMutableArray<RDNA4CommandSegment *> *segments =
            [NSMutableArray arrayWithObjects:unknown, future, nil];
        RDNA4SegmentRetireCommandBuffer *commandBuffer =
            newCommandBuffer(segments, unknown);
        for (RDNA4CommandSegment *segment in segments) {
            [commandBuffer rdna4TestSetCurrentSegment:segment serial:30];
            RDNA4EncoderRefsRenderEncoder *render =
                [RDNA4EncoderRefsRenderEncoder alloc];
            [render rdna4TestConfigureWithCommandBuffer:commandBuffer drawCache:NO];
            [render rdna4TestCreateRenderHandlesCached:NO];
            [render rdna4TestBeginPassWithFetch:NO];
            [render rdna4ReleaseRenderHandles];
            render = nil;
        }
        require(liveFramebuffers.count == 2 && liveRenderPasses.count == 2,
            "quarantine fixture lost encoded handle owners");
        [commandBuffer rdna4CancelUnsubmittedArgumentIntents];
        require(unknown->commandRefs.count == 1 &&
                future->commandRefs.count == 0 &&
                liveFramebuffers.count == 1 && liveRenderPasses.count == 1,
            "cancellation destroyed unknown-retirement handles or retained future handles");
        [commandBuffer rdna4CancelUnsubmittedArgumentIntents];
        require(liveFramebuffers.count == 1 && liveRenderPasses.count == 1,
            "repeated cancellation destroyed quarantined handles");
        // Simulate an eventual known fence only to clean up the mock quarantine.
        signalSegment(commandBuffer, unknown);
        require(liveFramebuffers.count == 0 && liveRenderPasses.count == 0,
            "known completion retained the mock quarantined handle owner");
        disposeCommandBuffer(&commandBuffer);

        RDNA4CommandSegment *unsubmitted = [RDNA4CommandSegment new];
        NSMutableArray<RDNA4CommandSegment *> *deallocSegments =
            [NSMutableArray arrayWithObject:unsubmitted];
        commandBuffer = newCommandBuffer(deallocSegments, unsubmitted);
        RDNA4EncoderRefsRenderEncoder *render =
            [RDNA4EncoderRefsRenderEncoder alloc];
        [render rdna4TestConfigureWithCommandBuffer:commandBuffer drawCache:NO];
        [render rdna4TestCreateRenderHandlesCached:NO];
        [render rdna4TestSnapshot];
        [render rdna4ReleaseRenderHandles];
        render = nil;
        unsigned oldFramebufferDestroys = framebufferDestroys;
        commandBuffer = nil;
        require(liveFramebuffers.count == 0 && liveRenderPasses.count == 0 &&
                framebufferDestroys == oldFramebufferDestroys + 1,
            "unsubmitted command-buffer dealloc leaked or double-destroyed handles");
    }
}

static void testDescriptorPoolsAreSegmentOwned(void) {
    @autoreleasepool {
        RDNA4CommandSegment *first = [RDNA4CommandSegment new];
        first->vk = (VkCommandBuffer)(uintptr_t)0x64;
        first->hasWork = YES;
        RDNA4CommandSegment *second = [RDNA4CommandSegment new];
        second->vk = (VkCommandBuffer)(uintptr_t)0x65;
        second->hasWork = YES;
        NSMutableArray<RDNA4CommandSegment *> *segments =
            [NSMutableArray arrayWithObjects:first, second, nil];
        RDNA4SegmentRetireCommandBuffer *commandBuffer =
            newCommandBuffer(segments, first);
        [commandBuffer rdna4TestSetCurrentSegment:first serial:4];

        VkDescriptorSetLayout layout = (VkDescriptorSetLayout)(uintptr_t)0x31;
        require([commandBuffer rdna4AllocSet:layout] != VK_NULL_HANDLE &&
                [commandBuffer rdna4AllocSet:layout] != VK_NULL_HANDLE,
            "descriptor pool fixture allocation failed");
        require(first->descriptorPools.count == 1 &&
                descriptorPoolCreates == 1,
            "same-segment descriptor allocations used multiple pools");

        [commandBuffer rdna4TestSetCurrentSegment:second serial:5];
        descriptorAllocationFailuresRemaining = 1;
        require([commandBuffer rdna4AllocSet:layout] != VK_NULL_HANDLE,
            "descriptor allocation did not retry after a failed set");
        require(second->descriptorPools.count == 2 &&
                descriptorPoolCreates == 3,
            "failed descriptor-set allocation lost or duplicated pool ownership");

        signalSegment(commandBuffer, first);
        require(first->descriptorPools.count == 0 &&
                second->descriptorPools.count == 2 &&
                descriptorPoolDestroys == 1,
            "completing the first segment released the wrong descriptor pools");
        require([commandBuffer rdna4TestDescriptorPool] != VK_NULL_HANDLE,
            "completing the first segment cleared the current segment's pool");

        [commandBuffer rdna4PoolDone];
        require(second->descriptorPools.count == 0 &&
                descriptorPoolDestroys == descriptorPoolCreates &&
                [commandBuffer rdna4TestDescriptorPool] == VK_NULL_HANDLE,
            "pool completion leaked or double-freed segment pools");
        unsigned destroyed = descriptorPoolDestroys;
        [commandBuffer rdna4PoolDone];
        signalSegment(commandBuffer, second);
        require(descriptorPoolDestroys == destroyed,
            "pool completion or segment signal destroyed a pool twice");

        disposeCommandBuffer(&commandBuffer);
        require(liveDescriptorPools.count == 0,
            "descriptor pool fixture leaked a Vulkan pool");

        RDNA4CommandSegment *unsubmitted = [RDNA4CommandSegment new];
        unsubmitted->vk = (VkCommandBuffer)(uintptr_t)0x66;
        NSMutableArray<RDNA4CommandSegment *> *deallocSegments =
            [NSMutableArray arrayWithObject:unsubmitted];
        RDNA4SegmentRetireCommandBuffer *deallocCommandBuffer =
            newCommandBuffer(deallocSegments, unsubmitted);
        require([deallocCommandBuffer rdna4AllocSet:layout] != VK_NULL_HANDLE,
            "dealloc fixture descriptor allocation failed");
        unsigned creates = descriptorPoolCreates;
        unsigned destroys = descriptorPoolDestroys;
        deallocCommandBuffer = nil;
        require(descriptorPoolCreates == creates &&
                descriptorPoolDestroys == destroys + 1 &&
                liveDescriptorPools.count == 0,
            "unsubmitted command-buffer dealloc double-freed its segment pool");
    }
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        if (argc == 2 && !strcmp(argv[1], "--help")) {
            puts("Usage: test-argument-segment-retire\n"
                 "Runs Device-level segment retirement and summary fault "
                 "integration with mocked Vulkan; no VM or GPU execution.");
            return 0;
        }
        if (argc != 1) return 2;
        setupMocks();
        testCancellationPreservesSubmittedAndUnknown();
        testRealArgumentWriteReleasesBufferWhileCommandRetained();
        testSummaryAllocationFailureAndFutureCancel();
        testEncoderBindingsFollowSegments();
        testAttachmentBindingsFollowSegments();
        testComputeBindingsFollowSegments();
        testDeclarationsFollowSegments();
        testRenderHandlesFollowSegments();
        testRenderHandlesBoundedAcrossEncoders();
        testRenderHandlesCancellationAndUnknownRetirement();
        testDescriptorPoolsAreSegmentOwned();
        require(liveBuffers == 0, "mock Vulkan buffers leaked");
        require(liveMemory == 0, "mock Vulkan memory leaked");
        require(commandBufferFrees == expectedCommandBufferFrees,
            "unexpected command-buffer free count");
        printf("ARGUMENT_SEGMENT_RETIRE_PASS checks=%u mock_vulkan=1 "
               "mock_metal_lifecycle=1 gpu_execution=0\n",
               checks);
    }
    return 0;
}
