/*
 * Device-level argument submission fault fixture.
 *
 * This includes the production Device implementation and drives the real
 * command-buffer scheduler with a narrow Vulkan/queue test double. It covers:
 *   - vkCreateFence failure before submit: every unattempted segment rolls back,
 *   - one accepted segment followed by a pre-submit fence failure: no replay,
 *   - vkQueueSubmit failure: retire-unknown state is quarantined and retained,
 *   - a future segment rolls back while an unrelated owner survives,
 *   - summary allocation/descriptor/incomplete failure and later recovery,
 *   - repeated summary owners, descriptors and scratch have bounded live peaks.
 *
 * No Metal queue, GPU, VM, or real Vulkan device is used. A PASS here is host
 * scheduler/mocked-Vulkan evidence only.
 */
#include "GarnetDevice.m"

#include <stdatomic.h>
#include <spawn.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {
    size_t size;
    uint8_t *cpu;
} SubmitFaultMemory;

typedef struct {
    VkDeviceSize size;
    SubmitFaultMemory *memory;
} SubmitFaultBuffer;

static unsigned checks;
static unsigned liveBuffers;
static unsigned liveMemory;
static unsigned allocationCalls;
static unsigned failedAllocationAt;
static uint64_t liveMemoryBytes;
static uint64_t privateMemoryBytes;
static uint64_t peakMemoryBytes;
static uint64_t peakPrivateMemoryBytes;
static unsigned peakBuffers;
static unsigned liveSummaryPools;
static unsigned peakSummaryPools;
static BOOL failSummaryDescriptorAllocation;

static unsigned fenceCreateCalls;
static unsigned fenceCreateFailureAt;
static unsigned fenceDestroyCalls;
static unsigned fenceWaitCalls;
static VkResult fenceWaitResult = VK_SUCCESS;

static unsigned queueSubmitCalls;
static unsigned queueSubmitFailureAt;
static unsigned commandFreeCalls;
static unsigned commandFreeHandles;
static VkCommandBuffer submittedCommands[8];
static VkFence submittedFences[8];

static void require(BOOL value, const char *message) {
    ++checks;
    if (!value) {
        fprintf(stderr, "ARGUMENT_SUBMIT_FAULTS_FAIL %s\n", message);
        exit(1);
    }
}

static VkResult VKAPI_CALL submitFaultCreateBuffer(VkDevice device,
    const VkBufferCreateInfo *info, const VkAllocationCallbacks *allocator,
    VkBuffer *buffer) {
    (void)device;
    (void)allocator;
    if (!info || !buffer) return VK_ERROR_INITIALIZATION_FAILED;
    SubmitFaultBuffer *created = calloc(1, sizeof *created);
    if (!created) return VK_ERROR_OUT_OF_HOST_MEMORY;
    created->size = info->size;
    *buffer = (VkBuffer)(uintptr_t)created;
    ++liveBuffers;
    if (liveBuffers > peakBuffers) peakBuffers = liveBuffers;
    return VK_SUCCESS;
}

static void VKAPI_CALL submitFaultDestroyBuffer(VkDevice device,
    VkBuffer buffer, const VkAllocationCallbacks *allocator) {
    (void)device;
    (void)allocator;
    if (!buffer) return;
    free((void *)(uintptr_t)buffer);
    require(liveBuffers > 0, "mock buffer destroyed below zero");
    --liveBuffers;
}

static void VKAPI_CALL submitFaultBufferRequirements(VkDevice device,
    VkBuffer buffer, VkMemoryRequirements *requirements) {
    (void)device;
    SubmitFaultBuffer *created = (SubmitFaultBuffer *)(uintptr_t)buffer;
    require(created != NULL, "buffer requirements received a null buffer");
    *requirements = (VkMemoryRequirements){
        .size = (created->size + 4095u) & ~(VkDeviceSize)4095u,
        .alignment = 4096,
        .memoryTypeBits = 3,
    };
}

static VkResult VKAPI_CALL submitFaultAllocateMemory(VkDevice device,
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
    SubmitFaultMemory *created = calloc(1, sizeof *created);
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
    liveMemoryBytes += created->size;
    if (!created->cpu) privateMemoryBytes += created->size;
    if (liveMemoryBytes > peakMemoryBytes)
        peakMemoryBytes = liveMemoryBytes;
    if (privateMemoryBytes > peakPrivateMemoryBytes)
        peakPrivateMemoryBytes = privateMemoryBytes;
    return VK_SUCCESS;
}

static void VKAPI_CALL submitFaultFreeMemory(VkDevice device,
    VkDeviceMemory memory, const VkAllocationCallbacks *allocator) {
    (void)device;
    (void)allocator;
    if (!memory) return;
    SubmitFaultMemory *created = (SubmitFaultMemory *)(uintptr_t)memory;
    liveMemoryBytes -= created->size;
    if (!created->cpu) privateMemoryBytes -= created->size;
    free(created->cpu);
    free(created);
    require(liveMemory > 0, "mock memory freed below zero");
    --liveMemory;
}

static VkResult VKAPI_CALL submitFaultBindMemory(VkDevice device,
    VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset) {
    (void)device;
    SubmitFaultBuffer *created = (SubmitFaultBuffer *)(uintptr_t)buffer;
    require(created != NULL, "bind received a null buffer");
    require(offset == 0, "fixture only binds at offset zero");
    created->memory = (SubmitFaultMemory *)(uintptr_t)memory;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL submitFaultMapMemory(VkDevice device,
    VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size,
    VkMemoryMapFlags flags, void **pointer) {
    (void)device;
    (void)offset;
    (void)size;
    (void)flags;
    if (!pointer) return VK_ERROR_INITIALIZATION_FAILED;
    SubmitFaultMemory *created = (SubmitFaultMemory *)(uintptr_t)memory;
    *pointer = created ? created->cpu : NULL;
    return *pointer ? VK_SUCCESS : VK_ERROR_MEMORY_MAP_FAILED;
}

static VkResult VKAPI_CALL submitFaultCreateFence(VkDevice device,
    const VkFenceCreateInfo *info, const VkAllocationCallbacks *allocator,
    VkFence *fence) {
    (void)device;
    (void)info;
    (void)allocator;
    if (!fence) return VK_ERROR_INITIALIZATION_FAILED;
    ++fenceCreateCalls;
    if (fenceCreateFailureAt &&
        fenceCreateCalls == fenceCreateFailureAt) {
        fenceCreateFailureAt = 0;
        *fence = VK_NULL_HANDLE;
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    *fence = (VkFence)(uintptr_t)(0xf000u + fenceCreateCalls);
    return VK_SUCCESS;
}

static void VKAPI_CALL submitFaultDestroyFence(VkDevice device,
    VkFence fence, const VkAllocationCallbacks *allocator) {
    (void)device;
    (void)allocator;
    require(fence != VK_NULL_HANDLE,
        "vkDestroyFence received a null fence");
    ++fenceDestroyCalls;
}

static VkResult VKAPI_CALL submitFaultQueueSubmit(VkQueue queue,
    uint32_t count, const VkSubmitInfo *submits, VkFence fence) {
    (void)queue;
    require(count == 1 && submits != NULL,
        "vkQueueSubmit did not receive one submit");
    require(submits[0].commandBufferCount == 1 &&
            submits[0].pCommandBuffers != NULL,
        "vkQueueSubmit did not receive one command buffer");
    require(submits[0].pCommandBuffers[0] != VK_NULL_HANDLE,
        "vkQueueSubmit received a null command buffer");
    require(fence != VK_NULL_HANDLE,
        "vkQueueSubmit received a null fence");
    ++queueSubmitCalls;
    unsigned slot = queueSubmitCalls - 1u;
    require(slot < sizeof submittedCommands / sizeof submittedCommands[0],
        "vkQueueSubmit record overflow");
    submittedCommands[slot] = submits[0].pCommandBuffers[0];
    submittedFences[slot] = fence;
    if (queueSubmitFailureAt && queueSubmitCalls == queueSubmitFailureAt) {
        queueSubmitFailureAt = 0;
        return VK_ERROR_DEVICE_LOST;
    }
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL submitFaultWaitForFences(VkDevice device,
    uint32_t count, const VkFence *fences, VkBool32 waitAll,
    uint64_t timeout) {
    (void)device;
    (void)timeout;
    require(count == 1 && fences != NULL && fences[0] != VK_NULL_HANDLE,
        "vkWaitForFences did not receive one fence");
    require(waitAll == VK_TRUE, "vkWaitForFences did not wait for all");
    ++fenceWaitCalls;
    return fenceWaitResult;
}

static void VKAPI_CALL submitFaultFreeCommandBuffers(VkDevice device,
    VkCommandPool pool, uint32_t count, const VkCommandBuffer *commands) {
    (void)device;
    require(pool != VK_NULL_HANDLE,
        "vkFreeCommandBuffers received a null pool");
    require(count > 0 && commands != NULL,
        "vkFreeCommandBuffers received no commands");
    ++commandFreeCalls;
    for (uint32_t index = 0; index < count; ++index) {
        require(commands[index] != VK_NULL_HANDLE,
            "vkFreeCommandBuffers received a null command");
        ++commandFreeHandles;
    }
}

/* The owner fixture supplies an explicit bitset oracle, not GPU execution. */
static VkResult VKAPI_CALL summaryCreatePool(VkDevice device,
    const VkDescriptorPoolCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkDescriptorPool *pool) {
    (void)device;
    (void)allocator;
    require(info->maxSets == 1 && info->poolSizeCount == 1 &&
            info->pPoolSizes[0].descriptorCount == 3,
        "summary descriptor capacity changed");
    *pool = (VkDescriptorPool)(uintptr_t)0x5100;
    ++liveSummaryPools;
    if (liveSummaryPools > peakSummaryPools)
        peakSummaryPools = liveSummaryPools;
    return VK_SUCCESS;
}

static void VKAPI_CALL summaryDestroyPool(VkDevice device,
    VkDescriptorPool pool, const VkAllocationCallbacks *allocator) {
    (void)device;
    (void)allocator;
    require(pool != VK_NULL_HANDLE && liveSummaryPools > 0,
        "summary pool release without allocation");
    --liveSummaryPools;
}

static VkResult VKAPI_CALL summaryAllocateSets(VkDevice device,
    const VkDescriptorSetAllocateInfo *info, VkDescriptorSet *sets) {
    (void)device;
    require(info->descriptorSetCount == 1,
        "summary requested more than one descriptor set");
    if (failSummaryDescriptorAllocation) {
        failSummaryDescriptorAllocation = NO;
        return VK_ERROR_OUT_OF_POOL_MEMORY;
    }
    *sets = (VkDescriptorSet)(uintptr_t)0x5200;
    return VK_SUCCESS;
}

static void VKAPI_CALL summaryUpdateSets(VkDevice device,
    uint32_t count, const VkWriteDescriptorSet *writes,
    uint32_t copyCount, const VkCopyDescriptorSet *copies) {
    (void)device;
    (void)copies;
    require(count == 3 && copyCount == 0,
        "summary descriptor write count");
    for (uint32_t index = 0; index < count; ++index)
        require(writes[index].pBufferInfo->buffer != VK_NULL_HANDLE &&
                writes[index].pBufferInfo->range > 0,
            "summary descriptor has no backing storage");
}

static void VKAPI_CALL summaryBindPipeline(VkCommandBuffer command,
    VkPipelineBindPoint point, VkPipeline pipeline) {
    require(command && point == VK_PIPELINE_BIND_POINT_COMPUTE && pipeline,
        "summary bind pipeline");
}

static void VKAPI_CALL summaryBindSets(VkCommandBuffer command,
    VkPipelineBindPoint point, VkPipelineLayout layout, uint32_t first,
    uint32_t count, const VkDescriptorSet *sets,
    uint32_t offsetCount, const uint32_t *offsets) {
    (void)offsets;
    require(command && point == VK_PIPELINE_BIND_POINT_COMPUTE && layout &&
            first == 0 && count == 1 && sets[0] && offsetCount == 0,
        "summary bind descriptor set");
}

static void VKAPI_CALL summaryCopy(VkCommandBuffer command, VkBuffer source,
    VkBuffer destination, uint32_t count, const VkBufferCopy *regions) {
    require(command && source && destination && count == 1 && regions->size,
        "summary copy recording");
}

static void VKAPI_CALL summaryFill(VkCommandBuffer command, VkBuffer buffer,
    VkDeviceSize offset, VkDeviceSize size, uint32_t value) {
    (void)offset;
    require(command && buffer && size && value == 0,
        "summary fill recording");
}

static void VKAPI_CALL summaryPush(VkCommandBuffer command,
    VkPipelineLayout layout, VkShaderStageFlags stages,
    uint32_t offset, uint32_t size, const void *values) {
    require(command && layout && stages == VK_SHADER_STAGE_COMPUTE_BIT &&
            offset == 0 && size == sizeof(rdna4_argument_summary_push) && values,
        "summary push recording");
}

static void VKAPI_CALL summaryDispatch(VkCommandBuffer command,
    uint32_t x, uint32_t y, uint32_t z) {
    require(command && x && y == 1 && z == 1,
        "summary dispatch recording");
}

static void VKAPI_CALL summaryBarrier(VkCommandBuffer command,
    VkPipelineStageFlags source, VkPipelineStageFlags destination,
    VkDependencyFlags flags, uint32_t memoryCount,
    const VkMemoryBarrier *memory, uint32_t bufferCount,
    const VkBufferMemoryBarrier *buffers, uint32_t imageCount,
    const VkImageMemoryBarrier *images) {
    (void)flags;
    (void)memory;
    (void)bufferCount;
    (void)buffers;
    (void)imageCount;
    (void)images;
    require(command && source && destination && memoryCount == 1,
        "summary barrier recording");
}

@interface SubmitFaultSentinel : NSObject
@end

@implementation SubmitFaultSentinel
@end

@interface SubmitFaultDevice : NSObject
- (rdna4dc_caps)rdna4MemoryCaps;
@end
@implementation SubmitFaultDevice
- (rdna4dc_caps)rdna4MemoryCaps { return RDNA4Caps; }
@end

@interface SubmitFaultCommandBuffer : RDNA4CommandBuffer
- (BOOL)rdna4TestPrepareSegments:
            (NSMutableArray<RDNA4CommandSegment *> *)segments
                        queue:(RDNA4CommandQueue *)queue;
- (BOOL)rdna4TestSubmitAttempted;
- (NSUInteger)rdna4TestSegmentCount;
- (uint64_t)rdna4TestOpenHeapTimeline;
@end

@implementation SubmitFaultCommandBuffer
- (BOOL)rdna4TestPrepareSegments:
            (NSMutableArray<RDNA4CommandSegment *> *)segments
                        queue:(RDNA4CommandQueue *)queue {
    if (pthread_mutex_init(&_smu, NULL) != 0) return NO;
    _segments = segments;
    _curSegment = segments.lastObject;
    _nextSegment = 0;
    _nq = queue;
    _prepared = YES;
    _dslot = -1;
    _startTime = rdna4_now();
    _vk = _curSegment ? _curSegment->vk : VK_NULL_HANDLE;
    return YES;
}

- (BOOL)rdna4TestSubmitAttempted {
    return _submitAttempted;
}

- (NSUInteger)rdna4TestSegmentCount {
    return _segments.count;
}

- (uint64_t)rdna4TestOpenHeapTimeline {
    _pser = rdna4_fence_open();
    return _pser;
}

/* The test object has no real Metal queue from which to report scheduling. */
- (void)didScheduleWithStartTime:(uint64_t)start
                         endTime:(uint64_t)end
                           error:(NSError *)error {
    (void)start;
    (void)end;
    (void)error;
}
@end

@interface SubmitFaultQueue : NSObject {
    dispatch_semaphore_t _kick;
    dispatch_semaphore_t _complete;
}
@property(readonly) unsigned completionCalls;
@property(readonly) NSError *lastCompletionError;
- (void)waitForKick;
- (void)waitForCompletion;
@end

static dispatch_queue_t submitFaultCompletionQueue(void) {
    static dispatch_queue_t queue;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        queue = dispatch_queue_create("rdna4.submit-faults", NULL);
    });
    return queue;
}

@implementation SubmitFaultQueue
- (instancetype)init {
    self = [super init];
    if (!self) return nil;
    _kick = dispatch_semaphore_create(0);
    _complete = dispatch_semaphore_create(0);
    return self;
}

- (VkCommandPool)pool {
    return (VkCommandPool)(uintptr_t)0x200;
}

- (dispatch_queue_t)rdna4CompletionQueue {
    return submitFaultCompletionQueue();
}

- (id)device {
    return self;
}

- (void)rdna4Kick {
    dispatch_semaphore_signal(_kick);
}

- (void)commandBufferDidComplete:(id)commandBuffer
                        startTime:(uint64_t)start
                   completionTime:(uint64_t)completion
                            error:(NSError *)error {
    (void)commandBuffer;
    (void)start;
    (void)completion;
    _completionCalls++;
    _lastCompletionError = error;
}

- (void)rdna4DidComplete:(RDNA4CommandBuffer *)commandBuffer {
    (void)commandBuffer;
    dispatch_semaphore_signal(_complete);
}

- (void)waitForKick {
    require(dispatch_semaphore_wait(_kick,
            dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) == 0,
        "timed out waiting for the accepted segment to complete");
    dispatch_sync(submitFaultCompletionQueue(), ^{});
}

- (void)waitForCompletion {
    require(dispatch_semaphore_wait(_complete,
            dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) == 0,
        "timed out waiting for terminal completion");
    dispatch_sync(submitFaultCompletionQueue(), ^{});
}
@end

static SubmitFaultCommandBuffer *newCommandBuffer(
    NSMutableArray<RDNA4CommandSegment *> *segments,
    SubmitFaultQueue *queue) {
    /*
     * -initWithQueue: allocates a Vulkan segment. +alloc zero-initializes the
     * inherited ivars; the narrow setter supplies only scheduler state.
     */
    SubmitFaultCommandBuffer *commandBuffer =
        [SubmitFaultCommandBuffer alloc];
    require(commandBuffer != nil, "command-buffer test object allocation");
    require([commandBuffer rdna4TestPrepareSegments:segments
                                               queue:(RDNA4CommandQueue *)queue],
        "command-buffer scheduler state initialization");
    return commandBuffer;
}

static void disposeCommandBuffer(
    SubmitFaultCommandBuffer * __strong *commandBuffer) {
    if (!*commandBuffer) return;
    *commandBuffer = nil;
}

static RDNA4Buffer *newBuffer(id device, NSUInteger length,
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

static RDNA4CommandSegment *newSegment(VkCommandBuffer command,
    RDNA4ArgumentWriteIntent *intent, BOOL terminal) {
    RDNA4CommandSegment *segment = [RDNA4CommandSegment new];
    segment->vk = command;
    segment->ended = YES;
    segment->hasWork = YES;
    segment->terminal = terminal;
    if (intent) [segment->argumentIntents addObject:intent];
    return segment;
}

static void addSegmentSentinels(RDNA4CommandSegment *segment) {
    SubmitFaultSentinel *attempt = [SubmitFaultSentinel new];
    SubmitFaultSentinel *ref = [SubmitFaultSentinel new];
    SubmitFaultSentinel *pool = [SubmitFaultSentinel new];
    segment->argumentSummaryAttempts = @[attempt];
    [segment->argumentRefs addObject:ref];
    [segment->argumentPools addObject:pool];
}

static void setupMocks(void) {
    RDNA4R.tried = YES;
    RDNA4R.ok = YES;
    RDNA4R.dev = (VkDevice)(uintptr_t)1;
    RDNA4R.q = (VkQueue)(uintptr_t)2;
    RDNA4R.bufferAddress = NO;
    pthread_mutex_init(&RDNA4R.qlock, NULL);
    RDNA4R.mp.memoryTypeCount = 2;
    RDNA4R.mp.memoryTypes[0].propertyFlags =
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
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

    vkCreateBuffer = submitFaultCreateBuffer;
    vkDestroyBuffer = submitFaultDestroyBuffer;
    vkGetBufferMemoryRequirements = submitFaultBufferRequirements;
    vkAllocateMemory = submitFaultAllocateMemory;
    vkFreeMemory = submitFaultFreeMemory;
    vkBindBufferMemory = submitFaultBindMemory;
    vkMapMemory = submitFaultMapMemory;

    vkCreateFence = submitFaultCreateFence;
    vkDestroyFence = submitFaultDestroyFence;
    vkQueueSubmit = submitFaultQueueSubmit;
    vkWaitForFences = submitFaultWaitForFences;
    vkFreeCommandBuffers = submitFaultFreeCommandBuffers;
    vkCreateDescriptorPool = summaryCreatePool;
    vkDestroyDescriptorPool = summaryDestroyPool;
    vkAllocateDescriptorSets = summaryAllocateSets;
    vkUpdateDescriptorSets = summaryUpdateSets;
    vkCmdBindPipeline = summaryBindPipeline;
    vkCmdBindDescriptorSets = summaryBindSets;
    vkCmdCopyBuffer = summaryCopy;
    vkCmdFillBuffer = summaryFill;
    vkCmdPushConstants = summaryPush;
    vkCmdDispatch = summaryDispatch;
    vkCmdPipelineBarrier = summaryBarrier;
    /* Pipeline construction has a separate runtime test; use its ready state. */
    rdna4_argument_summary_pipeline = (rdna4_argument_summary_pipeline_state){
        .ready = true, .device_key = (uintptr_t)RDNA4R.dev,
        .shader = 1, .descriptor_layout = 2, .pipeline_layout = 3,
        .pipeline = 4,
    };
}

static void testFenceFailureRollsBackEverySegment(void) {
    @autoreleasepool {
        NSError *error = nil;
        id device = [NSObject new];
        id sampler = [NSObject new];
        RDNA4Buffer *firstBuffer =
            newBuffer(device, 64, MTLResourceStorageModePrivate, &error);
        RDNA4Buffer *secondBuffer =
            newBuffer(device, 64, MTLResourceStorageModePrivate, &error);
        require(firstBuffer != nil && secondBuffer != nil && !error,
            "pre-submit fence failure buffers");
        RDNA4ArgumentWriteIntent *firstIntent =
            newIntent(firstBuffer, sampler);
        RDNA4ArgumentWriteIntent *secondIntent =
            newIntent(secondBuffer, sampler);
        require(firstIntent != nil && secondIntent != nil,
            "pre-submit fence failure intents");

        RDNA4CommandSegment *first = newSegment(
            (VkCommandBuffer)(uintptr_t)0x1001, firstIntent, NO);
        RDNA4CommandSegment *second = newSegment(
            (VkCommandBuffer)(uintptr_t)0x1002, secondIntent, YES);
        addSegmentSentinels(first);
        addSegmentSentinels(second);
        __weak id weakFirstAttempt = first->argumentSummaryAttempts[0];
        __weak id weakFirstRef = first->argumentRefs.anyObject;
        __weak id weakFirstPool = first->argumentPools[0];
        __weak id weakSecondAttempt = second->argumentSummaryAttempts[0];
        __weak id weakSecondRef = second->argumentRefs.anyObject;
        __weak id weakSecondPool = second->argumentPools[0];

        SubmitFaultQueue *queue = [SubmitFaultQueue new];
        require(queue != nil, "pre-submit fence failure queue");
        SubmitFaultCommandBuffer *commandBuffer = newCommandBuffer(
            [NSMutableArray arrayWithObjects:first, second, nil], queue);
        __weak SubmitFaultCommandBuffer *weakCommandBuffer = commandBuffer;

        unsigned createBefore = fenceCreateCalls;
        unsigned submitBefore = queueSubmitCalls;
        unsigned waitBefore = fenceWaitCalls;
        unsigned freeBefore = commandFreeCalls;
        unsigned handlesBefore = commandFreeHandles;
        fenceCreateFailureAt = createBefore + 1;
        BOOL advanced = [commandBuffer
            rdna4AdvanceWithQueue:(RDNA4CommandQueue *)queue];
        require(!advanced, "pre-submit fence failure was accepted");
        require(fenceCreateCalls == createBefore + 1,
            "pre-submit fence failure call count");
        require(queueSubmitCalls == submitBefore,
            "vkQueueSubmit ran after vkCreateFence failed");
        require(fenceWaitCalls == waitBefore,
            "fence wait ran after vkCreateFence failed");
        require(![commandBuffer rdna4TestSubmitAttempted],
            "pre-submit fence failure marked the command buffer attempted");
        require(first->fence == VK_NULL_HANDLE &&
                second->fence == VK_NULL_HANDLE,
            "pre-submit fence failure installed a fence");
        require(!first->submitted && !second->submitted,
            "pre-submit fence failure marked a segment submitted");
        require(!first->retirementUnknown && !second->retirementUnknown,
            "pre-submit fence failure marked retirement unknown");
        require(atomic_load(&rdna4_submit_fault) == VK_SUCCESS,
            "pre-submit fence failure set the global submit fault");
        require(!firstIntent->active && !secondIntent->active,
            "pre-submit fence failure retained an active intent");
        require(first->argumentIntents.count == 0 &&
                second->argumentIntents.count == 0,
            "pre-submit fence failure retained segment intents");
        require(first->argumentRefs.count == 0 &&
                second->argumentRefs.count == 0,
            "pre-submit fence failure retained segment refs");
        require(first->argumentPools.count == 0 &&
                second->argumentPools.count == 0,
            "pre-submit fence failure retained segment pools");
        require(first->argumentSummaryAttempts == nil &&
                second->argumentSummaryAttempts == nil,
            "pre-submit fence failure retained summary attempts");
        require(weakFirstAttempt == nil && weakFirstRef == nil &&
                weakFirstPool == nil && weakSecondAttempt == nil &&
                weakSecondRef == nil && weakSecondPool == nil,
            "pre-submit fence failure leaked segment-owned objects");
        [queue waitForCompletion];
        require(queue.completionCalls == 1 &&
                queue.lastCompletionError != nil,
            "pre-submit fence failure did not complete with an error");

        unsigned freeExpected =
            (unsigned)[commandBuffer rdna4TestSegmentCount];
        disposeCommandBuffer(&commandBuffer);
        require(weakCommandBuffer == nil,
            "pre-submit fence failure command buffer did not deallocate");
        require(commandFreeCalls == freeBefore + 1,
            "pre-submit fence failure free call count");
        require(commandFreeHandles == handlesBefore + freeExpected,
            "pre-submit fence failure did not free both commands");
        firstBuffer = nil;
        secondBuffer = nil;
        device = nil;
        sampler = nil;
    }
}

static void testAcceptedSegmentIsNotReplayed(void) {
    @autoreleasepool {
        NSError *error = nil;
        id device = [NSObject new];
        id sampler = [NSObject new];
        RDNA4Buffer *acceptedBuffer =
            newBuffer(device, 64, MTLResourceStorageModePrivate, &error);
        RDNA4Buffer *futureBuffer =
            newBuffer(device, 64, MTLResourceStorageModePrivate, &error);
        require(acceptedBuffer != nil && futureBuffer != nil && !error,
            "accepted-segment buffers");
        RDNA4ArgumentWriteIntent *acceptedIntent =
            newIntent(acceptedBuffer, sampler);
        RDNA4ArgumentWriteIntent *futureIntent =
            newIntent(futureBuffer, sampler);
        require(acceptedIntent != nil && futureIntent != nil,
            "accepted-segment intents");

        RDNA4CommandSegment *accepted = newSegment(
            (VkCommandBuffer)(uintptr_t)0x2001, acceptedIntent, NO);
        RDNA4CommandSegment *future = newSegment(
            (VkCommandBuffer)(uintptr_t)0x2002, futureIntent, YES);
        SubmitFaultSentinel *acceptedRef = [SubmitFaultSentinel new];
        SubmitFaultSentinel *acceptedPool = [SubmitFaultSentinel new];
        __weak id weakAcceptedRef = acceptedRef;
        __weak id weakAcceptedPool = acceptedPool;
        [accepted->argumentRefs addObject:acceptedRef];
        [accepted->argumentPools addObject:acceptedPool];
        acceptedRef = nil;
        acceptedPool = nil;
        addSegmentSentinels(future);
        __weak id weakFutureAttempt = future->argumentSummaryAttempts[0];
        __weak id weakFutureRef = future->argumentRefs.anyObject;
        __weak id weakFuturePool = future->argumentPools[0];

        SubmitFaultQueue *queue = [SubmitFaultQueue new];
        require(queue != nil, "accepted-segment queue");
        SubmitFaultCommandBuffer *commandBuffer = newCommandBuffer(
            [NSMutableArray arrayWithObjects:accepted, future, nil], queue);
        __weak SubmitFaultCommandBuffer *weakCommandBuffer = commandBuffer;

        unsigned freeBefore = commandFreeCalls;
        unsigned handlesBefore = commandFreeHandles;
        unsigned fencesBefore = fenceDestroyCalls;
        BOOL advanced = [commandBuffer
            rdna4AdvanceWithQueue:(RDNA4CommandQueue *)queue];
        require(!advanced, "accepted segment returned a wait");
        require(queueSubmitCalls == 1,
            "accepted segment was not submitted exactly once");
        require(submittedCommands[0] ==
                (VkCommandBuffer)(uintptr_t)0x2001,
            "accepted segment submitted the wrong command");
        require(submittedFences[0] == accepted->fence &&
                accepted->fence != VK_NULL_HANDLE,
            "accepted segment submitted the wrong fence");
        [queue waitForKick];
        require(queue.completionCalls == 0,
            "nonterminal accepted segment completed the command buffer");
        require(accepted->submitted && accepted->done &&
                !accepted->retirementUnknown,
            "accepted segment did not reach completion");
        require(!acceptedIntent->active && futureIntent->active,
            "accepted/future intent state before second advance");
        require(accepted->argumentIntents.count == 0 &&
                accepted->argumentRefs.count == 0 &&
                accepted->argumentPools.count == 0,
            "accepted segment retained per-segment ownership");
        require(accepted->vk == VK_NULL_HANDLE,
            "accepted segment retained its command buffer");
        require(weakAcceptedRef == nil && weakAcceptedPool == nil,
            "accepted segment leaked refs or pools");
        require(futureIntent->active,
            "unattempted future intent was abandoned before its fence");
        require(commandFreeCalls == freeBefore + 1,
            "accepted segment did not free exactly once");

        unsigned createBefore = fenceCreateCalls;
        unsigned submitBefore = queueSubmitCalls;
        fenceCreateFailureAt = createBefore + 1;
        advanced = [commandBuffer
            rdna4AdvanceWithQueue:(RDNA4CommandQueue *)queue];
        require(!advanced, "future fence failure was accepted");
        require(fenceCreateCalls == createBefore + 1,
            "future fence failure call count");
        require(queueSubmitCalls == submitBefore,
            "accepted command was replayed after a future fence failure");
        require(submittedCommands[0] ==
                (VkCommandBuffer)(uintptr_t)0x2001,
            "accepted command buffer changed after replay check");
        require(!future->submitted && !future->retirementUnknown,
            "unattempted future segment was marked submitted or unknown");
        require(!futureIntent->active,
            "unattempted future intent remained active");
        require(future->argumentIntents.count == 0 &&
                future->argumentRefs.count == 0 &&
                future->argumentPools.count == 0 &&
                future->argumentSummaryAttempts == nil,
            "unattempted future segment retained ownership");
        require(weakFutureAttempt == nil && weakFutureRef == nil &&
                weakFuturePool == nil,
            "unattempted future segment leaked owned objects");
        [queue waitForCompletion];
        require(queue.completionCalls == 1 &&
                queue.lastCompletionError != nil,
            "future fence failure did not complete with an error");

        disposeCommandBuffer(&commandBuffer);
        require(weakCommandBuffer == nil,
            "accepted-segment command buffer did not deallocate");
        require(commandFreeCalls == freeBefore + 2,
            "accepted-segment free call count");
        require(commandFreeHandles == handlesBefore + 2,
            "accepted-segment fixture did not free both commands");
        require(fenceDestroyCalls == fencesBefore + 1,
            "accepted-segment fence was not destroyed exactly once");
        acceptedBuffer = nil;
        futureBuffer = nil;
        device = nil;
        sampler = nil;
    }
}

static void expectSummaryAllocationFailure(
    RDNA4ArgumentWriteIntent *pending, RDNA4Buffer *buffer,
    unsigned allocationOffset) {
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
        @[pending], (VkCommandBuffer)(uintptr_t)0x3001, &attempts, &error);
    require(!encoded, "summary allocation failure was accepted");
    require(error != nil, "summary allocation failure had no bounded error");
    require(failedAllocationAt == 0,
        "summary allocation failure was not injected");
    require(attempts.count == 0,
        "failed summary exposed a partial attempt");
    require(pending->active,
        "failed summary abandoned its pending intent");
    require(owners.writers.count == 1,
        "failed summary removed the pending owner");
    require(owners.latestSummaryEpoch == latest,
        "failed summary advanced latestSummaryEpoch");
    require(owners.lastCompletedEpoch == completed,
        "failed summary changed lastCompletedEpoch");
}

static void testSummaryAllocationFailureKeepsOldOwner(void) {
    @autoreleasepool {
        NSError *error = nil;
        id device = [NSObject new];
        id sampler = [NSObject new];
        RDNA4Buffer *buffer =
            newBuffer(device, 64, MTLResourceStorageModePrivate, &error);
        require(buffer != nil && !error, "summary owner buffer");
        require(rdna4_argument_resource_token(sampler, device, NO) != 0,
            "summary owner sampler token");

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
         * Fault the candidate dictionary allocation and then the output
         * allocation. Both occur before any summary command is recorded.
         */
        expectSummaryAllocationFailure(pending, buffer, 1);
        expectSummaryAllocationFailure(pending, buffer, 2);
        require(owners.possible.count == 1,
            "summary failure removed the old persistent owner");

        rdna4_argument_sampler_abandon(pending);
        require(!pending->active && owners.writers.count == 0,
            "pending summary writer was not cleaned up");
        require(owners.possible.count == 1,
            "pending cleanup removed the old persistent owner");
        buffer = nil;
        device = nil;
        sampler = nil;
    }
}

static NSUInteger summaryOwnerCount(RDNA4ArgumentSamplerOwners *owners) {
    /* Observer snapshots must drain before the weak retirement assertion. */
    @autoreleasepool {
        NSMutableSet *objects = [NSMutableSet set];
        for (NSArray *slot in owners.slots.allValues)
            if (slot.count > 1) [objects addObject:slot[1]];
        for (NSSet *possible in owners.possible.allValues)
            [objects unionSet:possible];
        for (RDNA4ArgumentWriteIntent *writer in owners.writers)
            [objects unionSet:writer->candidates];
        for (RDNA4ArgumentCompletedWrite *write in owners.completed)
            [objects unionSet:write->candidates];
        return objects.count;
    }
}

static NSArray *encodeSummaryOracle(RDNA4ArgumentWriteIntent *intent,
    uint64_t liveToken) {
    NSError *error = nil;
    NSArray *attempts = nil;
    require(rdna4_argument_summary_owners_encode(@[intent],
            (VkCommandBuffer)(uintptr_t)0x6001, &attempts, &error) && !error,
        "summary retry encoding failed");
    require(attempts.count == 1, "summary retry returned no owner attempt");
    RDNA4ArgumentSummaryOwnerAttempt *ownerAttempt = attempts[0];
    require(ownerAttempt->captures.count == 1,
        "summary retry returned no complete capture");
    RDNA4ArgumentSummaryOwnerCapture *capture = ownerAttempt->captures[0];
    require(capture->buffer.contents == NULL,
        "Private summary exposed host contents");
    require(capture->attempt->_sourceScratch != nil &&
            capture->attempt->_dictionaryScratch != nil &&
            capture->attempt->_outputScratch != nil &&
            capture->attempt->_descriptorPool != VK_NULL_HANDLE,
        "summary did not allocate the production scratch/descriptor graph");
    uint32_t *words = capture->task.outputBuffer.contents;
    memset(words, 0, capture->task.outputBuffer.length);
    BOOL found = liveToken == 0;
    for (NSUInteger index = 0; index < capture->tokens.count; ++index) {
        if (capture->tokenValues[index] != liveToken) continue;
        words[index / 32u] |= UINT32_C(1) << (index % 32u);
        found = YES;
    }
    require(found, "oracle token was absent from captured candidates");
    return attempts;
}

static void finishSummary(NSArray *attempts, uint64_t epoch) {
    NSError *error = nil;
    for (RDNA4ArgumentSummaryOwnerAttempt *attempt in attempts) {
        require(rdna4_argument_summary_owners_finish(attempt, YES, epoch,
                &error) && !error, "summary retry publication failed");
        require(attempt->finished && attempt->captures.count == 0,
            "finished summary retained its capture graph");
    }
}

static void testSummaryHostGenerationInterleave(void) {
    @autoreleasepool {
        NSError *error = nil;
        id device = [NSObject new];
        RDNA4Buffer *buffer =
            newBuffer(device, 64, MTLResourceStorageModePrivate, &error);
        require(buffer && !error, "generation race buffer");
        __weak id weakOld = nil;
        __weak id weakNew = nil;
        uint64_t newToken = 0;
        RDNA4ArgumentSamplerOwners *owners = nil;
        @autoreleasepool {
            id old = [SubmitFaultSentinel new];
            weakOld = old;
            require(rdna4_argument_resource_token(old, device, NO) != 0,
                "generation race old token");
            RDNA4ArgumentWriteIntent *initial = newIntent(buffer, old);
            rdna4_argument_sampler_submit(initial, 1);
            rdna4_argument_sampler_complete(initial);
            RDNA4ArgumentWriteIntent *pending = newIntent(buffer, nil);
            NSArray *attempts = encodeSummaryOracle(pending, 0);
            RDNA4ArgumentSummaryOwnerAttempt *attempt = attempts[0];
            RDNA4ArgumentSummaryOwnerCapture *capture = attempt->captures[0];
            owners = rdna4_argument_sampler_owners(buffer, NO);
            uint64_t capturedGeneration = capture->hostGeneration;
            id replacement = [SubmitFaultSentinel new];
            weakNew = replacement;
            newToken = rdna4_argument_resource_token(replacement, device, NO);
            require(newToken != 0, "generation race replacement token");
            /* Exercise the setter's metadata half; Private bytes stay unmapped. */
            @synchronized (buffer) {
                (void)rdna4_argument_sampler_note_host_write_locked(owners,
                    NSMakeRange(0, buffer.length));
                rdna4_argument_sampler_fill_locked(owners,
                    NSMakeRange(0, buffer.length));
                owners.slots[@0] = @[@(newToken), replacement];
            }
            require(buffer.contents == NULL &&
                    owners.hostGeneration == capturedGeneration + 1u,
                "generation race did not advance the atomic metadata update");
            rdna4_argument_sampler_submit(pending, 2);
            rdna4_argument_sampler_complete(pending);
            finishSummary(attempts, 2);
            require(owners.latestSummaryEpoch == 0 &&
                    owners.slots[@0][1] == replacement &&
                    summaryOwnerCount(owners) == 1 &&
                    owners.writers.count == 0,
                "stale capture overwrote the newer CPU owner metadata");
        }
        require(weakOld == nil && liveBuffers == 1 && liveSummaryPools == 0,
            "stale summary retained its old capture graph");
        @autoreleasepool {
            require(weakNew != nil, "stale summary lost the new owner");
            RDNA4ArgumentWriteIntent *retry = newIntent(buffer, nil);
            NSArray *attempts = encodeSummaryOracle(retry, newToken);
            rdna4_argument_sampler_submit(retry, 3);
            rdna4_argument_sampler_complete(retry);
            finishSummary(attempts, 3);
            require(owners.latestSummaryEpoch == 3 &&
                    summaryOwnerCount(owners) == 1 && weakNew,
                "later matching generation did not publish the new owner");
        }
        @autoreleasepool {
            RDNA4ArgumentWriteIntent *clear = newIntent(buffer, nil);
            NSArray *attempts = encodeSummaryOracle(clear, 0);
            rdna4_argument_sampler_submit(clear, 4);
            rdna4_argument_sampler_complete(clear);
            finishSummary(attempts, 4);
        }
        require(weakNew == nil && summaryOwnerCount(owners) == 0 &&
                liveBuffers == 1 && liveSummaryPools == 0,
            "matching generation could not reclaim the replacement owner");
        puts("ARGUMENT_SUMMARY_GENERATION_PASS captured=1 host_metadata_update=1 "
             "stale_preserved=1 retry_published=1 later_reclaimed=1 "
             "private_contents=null public_private_cpu_write=0 gpu_execution=0");
    }
}

static void testSummaryFailureRetriesAndReclaims(void) {
    @autoreleasepool {
        NSError *error = nil;
        id device = [NSObject new];
        RDNA4Buffer *buffer =
            newBuffer(device, 64, MTLResourceStorageModePrivate, &error);
        require(buffer && !error, "retry source buffer");
        __weak id weakOld = nil;
        @autoreleasepool {
            id old = [SubmitFaultSentinel new];
            weakOld = old;
            require(rdna4_argument_resource_token(old, device, NO) != 0,
                "retry old token");
            RDNA4ArgumentWriteIntent *write = newIntent(buffer, old);
            rdna4_argument_sampler_submit(write, 1);
            rdna4_argument_sampler_complete(write);
        }
        RDNA4ArgumentSamplerOwners *owners =
            rdna4_argument_sampler_owners(buffer, NO);
        @autoreleasepool {
            require(weakOld && summaryOwnerCount(owners) == 1,
                "retry old persistent owner missing");
        }
        RDNA4ArgumentWriteIntent *pending = newIntent(buffer, nil);
        for (unsigned stage = 1; stage <= 6; ++stage) {
            @autoreleasepool {
                if (stage <= 5) {
                    expectSummaryAllocationFailure(pending, buffer, stage);
                } else {
                    NSArray *attempts = nil;
                    error = nil;
                    failSummaryDescriptorAllocation = YES;
                    require(!rdna4_argument_summary_owners_encode(@[pending],
                            (VkCommandBuffer)(uintptr_t)0x6101,
                            &attempts, &error) && error,
                        "descriptor allocation failure lacked bounded error");
                    require(!failSummaryDescriptorAllocation &&
                            attempts.count == 0 && pending->active,
                        "descriptor failure exposed an attempt or lost writer");
                }
                require(weakOld && summaryOwnerCount(owners) == 1 &&
                        owners.latestSummaryEpoch == 0,
                    "failed summary discarded old owner");
            }
            require(liveBuffers == 1 && liveMemory == 1 &&
                    liveSummaryPools == 0,
                "failed summary leaked a partial allocation graph");
        }
        @autoreleasepool {
            NSArray *attempts = encodeSummaryOracle(pending, 0);
            rdna4_argument_sampler_submit(pending, 2);
            rdna4_argument_sampler_complete(pending);
            RDNA4ArgumentSummaryOwnerAttempt *attempt = attempts[0];
            RDNA4ArgumentSummaryOwnerCapture *capture = attempt->captures[0];
            capture->attempt->_completion.encoded_jobs = 0;
            error = nil;
            require(!rdna4_argument_summary_owners_finish(attempt, YES, 2,
                    &error) && error,
                "incomplete summary was treated as success");
            require(attempt->captures.count == 0 &&
                    owners.latestSummaryEpoch == 0 &&
                    summaryOwnerCount(owners) == 1 && weakOld,
                "incomplete summary discarded old owner or retained capture");
        }
        require(liveBuffers == 1 && liveSummaryPools == 0,
            "incomplete summary leaked scratch or descriptors");
        @autoreleasepool {
            RDNA4ArgumentWriteIntent *retry = newIntent(buffer, nil);
            NSArray *attempts = encodeSummaryOracle(retry, 0);
            rdna4_argument_sampler_submit(retry, 3);
            rdna4_argument_sampler_complete(retry);
            finishSummary(attempts, 3);
            require(owners.latestSummaryEpoch == 3 &&
                    summaryOwnerCount(owners) == 0 &&
                    owners.writers.count == 0 && owners.completed.count == 0,
                "later successful completion did not reclaim failed history");
        }
        require(weakOld == nil && liveBuffers == 1 &&
                liveMemory == 1 && liveSummaryPools == 0,
            "recovered summary retained old sampler or temporary resources");
        printf("ARGUMENT_SUMMARY_RECOVERY_PASS allocation_failures=5 "
               "descriptor_failures=1 incomplete_failures=1 retries=1 "
               "old_owner_reclaimed=1 gpu_execution=0\n");
    }
}

static void testSummarySteadyStatePeaks(unsigned rounds) {
    @autoreleasepool {
        enum { maxRounds = 256 };
        require(rounds >= 2 && rounds <= maxRounds,
            "steady fixture round count exceeds weak history capacity");
        NSError *error = nil;
        id device = [NSObject new];
        RDNA4Buffer *buffer =
            newBuffer(device, 64, MTLResourceStorageModePrivate, &error);
        require(buffer && !error, "steady source buffer");
        uint64_t sourceBytes = liveMemoryBytes;
        uint64_t sourcePrivateBytes = privateMemoryBytes;
        /* Scratch reserves configured chunk/page capacity, even for short input. */
        uint64_t sourceChunk = RDNA4R.lim.maxStorageBufferRange -
            RDNA4_ARGUMENT_SUMMARY_OVERLAP_BYTES;
        if (sourceChunk > RDNA4_ARGUMENT_SUMMARY_DEFAULT_CHUNK_BYTES)
            sourceChunk = RDNA4_ARGUMENT_SUMMARY_DEFAULT_CHUNK_BYTES;
        sourceChunk -= sourceChunk % RDNA4R.lim.minStorageBufferOffsetAlignment;
        uint64_t sourceBinding = (sourceChunk +
            RDNA4_ARGUMENT_SUMMARY_OVERLAP_BYTES + 3u) & ~UINT64_C(3);
        uint64_t dictionaryCapacity = RDNA4R.lim.maxStorageBufferRange /
            sizeof(rdna4_argument_summary_candidate);
        if (dictionaryCapacity > RDNA4_ARGUMENT_SUMMARY_DEFAULT_DICTIONARY_PAGE)
            dictionaryCapacity = RDNA4_ARGUMENT_SUMMARY_DEFAULT_DICTIONARY_PAGE;
        dictionaryCapacity -= dictionaryCapacity %
            RDNA4_ARGUMENT_SUMMARY_DICTIONARY_ALIGN;
        uint64_t dictionaryBinding = dictionaryCapacity *
            sizeof(rdna4_argument_summary_candidate);
        uint64_t outputBinding = dictionaryCapacity /
            RDNA4_ARGUMENT_SUMMARY_DICTIONARY_ALIGN * sizeof(uint32_t);
        uint64_t expectedScratchBytes =
            ((sourceBinding + 4095u) & ~UINT64_C(4095)) +
            ((dictionaryBinding + 4095u) & ~UINT64_C(4095)) +
            ((outputBinding + 4095u) & ~UINT64_C(4095));
        uint64_t expectedTemporaryBytes = expectedScratchBytes +
            ((2u * sizeof(rdna4_argument_summary_candidate) + 4095u) &
                ~UINT64_C(4095)) +
            ((sizeof(uint32_t) + 4095u) & ~UINT64_C(4095));
        peakMemoryBytes = liveMemoryBytes;
        peakPrivateMemoryBytes = privateMemoryBytes;
        peakBuffers = liveBuffers;
        peakSummaryPools = liveSummaryPools;
        __weak id current = nil;
        __weak id history[maxRounds] = { nil };
        NSMutableArray *completedAttempts = [NSMutableArray new];
        NSUInteger peakOwners = 0;
        for (unsigned round = 0; round < rounds; ++round) {
            @autoreleasepool {
                id sampler = [SubmitFaultSentinel new];
                uint64_t token =
                    rdna4_argument_resource_token(sampler, device, NO);
                require(token != 0, "steady sampler token");
                history[round] = sampler;
                NSMutableSet *inflight = [NSMutableSet new];
                if (current) [inflight addObject:current];
                [inflight addObject:sampler];
                RDNA4ArgumentWriteIntent *write = newIntent(buffer, sampler);
                NSArray *attempts = encodeSummaryOracle(write, token);
                RDNA4ArgumentSummaryOwnerAttempt *attempt = attempts[0];
                RDNA4ArgumentSummaryOwnerCapture *capture = attempt->captures[0];
                require(capture->task->_plan.source_binding_bytes == sourceBinding &&
                        capture->task->_plan.dictionary_binding_bytes == dictionaryBinding &&
                        capture->task->_plan.output_page_bytes == outputBinding,
                    "steady plan differs from configured chunk/page capacity");
                [completedAttempts addObjectsFromArray:attempts];
                RDNA4ArgumentSamplerOwners *owners =
                    rdna4_argument_sampler_owners(buffer, NO);
                NSUInteger ownerCount = summaryOwnerCount(owners);
                if (ownerCount > peakOwners) peakOwners = ownerCount;
                require(ownerCount == (round ? 2u : 1u) &&
                        owners.writers.count == 1,
                    "steady ownership grew beyond current and incoming writer");
                require(liveBuffers == 6 && liveSummaryPools == 1,
                    "steady capture has an unexpected allocation graph");
                rdna4_argument_sampler_submit(write, round + 1u);
                rdna4_argument_sampler_complete(write);
                finishSummary(attempts, round + 1u);
                require(summaryOwnerCount(owners) == 1 &&
                        owners.writers.count == 0 && owners.completed.count == 0,
                    "steady publication retained retired owner history");
                require(owners.latestSummaryEpoch == round + 1u,
                    "steady publication did not advance accepted epoch");
                if (round)
                    require(history[round - 1u] != nil && inflight.count == 2,
                        "summary refinement released an in-flight snapshot");
                current = sampler;
            }
            require(current != nil && liveBuffers == 1 &&
                    liveMemoryBytes == sourceBytes && liveSummaryPools == 0,
                "steady completion retained scratch/descriptor history");
            for (unsigned old = 0; old < round; ++old)
                require(history[old] == nil,
                    "retired historical sampler remained alive");
        }
        require(peakOwners == 2 && peakBuffers == 6 &&
                peakSummaryPools == 1 &&
                peakPrivateMemoryBytes - sourcePrivateBytes == expectedScratchBytes &&
                peakMemoryBytes - sourceBytes == expectedTemporaryBytes,
            "steady resource peak exceeded one active summary");
        require(completedAttempts.count == rounds,
            "steady fixture did not retain completed wrappers");
        printf("ARGUMENT_SUMMARY_STEADY_PASS rounds=%u current_owners=1 "
               "inflight_peak=2 retired_history=%u owner_peak=%lu "
               "descriptor_pool_peak=%u descriptor_set_peak=%u "
               "descriptor_capacity_peak=%u scratch_bytes_peak=%llu "
               "temporary_bytes_peak=%llu expected_scratch_bytes=%llu "
               "expected_temporary_bytes=%llu buffer_peak=%u quarantine=0 "
               "retained_finished_wrappers=%lu gpu_execution=0\n",
               rounds, rounds - 1u, (unsigned long)peakOwners,
               peakSummaryPools, peakSummaryPools, peakSummaryPools * 3u,
               (unsigned long long)(peakPrivateMemoryBytes - sourcePrivateBytes),
               (unsigned long long)(peakMemoryBytes - sourceBytes),
               (unsigned long long)expectedScratchBytes,
               (unsigned long long)expectedTemporaryBytes, peakBuffers,
               (unsigned long)completedAttempts.count);
    }
}

static void testQueueSubmitFailureQuarantinesUnknownSegment(void) {
    @autoreleasepool {
        NSError *error = nil;
        id device = [NSObject new];
        id sampler = [NSObject new];
        RDNA4Buffer *currentBuffer =
            newBuffer(device, 64, MTLResourceStorageModePrivate, &error);
        RDNA4Buffer *futureBuffer =
            newBuffer(device, 64, MTLResourceStorageModePrivate, &error);
        RDNA4Buffer *unrelatedBuffer =
            newBuffer(device, 64, MTLResourceStorageModePrivate, &error);
        require(currentBuffer != nil && futureBuffer != nil &&
                unrelatedBuffer != nil && !error,
            "retire-unknown buffers");
        RDNA4ArgumentWriteIntent *currentIntent =
            newIntent(currentBuffer, sampler);
        RDNA4ArgumentWriteIntent *futureIntent =
            newIntent(futureBuffer, sampler);
        SubmitFaultSentinel *unrelatedCandidate = [SubmitFaultSentinel new];
        NSSet *unrelatedCandidates =
            [NSSet setWithObject:unrelatedCandidate];
        RDNA4ArgumentWriteIntent *unrelatedIntent =
            rdna4_argument_sampler_intent(
                unrelatedBuffer, NSMakeRange(0, unrelatedBuffer.length),
                unrelatedCandidates, NO);
        __weak id weakUnrelatedCandidate = unrelatedCandidate;
        unrelatedCandidate = nil;
        require(currentIntent != nil && futureIntent != nil &&
                unrelatedIntent != nil,
            "retire-unknown intents");

        RDNA4CommandSegment *current = newSegment(
            (VkCommandBuffer)(uintptr_t)0x4001, currentIntent, YES);
        RDNA4CommandSegment *future = newSegment(
            (VkCommandBuffer)(uintptr_t)0x4002, futureIntent, NO);
        addSegmentSentinels(current);
        addSegmentSentinels(future);
        __weak id weakCurrentAttempt =
            current->argumentSummaryAttempts[0];
        __weak id weakCurrentRef = current->argumentRefs.anyObject;
        __weak id weakCurrentPool = current->argumentPools[0];
        __weak id weakFutureAttempt =
            future->argumentSummaryAttempts[0];
        __weak id weakFutureRef = future->argumentRefs.anyObject;
        __weak id weakFuturePool = future->argumentPools[0];

        SubmitFaultQueue *queue = [SubmitFaultQueue new];
        require(queue != nil, "retire-unknown queue");
        SubmitFaultCommandBuffer *commandBuffer = newCommandBuffer(
            [NSMutableArray arrayWithObjects:current, future, nil], queue);
        __weak SubmitFaultCommandBuffer *weakCommandBuffer = commandBuffer;

        unsigned freeBefore = commandFreeCalls;
        unsigned submitBefore = queueSubmitCalls;
        unsigned waitBefore = fenceWaitCalls;
        queueSubmitFailureAt = submitBefore + 1;
        BOOL advanced = [commandBuffer
            rdna4AdvanceWithQueue:(RDNA4CommandQueue *)queue];
        require(!advanced, "unknown-retirement submit was accepted");
        require(queueSubmitCalls == submitBefore + 1,
            "unknown-retirement submit call count");
        require(submittedCommands[submitBefore] ==
                (VkCommandBuffer)(uintptr_t)0x4001,
            "unknown-retirement submitted the wrong command");
        require(fenceWaitCalls == waitBefore,
            "queue-submit failure unexpectedly waited a fence");
        require([commandBuffer rdna4TestSubmitAttempted],
            "unknown-retirement submit was not marked attempted");
        require(current->retirementUnknown,
            "failed submit segment was not marked retirement unknown");
        require(!future->retirementUnknown,
            "unattempted future segment was marked retirement unknown");
        require(!futureIntent->active,
            "unattempted future intent survived queue-submit failure");
        require(currentIntent->active,
            "retirement-unknown intent was abandoned");
        require(current->argumentIntents.count == 1 &&
                current->argumentRefs.count == 1 &&
                current->argumentPools.count == 1 &&
                current->argumentSummaryAttempts.count == 1,
            "retirement-unknown segment lost ownership");
        require(future->argumentIntents.count == 0 &&
                future->argumentRefs.count == 0 &&
                future->argumentPools.count == 0 &&
                future->argumentSummaryAttempts == nil,
            "future segment retained ownership after queue-submit failure");
        require(weakCurrentAttempt != nil && weakCurrentRef != nil &&
                weakCurrentPool != nil,
            "retirement-unknown segment released owned objects");
        require(weakFutureAttempt == nil && weakFutureRef == nil &&
                weakFuturePool == nil,
            "future segment leaked canceled objects");
        require(current->vk == (VkCommandBuffer)(uintptr_t)0x4001 &&
                future->vk == (VkCommandBuffer)(uintptr_t)0x4002,
            "queue-submit failure freed an unknown or future command");
        require(commandFreeCalls == freeBefore,
            "queue-submit failure released command buffers to Vulkan");
        require(atomic_load(&rdna4_submit_fault) ==
                VK_ERROR_DEVICE_LOST,
            "queue-submit failure did not poison future submissions");
        require(rdna4_unresolved_commands != nil &&
                [rdna4_unresolved_commands
                    indexOfObjectIdenticalTo:commandBuffer] != NSNotFound,
            "retirement-unknown command buffer was not quarantined");
        [queue waitForCompletion];
        require(queue.completionCalls == 1 &&
                queue.lastCompletionError != nil,
            "unknown-retirement submit did not complete with an error");

        RDNA4ArgumentSamplerOwners *unrelatedOwners =
            rdna4_argument_sampler_owners(unrelatedBuffer, NO);
        require(unrelatedIntent->active &&
                unrelatedOwners.writers.count == 1,
            "unrelated argument owner did not survive submit failure");
        require(weakUnrelatedCandidate != nil,
            "unrelated owner candidate was released by submit failure");
        rdna4_argument_sampler_abandon(unrelatedIntent);
        require(unrelatedOwners.writers.count == 0,
            "unrelated owner cleanup failed");
        unrelatedIntent = nil;
        unrelatedCandidates = nil;
        require(weakUnrelatedCandidate == nil,
            "unrelated owner candidate survived intentional cleanup");

        /*
         * Quarantine intentionally keeps the command buffer alive. The weak
         * check documents that this fixture does not claim cleanup.
         */
        disposeCommandBuffer(&commandBuffer);
        require(weakCommandBuffer != nil,
            "quarantine unexpectedly released the unresolved command buffer");
        currentBuffer = nil;
        futureBuffer = nil;
        unrelatedBuffer = nil;
        device = nil;
        sampler = nil;
    }
}

static void VKAPI_CALL summaryHeapRequirements(VkDevice device,
    const VkBufferMemoryRequirementsInfo2 *info,
    VkMemoryRequirements2 *requirements) {
    submitFaultBufferRequirements(device, info->buffer,
        &requirements->memoryRequirements);
    VkMemoryDedicatedRequirements *dedicated = requirements->pNext;
    require(dedicated &&
            dedicated->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
        "heap summary did not query dedicated requirements");
    dedicated->prefersDedicatedAllocation = VK_FALSE;
    dedicated->requiresDedicatedAllocation = VK_FALSE;
}

static RDNA4Heap *newSummaryHeap(id device) {
    MTLHeapDescriptor *descriptor = [MTLHeapDescriptor new];
    descriptor.type = MTLHeapTypeAutomatic;
    descriptor.size = 16384;
    descriptor.storageMode = MTLStorageModePrivate;
    return [[RDNA4Heap alloc] initWithDevice:device descriptor:descriptor];
}

static void testAcceptedSummaryFenceFailure(void) {
    @autoreleasepool {
        __weak id weakSource = nil;
        __weak id weakSampler = nil;
        __weak id weakCapture = nil;
        __weak id weakTask = nil;
        __weak id weakDictionary = nil;
        __weak id weakOutput = nil;
        __weak id weakSourceScratch = nil;
        __weak id weakDictionaryScratch = nil;
        __weak id weakOutputScratch = nil;
        __weak id weakCommand = nil;
        __weak id weakHeap = nil;
        __weak id weakOtherHeap = nil;
        __weak id weakOtherSampler = nil;
        uint64_t retainedBytes = 0;
        uint64_t retainedAccounting = 0;
        RDNA4P.bmr2 = summaryHeapRequirements;
        @autoreleasepool {
            id device = [SubmitFaultDevice new];
            RDNA4Heap *heap = newSummaryHeap(device);
            RDNA4Heap *otherHeap = newSummaryHeap([SubmitFaultDevice new]);
            require(heap && otherHeap, "summary fault real heaps");
            weakHeap = heap;
            weakOtherHeap = otherHeap;
            RDNA4Buffer *source = [heap newBufferWithLength:16384
                options:MTLResourceStorageModePrivate];
            RDNA4Buffer *other = [otherHeap newBufferWithLength:16384
                options:MTLResourceStorageModePrivate];
            require(source && other && source.contents == NULL,
                "summary fault heap-backed sources");
            weakSource = source;
            id sampler = [SubmitFaultSentinel new];
            weakSampler = sampler;
            uint64_t token = rdna4_argument_resource_token(sampler, device, NO);
            require(token != 0, "summary fault sampler token");
            RDNA4ArgumentWriteIntent *writer = newIntent(source, sampler);
            RDNA4ArgumentWriteIntent *otherWriter =
                newIntent(other, [SubmitFaultSentinel new]);
            weakOtherSampler = otherWriter->candidates.anyObject;
            RDNA4ArgumentSamplerOwners *otherOwners =
                rdna4_argument_sampler_owners(other, NO);
            NSArray *attempts = encodeSummaryOracle(writer, token);
            RDNA4ArgumentSummaryOwnerAttempt *attempt = attempts[0];
            RDNA4ArgumentSummaryOwnerCapture *capture = attempt->captures[0];
            weakCapture = capture;
            weakTask = capture->task;
            weakDictionary = capture->task.dictionaryBuffer;
            weakOutput = capture->task.outputBuffer;
            weakSourceScratch = capture->attempt->_sourceScratch;
            weakDictionaryScratch = capture->attempt->_dictionaryScratch;
            weakOutputScratch = capture->attempt->_outputScratch;
            RDNA4CommandSegment *segment = newSegment(
                (VkCommandBuffer)(uintptr_t)0x7101, writer, YES);
            segment->argumentSummaryAttempts = attempts;
            [segment->argumentRefs addObject:source];
            SubmitFaultQueue *queue = [SubmitFaultQueue new];
            SubmitFaultCommandBuffer *command = newCommandBuffer(
                [NSMutableArray arrayWithObject:segment], queue);
            weakCommand = command;
            uint64_t heapSerial = [command rdna4TestOpenHeapTimeline];
            require(heapSerial != 0, "summary fault heap timeline token");
            unsigned submits = queueSubmitCalls;
            unsigned frees = commandFreeHandles;
            unsigned destroyedFences = fenceDestroyCalls;
            uint64_t allocationBefore = atomic_load(&rdna4_alloc_total);
            uint64_t bytesBefore = liveMemoryBytes;
            fenceWaitResult = VK_ERROR_DEVICE_LOST;
            (void)[command rdna4AdvanceWithQueue:(RDNA4CommandQueue *)queue];
            [queue waitForCompletion];
            require(queueSubmitCalls == submits + 1 && segment->submitted &&
                    segment->argumentSubmissionEpoch != 0 &&
                    segment->argumentSubmissionEpoch == writer->submissionEpoch,
                "summary fault did not accept exactly one segment");
            require(fenceWaitCalls == 1 && queue.completionCalls == 1 &&
                    queue.lastCompletionError != nil,
                "summary fence failure did not propagate a bounded error");
            require(!segment->done && writer->active && !attempt->finished &&
                    attempt->captures.count == 1 &&
                    segment->argumentSummaryAttempts.count == 1 &&
                    rdna4_argument_sampler_owners(source, NO).latestSummaryEpoch == 0,
                "failed summary fence retired work or published owners");
            require(commandFreeHandles == frees &&
                    fenceDestroyCalls == destroyedFences && liveSummaryPools == 1,
                "failed summary fence released unknown GPU resources");
            require(atomic_load(&rdna4_submit_fault) == VK_ERROR_DEVICE_LOST &&
                    [rdna4_unresolved_commands indexOfObjectIdenticalTo:command] != NSNotFound &&
                    liveMemoryBytes == bytesBefore &&
                    atomic_load(&rdna4_alloc_total) == allocationBefore,
                "failed summary fence escaped quarantine accounting");
            require([heap usedSize] == 16384 && [heap rdna4BackingInUse] == 16384 &&
                    [heap rdna4BackingBytes] == 16384 &&
                    RDNA4HT.p.completed < heapSerial,
                "failed summary fence retired the heap backing token");
            [source makeAliasable];
            [heap rdna4Reap];
            require([heap usedSize] == 0 && [heap rdna4BackingInUse] == 16384 &&
                    [heap rdna4BackingBytes] == 16384,
                "unknown summary allowed physical alias reuse");
            @autoreleasepool {
                require([heap newBufferWithLength:16384
                        options:MTLResourceStorageModePrivate] == nil,
                    "unknown summary heap range was reallocated");
            }
            require(liveMemoryBytes == bytesBefore &&
                    atomic_load(&rdna4_alloc_total) == allocationBefore,
                "blocked summary alias allocation changed accounting");
            require(otherWriter->active && otherOwners.writers.count == 1 &&
                    summaryOwnerCount(otherOwners) == 1 && weakOtherSampler &&
                    [otherHeap usedSize] == 16384 &&
                    [otherHeap rdna4BackingInUse] == 16384,
                "summary quarantine corrupted unrelated device/heap ownership");
            rdna4_argument_sampler_abandon(otherWriter);
            require(otherOwners.writers.count == 0,
                "unrelated owner could not clean up after summary quarantine");
            retainedBytes = bytesBefore - 16384u;
            retainedAccounting = allocationBefore - 16384u;
        }
        require(weakOtherHeap == nil && weakOtherSampler == nil,
            "summary quarantine retained unrelated device/heap objects");
        require(weakSource && weakSampler && weakCapture && weakTask &&
                weakDictionary && weakOutput && weakSourceScratch &&
                weakDictionaryScratch && weakOutputScratch && weakCommand && weakHeap,
            "quarantine lost the real summary capture graph after caller release");
        require(liveBuffers == 6 && liveMemory == 6 && liveSummaryPools == 1 &&
                liveMemoryBytes == retainedBytes &&
                atomic_load(&rdna4_alloc_total) == retainedAccounting &&
                atomic_load(&rdna4_heap_reserved) == 16384,
            "quarantine graph or unrelated heap cleanup lost allocation accounting");
        printf("ARGUMENT_SUMMARY_FENCE_QUARANTINE_PASS checks=%u accepted=1 "
               "fence_device_lost=1 real_capture_retained=1 buffers=%u memory=%u "
               "descriptor_pools=%u quarantined_bytes=%llu driver_accounted_bytes=%llu "
               "heap_physical_retained=16384 alias_rejected=1 unrelated_heap_clean=1 "
               "gpu_execution=0\n", checks, liveBuffers, liveMemory, liveSummaryPools,
               (unsigned long long)liveMemoryBytes,
               (unsigned long long)atomic_load(&rdna4_alloc_total));
    }
}

static void runSummaryFenceFaultProcess(const char *executable) {
    pid_t child = -1;
    char *arguments[] = { (char *)executable, "--summary-fence-fault", NULL };
    require(posix_spawn(&child, executable, NULL, NULL, arguments,
            *_NSGetEnviron()) == 0,
        "could not spawn isolated summary fence fault fixture");
    int status = 0;
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    require(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "isolated summary fence fault fixture failed");
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        if (argc == 2 && !strcmp(argv[1], "--help")) {
            puts("Usage: test-argument-submit-faults\n"
                 "Drives the production argument scheduler with mocked Vulkan; "
                 "no VM or GPU execution.");
            return 0;
        }
        if (argc == 2 && !strcmp(argv[1], "--summary-fence-fault")) {
            setupMocks();
            testAcceptedSummaryFenceFailure();
            return 0;
        }
        if (argc != 1) return 2;
        setupMocks();
        testFenceFailureRollsBackEverySegment();
        require(liveBuffers == 0, "Vulkan buffers leaked after fence failure");
        require(liveMemory == 0, "Vulkan memory leaked after fence failure");
        testAcceptedSegmentIsNotReplayed();
        require(liveBuffers == 0, "Vulkan buffers leaked after accepted segment");
        require(liveMemory == 0, "Vulkan memory leaked after accepted segment");
        testSummaryAllocationFailureKeepsOldOwner();
        require(liveBuffers == 0, "Vulkan buffers leaked after summary failure");
        require(liveMemory == 0, "Vulkan memory leaked after summary failure");
        testSummaryFailureRetriesAndReclaims();
        require(liveBuffers == 0 && liveMemory == 0 && liveSummaryPools == 0,
            "Vulkan resources leaked after summary recovery");
        testSummaryHostGenerationInterleave();
        require(liveBuffers == 0 && liveMemory == 0 && liveSummaryPools == 0,
            "Vulkan resources leaked after summary generation interleave");
        testSummarySteadyStatePeaks(64);
        require(liveBuffers == 0 && liveMemory == 0 && liveSummaryPools == 0,
            "Vulkan resources leaked after steady summary writers");
        runSummaryFenceFaultProcess(argv[0]);
        testSummarySteadyStatePeaks(256);
        require(liveBuffers == 0 && liveMemory == 0 && liveSummaryPools == 0,
            "Vulkan resources leaked after steady summary writers");
        /*
         * Keep this last: the production submit fault disables every later
         * submission for the lifetime of the process.
         */
        testQueueSubmitFailureQuarantinesUnknownSegment();
        require(liveBuffers == 0,
            "Vulkan buffers leaked after unknown-retirement failure");
        require(liveMemory == 0,
            "Vulkan memory leaked after unknown-retirement failure");
        printf("ARGUMENT_SUBMIT_FAULTS_PASS checks=%u mock_vulkan=1 "
               "mock_metal_queue=1 gpu_execution=0 quarantined=1\n",
               checks);
    }
    return 0;
}
