// ---------------------------------------------------------------------------------------------------------------
// GPU Timestamp Counter Sampling (MTLCounter / MTLCounterSet / MTLCounterSampleBuffer) over VK_QUERY_TYPE_TIMESTAMP.
//
// Wave 2B GarnetDevice.m wiring notes (do NOT edit GarnetDevice.m in Wave 2A):
//   1. Include after #include "rdna4_command_buffer_impl.inc" (order vs encoders is flexible):
//        #include "rdna4_counters_impl.inc"
//   2. Register protocols in +[GarnetDevice load]:
//        { const char *n[] = { "MTLCounter" }; rdna4_add_protocols([RDNA4Counter class], n, 1, "RDNA4Counter"); }
//        { const char *n[] = { "MTLCounterSet" }; rdna4_add_protocols([RDNA4CounterSet class], n, 1, "RDNA4CounterSet"); }
//        { const char *n[] = { "MTLCounterSampleBuffer" }; rdna4_add_protocols([RDNA4CounterSampleBuffer class], n, 1, "RDNA4CounterSampleBuffer"); }
//   3. Wire pass descriptors in @implementation RDNA4CommandBuffer (Encoders):
//        - (id)blitCommandEncoderWithDescriptor:(MTLBlitPassDescriptor *)d {
//            RDNA4LOGR("cb blitCommandEncoderWithDescriptor:");
//            return [[RDNA4BlitEncoder alloc] initWithCommandBuffer:self descriptor:d];
//        }
//        - (id)computeCommandEncoderWithDescriptor:(MTLComputePassDescriptor *)d {
//            (void)[self vk];
//            RDNA4LOGR("cb computeCommandEncoderWithDescriptor:");
//            return [[RDNA4ComputeEncoder alloc] initWithCommandBuffer:self descriptor:d];
//        }
//   4. Wire MTLDevice counter selectors in @implementation GarnetDevice:
//        - (NSArray<id<MTLCounterSet>> *)counterSets { return rdna4_device_counter_sets(); }
//        - (BOOL)supportsCounterSampling:(MTLCounterSamplingPoint)samplingPoint {
//            return rdna4_device_supports_counter_sampling(samplingPoint);
//        }
//        - (id<MTLCounterSampleBuffer>)newCounterSampleBufferWithDescriptor:(MTLCounterSampleBufferDescriptor *)descriptor
//                                                                     error:(NSError **)error {
//            return rdna4_device_new_counter_sample_buffer(self, descriptor, error);
//        }
//        - (void)sampleTimestamps:(MTLTimestamp *)cpuTimestamp gpuTimestamp:(MTLTimestamp *)gpuTimestamp {
//            rdna4_device_sample_timestamps(cpuTimestamp, gpuTimestamp);
//        }
//      Existing `- (BOOL)supportsVertexAmplificationCount:(NSUInteger)count { return count == 1; }`
//      already matches RDNA4AMP_MAX_SUPPORTED_COUNT (1) and test-gate.c:188.
//   5. Note for Wave 2B / Task 8.1 test-occ.c source-pin updates:
//      `rdna4_render_encoder_impl.inc` adds 3 pass-split `[self rdna4EndPass]` call sites
//      (counter sampling + layer-shifted amplification) and `rdna4_counters_impl.inc` uses
//      `VK_QUERY_RESULT_WITH_AVAILABILITY_BIT` (without `VK_QUERY_RESULT_WAIT_BIT`).
// ---------------------------------------------------------------------------------------------------------------
#ifndef RDNA4_COUNTERS_IMPL_INC
#define RDNA4_COUNTERS_IMPL_INC
#include "rdna4_counters.h"

_Static_assert(sizeof(MTLCounterResultTimestamp) == sizeof(uint64_t), "MTLCounterResultTimestamp must be 8 bytes");
_Static_assert((int)MTLCounterSamplingPointAtStageBoundary == RDNA4CNT_SP_STAGE, "stage sampling point enum");
_Static_assert((int)MTLCounterSamplingPointAtDrawBoundary == RDNA4CNT_SP_DRAW, "draw sampling point enum");
_Static_assert((int)MTLCounterSamplingPointAtDispatchBoundary == RDNA4CNT_SP_DISPATCH, "dispatch sampling point enum");
_Static_assert((int)MTLCounterSamplingPointAtTileDispatchBoundary == RDNA4CNT_SP_TILE_DISPATCH, "tile sampling point enum");
_Static_assert((int)MTLCounterSamplingPointAtBlitBoundary == RDNA4CNT_SP_BLIT, "blit sampling point enum");
_Static_assert((int)MTLStorageModeShared == RDNA4CNT_STORAGE_SHARED, "Shared storage enum");
_Static_assert((int)MTLStorageModePrivate == RDNA4CNT_STORAGE_PRIVATE, "Private storage enum");
_Static_assert(sizeof(MTLVertexAmplificationViewMapping) == sizeof(rdna4amp_mapping), "view mapping struct size");

#ifndef RDNA4_COUNTER_INTERNAL_PROTO
#define RDNA4_COUNTER_INTERNAL_PROTO
@protocol RDNA4CounterSampleBufferInternal <NSObject>
- (id)device;
- (NSUInteger)sampleCount;
- (MTLStorageMode)rdna4StorageMode;
- (BOOL)rdna4SampleOnCommandBuffer:(RDNA4CommandBuffer *)cb
                   vkCommandBuffer:(VkCommandBuffer)cmd
                       sampleIndex:(NSUInteger)sampleIndex
                           barrier:(BOOL)barrier
                        stageFlags:(VkPipelineStageFlagBits)stage
                               who:(const char *)who;
- (BOOL)rdna4ResolveRange:(NSRange)range
                intoBytes:(uint64_t *)dst
                    error:(NSError **)err;
@end
#endif

static NSError *rdna4_counter_err(MTLCounterSampleBufferError code, NSString *msg) {
    return [NSError errorWithDomain:MTLCounterErrorDomain
                               code:code
                           userInfo:@{ NSLocalizedDescriptionKey: msg ?: @"counter sample buffer error" }];
}

// Always resolve Vulkan 1.0 core vkCmdWriteTimestamp (synchronization2 is not enabled on RDNA4R.dev).
static PFN_vkCmdWriteTimestamp rdna4_vk_cmd_write_timestamp(void) {
    if (!RDNA4R.ok || !RDNA4R.dev) return NULL;
    static PFN_vkCmdWriteTimestamp fn;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        if (vkGetDeviceProcAddr)
            fn = (PFN_vkCmdWriteTimestamp)vkGetDeviceProcAddr(RDNA4R.dev, "vkCmdWriteTimestamp");
        if (!fn && rdna4_gipa && RDNA4R.inst)
            fn = (PFN_vkCmdWriteTimestamp)rdna4_gipa(RDNA4R.inst, "vkCmdWriteTimestamp");
    });
    return fn;
}

static void rdna4_counter_barrier(VkCommandBuffer cmd) {
    VkMemoryBarrier mb = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 1, &mb, 0, NULL, 0, NULL);
}

@interface RDNA4Counter : NSObject {
    NSString *_name;
}
- (instancetype)initWithName:(NSString *)name;
- (NSString *)name;
@end

@implementation RDNA4Counter
- (instancetype)initWithName:(NSString *)name {
    self = [super init];
    if (!self) return nil;
    _name = [name copy];
    return self;
}
RDNA4_DNR(RDNA4Counter)
- (NSString *)name { return _name; }
@end

@interface RDNA4CounterSet : NSObject {
    NSString *_name;
    NSArray<id<MTLCounter>> *_counters;
}
- (instancetype)initTimestampSet;
- (NSString *)name;
- (NSArray<id<MTLCounter>> *)counters;
@end

@implementation RDNA4CounterSet
- (instancetype)initTimestampSet {
    self = [super init];
    if (!self) return nil;
    _name = [MTLCommonCounterSetTimestamp copy];
    RDNA4Counter *ts = [[RDNA4Counter alloc] initWithName:MTLCommonCounterTimestamp];
    _counters = ts ? @[ (id<MTLCounter>)ts ] : @[];
    return self;
}
RDNA4_DNR(RDNA4CounterSet)
- (NSString *)name { return _name; }
- (NSArray<id<MTLCounter>> *)counters { return _counters; }
@end

@interface RDNA4CounterSampleBuffer : NSObject <RDNA4CounterSampleBufferInternal> {
    id _dev;
    NSString *_lbl;
    VkQueryPool _qp;
    rdna4cnt_pool_state _state;
    pthread_mutex_t _mu;
    BOOL _muInit;
}
- (instancetype)initWithDevice:(id)dev
                    descriptor:(MTLCounterSampleBufferDescriptor *)d
                         error:(NSError **)err;
- (id)device;
- (NSString *)label;
- (NSUInteger)sampleCount;
- (NSData *)resolveCounterRange:(NSRange)range;
@end

@implementation RDNA4CounterSampleBuffer
- (instancetype)initWithDevice:(id)dev
                    descriptor:(MTLCounterSampleBufferDescriptor *)d
                         error:(NSError **)err {
    self = [super init];
    if (!self) return nil;
    if (!d) {
        if (err) *err = rdna4_counter_err(MTLCounterSampleBufferErrorInvalid, @"counter sample buffer descriptor is nil");
        return nil;
    }
    if (!d.counterSet || ![d.counterSet isKindOfClass:[RDNA4CounterSet class]]) {
        if (err) *err = rdna4_counter_err(MTLCounterSampleBufferErrorInvalid, @"counterSet must be a supported timestamp counter set from this device");
        return nil;
    }
    const char *why = NULL;
    int vrc = rdna4cnt_validate_desc((uint64_t)d.sampleCount, (uint64_t)d.storageMode, &why);
    if (vrc != RDNA4CNT_DESC_OK) {
        MTLCounterSampleBufferError code = (vrc == RDNA4CNT_DESC_OOM)
            ? MTLCounterSampleBufferErrorOutOfMemory
            : MTLCounterSampleBufferErrorInvalid;
        if (err) *err = rdna4_counter_err(code, [NSString stringWithUTF8String:why ?: "invalid counter sample buffer descriptor"]);
        return nil;
    }
    NSError *openErr = nil;
    if (!rdna4_radv_open(&openErr)) {
        if (err) *err = rdna4_counter_err(MTLCounterSampleBufferErrorInternal,
                                          openErr.localizedDescription ?: @"RADV open failed for counter sample buffer");
        return nil;
    }
    const rdna4dc_metal3_caps m3 = rdna4_metal3_caps();
    if (!rdna4dc_metal3_timestamps_supported(&m3) || !vkCreateQueryPool || !vkDestroyQueryPool ||
        !vkCmdResetQueryPool || !vkGetQueryPoolResults || !rdna4_vk_cmd_write_timestamp()) {
        if (err) *err = rdna4_counter_err(MTLCounterSampleBufferErrorInternal, @"GPU timestamp counter sampling is unsupported by the backend");
        return nil;
    }
    if (!rdna4cnt_pool_init(&_state, (uint32_t)d.sampleCount, (uint32_t)d.storageMode,
                            m3.timestamp_valid_bits, (double)m3.timestamp_period_ns)) {
        if (err) *err = rdna4_counter_err(MTLCounterSampleBufferErrorOutOfMemory, @"cannot allocate counter sample tracking state");
        return nil;
    }
    VkQueryPoolCreateInfo qi = {
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType = VK_QUERY_TYPE_TIMESTAMP,
        .queryCount = _state.sample_count
    };
    VkResult vr = vkCreateQueryPool(RDNA4R.dev, &qi, NULL, &_qp);
    if (vr != VK_SUCCESS || _qp == VK_NULL_HANDLE) {
        MTLCounterSampleBufferError code = (vr == VK_ERROR_OUT_OF_HOST_MEMORY || vr == VK_ERROR_OUT_OF_DEVICE_MEMORY)
            ? MTLCounterSampleBufferErrorOutOfMemory
            : MTLCounterSampleBufferErrorInternal;
        if (err) *err = rdna4_counter_err(code, [NSString stringWithFormat:@"vkCreateQueryPool(TIMESTAMP, %u) = %d", _state.sample_count, vr]);
        rdna4cnt_pool_destroy(&_state);
        return nil;
    }
    // Reset every slot once on creation so unsampled or aborted slots read as unavailable (~0ULL).
    VkQueryPool poolToReset = _qp;
    uint32_t resetCount = _state.sample_count;
    NSError *resetErr = nil;
    BOOL unresolved = NO;
    BOOL resetOk = rdna4_oneshot(&resetErr, ^(VkCommandBuffer cmd) {
        vkCmdResetQueryPool(cmd, poolToReset, 0u, resetCount);
    }, &unresolved);
    if (!resetOk) {
        if (!unresolved) vkDestroyQueryPool(RDNA4R.dev, _qp, NULL);
        _qp = VK_NULL_HANDLE;
        rdna4cnt_pool_destroy(&_state);
        if (err) *err = rdna4_counter_err(MTLCounterSampleBufferErrorInternal,
                                          resetErr.localizedDescription ?: @"initial timestamp query pool reset failed");
        return nil;
    }
    pthread_mutex_init(&_mu, NULL);
    _muInit = YES;
    _dev = dev;
    _lbl = [d.label copy];
    RDNA4LOGR("RDNA4CounterSampleBuffer %p: %u timestamp slot(s), storage %u, %.3f ns/tick (%u valid bits)",
              (__bridge void *)self, _state.sample_count, _state.storage_mode, _state.period_ns, _state.valid_bits);
    return self;
}

- (void)dealloc {
    if (RDNA4R.ok && _qp != VK_NULL_HANDLE && vkDestroyQueryPool)
        vkDestroyQueryPool(RDNA4R.dev, _qp, NULL);
    _qp = VK_NULL_HANDLE;
    rdna4cnt_pool_destroy(&_state);
    if (_muInit) pthread_mutex_destroy(&_mu);
}

RDNA4_DNR(RDNA4CounterSampleBuffer)
- (id)device { return _dev; }
- (NSString *)label { return _lbl; }
- (NSUInteger)sampleCount { return (NSUInteger)_state.sample_count; }
- (MTLStorageMode)rdna4StorageMode { return (MTLStorageMode)_state.storage_mode; }

- (BOOL)rdna4SampleOnCommandBuffer:(RDNA4CommandBuffer *)cb
                   vkCommandBuffer:(VkCommandBuffer)cmd
                       sampleIndex:(NSUInteger)sampleIndex
                           barrier:(BOOL)barrier
                        stageFlags:(VkPipelineStageFlagBits)stage
                               who:(const char *)who {
    if (!cb || cmd == VK_NULL_HANDLE || _qp == VK_NULL_HANDLE) return NO;
    if (!rdna4cnt_index_ok((uint64_t)_state.sample_count, (uint64_t)sampleIndex)) {
        [cb rdna4Fail:[NSString stringWithFormat:@"%s sampleCountersInBuffer: sampleIndex %lu out of range (sampleCount %u)",
                       who ?: "encoder", (unsigned long)sampleIndex, _state.sample_count]];
        return NO;
    }
    PFN_vkCmdWriteTimestamp writeTs = rdna4_vk_cmd_write_timestamp();
    if (!writeTs || !vkCmdResetQueryPool) {
        [cb rdna4Fail:[NSString stringWithFormat:@"%s sampleCountersInBuffer: vkCmdWriteTimestamp unavailable", who ?: "encoder"]];
        return NO;
    }
    [cb rdna4RetainEncoded:self];
    if (barrier) rdna4_counter_barrier(cmd);
    // Caller must be outside any active Vulkan render pass before resetting/sampling.
    vkCmdResetQueryPool(cmd, _qp, (uint32_t)sampleIndex, 1u);
    writeTs(cmd, stage, _qp, (uint32_t)sampleIndex);
    pthread_mutex_lock(&_mu);
    rdna4cnt_slot_mark_sampled(&_state, (uint32_t)sampleIndex);
    pthread_mutex_unlock(&_mu);
    return YES;
}

- (BOOL)rdna4ResolveRange:(NSRange)range
                intoBytes:(uint64_t *)dst
                    error:(NSError **)err {
    if (!dst || !rdna4cnt_range_ok((uint64_t)_state.sample_count, (uint64_t)range.location, (uint64_t)range.length)) {
        if (err) *err = rdna4_counter_err(MTLCounterSampleBufferErrorInvalid, @"resolveCounterRange: range out of bounds");
        return NO;
    }
    for (NSUInteger i = 0; i < range.length; i++) dst[i] = RDNA4CNT_ERROR_VALUE;
    if (!RDNA4R.ok || _qp == VK_NULL_HANDLE || !vkGetQueryPoolResults || atomic_load(&rdna4_submit_fault) != VK_SUCCESS)
        return YES;

    uint8_t *sampledSnap = (uint8_t *)calloc(range.length, 1u);
    if (!sampledSnap) {
        if (err) *err = rdna4_counter_err(MTLCounterSampleBufferErrorOutOfMemory, @"out of memory resolving counter range");
        return NO;
    }
    pthread_mutex_lock(&_mu);
    for (NSUInteger i = 0; i < range.length; i++)
        sampledSnap[i] = (uint8_t)rdna4cnt_slot_is_sampled(&_state, (uint32_t)(range.location + i));
    pthread_mutex_unlock(&_mu);

    NSUInteger pos = 0;
    while (pos < range.length) {
        if (!sampledSnap[pos]) { pos++; continue; }
        NSUInteger runStart = pos;
        while (pos < range.length && sampledSnap[pos] && (pos - runStart) < 256u) pos++;
        uint32_t chunk = (uint32_t)(pos - runStart);
        uint64_t raw[2u * 256u];
        memset(raw, 0, sizeof(uint64_t) * 2u * chunk);
        VkResult vr = vkGetQueryPoolResults(
            RDNA4R.dev, _qp, (uint32_t)(range.location + runStart), chunk,
            sizeof(uint64_t) * 2u * chunk, raw, 2u * sizeof(uint64_t),
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        if (vr == VK_SUCCESS || vr == VK_NOT_READY) {
            rdna4cnt_resolve_pairs(raw, chunk, _state.valid_bits, _state.period_ns, dst + runStart);
        }
    }
    free(sampledSnap);
    return YES;
}

- (NSData *)resolveCounterRange:(NSRange)range {
    if (!rdna4cnt_resolve_cpu_allowed(_state.storage_mode)) {
        RDNA4LOG("resolveCounterRange: refused on non-Shared counter sample buffer (storageMode %u)", _state.storage_mode);
        return nil;
    }
    if (!rdna4cnt_range_ok((uint64_t)_state.sample_count, (uint64_t)range.location, (uint64_t)range.length)) {
        RDNA4LOG("resolveCounterRange: invalid range {%lu, %lu} for sampleCount %u",
                 (unsigned long)range.location, (unsigned long)range.length, _state.sample_count);
        return nil;
    }
    NSMutableData *out = [NSMutableData dataWithLength:range.length * sizeof(MTLCounterResultTimestamp)];
    if (!out) return nil;
    if (![self rdna4ResolveRange:range intoBytes:(uint64_t *)out.mutableBytes error:NULL]) return nil;
    return out;
}
@end

static NSArray<id<MTLCounterSet>> *rdna4_device_counter_sets(void) {
    const rdna4dc_metal3_caps m3 = rdna4_metal3_caps();
    if (!rdna4dc_metal3_timestamps_supported(&m3)) return @[];
    static NSArray<id<MTLCounterSet>> *sets;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        RDNA4CounterSet *ts = [[RDNA4CounterSet alloc] initTimestampSet];
        sets = ts ? @[ (id<MTLCounterSet>)ts ] : @[];
    });
    return sets;
}

static BOOL rdna4_device_supports_counter_sampling(MTLCounterSamplingPoint point) {
    const rdna4dc_metal3_caps m3 = rdna4_metal3_caps();
    return rdna4cnt_supports_sampling_point(&m3, (uint64_t)point) ? YES : NO;
}

static id<MTLCounterSampleBuffer> rdna4_device_new_counter_sample_buffer(
    id dev, MTLCounterSampleBufferDescriptor *descriptor, NSError **error) {
    return (id<MTLCounterSampleBuffer>)[[RDNA4CounterSampleBuffer alloc]
        initWithDevice:dev descriptor:descriptor error:error];
}

// Device-level calibrated CPU/GPU timestamp pair: never opens RADV unexpectedly and never fakes GPU time.
static void rdna4_device_sample_timestamps(MTLTimestamp *cpuTimestamp, MTLTimestamp *gpuTimestamp) {
    if (gpuTimestamp) *gpuTimestamp = 0;
    const rdna4dc_metal3_caps m3 = rdna4_metal3_caps();
    PFN_vkCmdWriteTimestamp writeTs = rdna4_vk_cmd_write_timestamp();
    if (!gpuTimestamp || !RDNA4R.ok || !rdna4dc_metal3_timestamps_supported(&m3) ||
        !writeTs || !vkCreateQueryPool || !vkCmdResetQueryPool || !vkGetQueryPoolResults ||
        atomic_load(&rdna4_submit_fault) != VK_SUCCESS) {
        if (cpuTimestamp) *cpuTimestamp = (MTLTimestamp)rdna4_now();
        return;
    }
    static pthread_mutex_t tsMu = PTHREAD_MUTEX_INITIALIZER;
    static VkQueryPool tsPool = VK_NULL_HANDLE;
    pthread_mutex_lock(&tsMu);
    if (tsPool == VK_NULL_HANDLE) {
        VkQueryPoolCreateInfo qi = {
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType = VK_QUERY_TYPE_TIMESTAMP,
            .queryCount = 1u
        };
        if (vkCreateQueryPool(RDNA4R.dev, &qi, NULL, &tsPool) != VK_SUCCESS) tsPool = VK_NULL_HANDLE;
    }
    VkQueryPool qp = tsPool;
    uint64_t t0 = rdna4_now();
    uint64_t gpuNs = 0;
    if (qp != VK_NULL_HANDLE) {
        BOOL unresolved = NO;
        BOOL ok = rdna4_oneshot(NULL, ^(VkCommandBuffer cmd) {
            vkCmdResetQueryPool(cmd, qp, 0u, 1u);
            writeTs(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qp, 0u);
        }, &unresolved);
        if (ok) {
            uint64_t raw[2] = { 0u, 0u };
            VkResult vr = vkGetQueryPoolResults(
                RDNA4R.dev, qp, 0u, 1u, sizeof raw, raw, sizeof raw,
                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
            if ((vr == VK_SUCCESS || vr == VK_NOT_READY) && raw[1] != 0u) {
                uint64_t ns = rdna4cnt_ticks_to_ns(raw[0], m3.timestamp_valid_bits, (double)m3.timestamp_period_ns);
                if (ns != RDNA4CNT_ERROR_VALUE) gpuNs = ns;
            }
        }
    }
    uint64_t t1 = rdna4_now();
    pthread_mutex_unlock(&tsMu);
    if (cpuTimestamp) *cpuTimestamp = (MTLTimestamp)(t0 + (t1 >= t0 ? (t1 - t0) / 2u : 0u));
    *gpuTimestamp = (MTLTimestamp)gpuNs;
}
#endif
