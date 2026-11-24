// ---------------------------------------------------------------------------------------------------------------
// RDNA4CommandQueue / RDNA4CommandBuffer (10b). A shared scheduler advances only each logical queue's
// head. Event positions split the Vulkan recording into ordered segments; each segment has its own fence.
// Every segment boundary flushes IOSurface writes; a signal also makes them host-visible before its fence. A concurrent waiter queue
// waits each fence in 2 s slices without blocking the serial completion queue, which then signals that segment's events on success and
// calls commandBufferDidComplete:startTime:completionTime:error: only after the terminal segment.
// 11e: the command buffer owns the bytes upload ring and non-resource command
// state. Each Vulkan command segment owns its descriptor pools and the strong
// resource snapshots encoded into that segment; both are released at that
// segment's fence rather than at terminal command-buffer completion. A sticky
// NSError is delivered as the completion error instead of submitting.
// ---------------------------------------------------------------------------------------------------------------
@interface RDNA4CommandSegment : NSObject {
@public
    VkCommandBuffer vk, linearInit;
    VkFence fence;
    NSMutableArray *waits;
    NSMutableArray *signals;
    NSMutableArray *fenceWaits;
    NSMutableArray *fenceUpdates;
    NSMutableArray *touches;   // IOSurface roots first touched in this segment
    NSMutableArray *argumentIntents;
    NSMutableSet *argumentRefs;
    NSMutableSet *commandRefs;
    NSMutableArray *argumentPools;
    NSMutableArray *descriptorPools;
    NSArray *argumentSummaryAttempts;
    BOOL ended, submitted, retirementUnknown, done, hasWork, pruned, terminal;
    uint64_t argumentSubmissionEpoch;
    NSUInteger hostWritebackCopies;
    NSUInteger hostLinearWrites;
    NSMutableIndexSet *displaySourceWrites, *displaySourceReads;
    NSMutableDictionary<NSNumber *, RDNA4Texture *> *displaySourceOverwrites;
    NSHashTable<RDNA4Texture *> *displayImageWrites, *displayImageReads, *displayImageOverwrites;
    BOOL hostOnly, hostActionAccepted, hostActionRunning;
    BOOL (^hostAction)(NSError **);
}
@end
@implementation RDNA4CommandSegment
- (instancetype)init {
    self = [super init];
    if (!self) return nil;
    waits = [NSMutableArray array];
    signals = [NSMutableArray array];
    fenceWaits = [NSMutableArray array];
    fenceUpdates = [NSMutableArray array];
    touches = [NSMutableArray array];
    argumentIntents = [NSMutableArray array];
    argumentRefs = [NSMutableSet set];
    commandRefs = [NSMutableSet set];
    argumentPools = [NSMutableArray array];
    descriptorPools = [NSMutableArray array];
    return self;
}
@end

@protocol RDNA4ResidencySnapshotInternal <NSObject>
- (NSArray *)rdna4Allocations;
- (void)rdna4AddBuffersToSet:(NSMutableSet *)buffers device:(id)device;
@end

@protocol RDNA4ResidencySetInternal <NSObject>
- (id)device;
- (id<RDNA4ResidencySnapshotInternal>)rdna4CommittedSnapshot;
@end

@class RDNA4CommandBuffer;
@interface RDNA4CommandQueue : _MTLCommandQueue {
    VkCommandPool _pool; dispatch_queue_t _cq; id _ndev;
    NSUInteger _gprio, _bprio; id _compQ;   // gap census: SkyLight's priority / completion-queue setters (recorded only)
    NSMutableArray<RDNA4CommandBuffer *> *_pending;
    NSMutableArray *_residencySets;
    pthread_mutex_t _qmu;
}
- (instancetype)initWithDevice:(id)dev descriptor:(id)desc error:(NSError **)err;
- (VkCommandPool)pool;
- (dispatch_queue_t)rdna4CompletionQueue;
- (void)rdna4Kick;
- (void)rdna4DidComplete:(RDNA4CommandBuffer *)cb;
- (BOOL)rdna4AdvanceHead;
- (void)addResidencySet:(id)residencySet;
- (void)addResidencySets:(const id __unsafe_unretained *)residencySets count:(NSUInteger)count;
- (void)removeResidencySet:(id)residencySet;
- (void)removeResidencySets:(const id __unsafe_unretained *)residencySets count:(NSUInteger)count;
- (NSArray *)rdna4ResidencySetSnapshots;
@end

// CRC diagnostic, completion time (fence done, the copy ran): A1, B, A2. Returns the record the present step uses for C (nil when the check cannot run).
static rdna4crc_rec_t *rdna4s_crc_check(RDNA4Texture *dt, int slot, uint64_t seq) {
    const uint8_t *src = [dt rdna4IBase]; IOSurfaceRef ios = [dt rdna4IOSRef];
    uint8_t scratch[16384]; if (!src || !ios || RDNA4S.pitch > sizeof scratch || slot < 0 || slot >= RDNA4DF_MAX_SLOTS) return NULL;
    const int doslot = !atomic_load(&RDNA4C.noslot);
    const uint8_t *slotBase = NULL;
    if (doslot) {
        pthread_mutex_lock(&RDNA4S.mu);
        if (!RDNA4S.map[slot]) {
            void *m = NULL; VkResult mr = vkMapMemory(RDNA4R.dev, RDNA4S.mem[slot], 0, VK_WHOLE_SIZE, 0, &m);
            if (mr != VK_SUCCESS || !m) { if (!RDNA4C.mapfail++) RDNA4LOG("scanout: CRC diagnostic: vkMapMemory(slot %d) = %d: the slot CRC is skipped", slot, mr); pthread_mutex_unlock(&RDNA4S.mu); return NULL; }
            RDNA4S.map[slot] = m;
        }
        slotBase = RDNA4S.map[slot];
        pthread_mutex_unlock(&RDNA4S.mu);
    }
    rdna4crc_rec_t *r = calloc(1, sizeof *r); if (!r) return NULL;
    r->ios = (IOSurfaceRef)CFRetain(ios); r->base = src; r->pitch = RDNA4S.pitch; r->rowbytes = RDNA4S.pitch; r->h = RDNA4S.ph; r->step = atomic_load(&RDNA4C.step); r->sid = IOSurfaceGetID(ios); r->slot = slot; r->seq = seq;
    rdna4crc_samp_t a2, b; uint64_t t0 = rdna4_now(), ts = 0;
    rdna4crc_sample(&r->a1, src, r->pitch, r->rowbytes, r->h, r->step, scratch);
    if (doslot) { uint64_t t1 = rdna4_now(); rdna4crc_sample(&b, slotBase, r->pitch, r->rowbytes, r->h, r->step, scratch); ts = rdna4_now() - t1; }
    rdna4crc_sample(&a2, src, r->pitch, r->rowbytes, r->h, r->step, scratch);
    uint64_t dtn = rdna4_now() - t0;
    pthread_mutex_lock(&RDNA4C.mu);
    int later = 0; for (int i = 0; i < 8; i++) if (RDNA4C.surf[i].sid == r->sid) { later = RDNA4C.surf[i].seq > seq; break; }
    int f = rdna4crc_account(&RDNA4C.st, &r->a1, &a2, doslot ? &b : NULL, dtn, ts, later); r->unst = (f & RDNA4CRC_F_UNSTABLE) != 0;
    int lg = f && RDNA4C.st.logged < 20 ? (int)++RDNA4C.st.logged : 0;
    pthread_mutex_unlock(&RDNA4C.mu);
    if (lg) {
        uint32_t d1[8] = { 0 }, d2[8] = { 0 }; int n1 = doslot ? rdna4crc_diff(&r->a1, &b, d1, 8) : 0, n2 = rdna4crc_diff(&r->a1, &a2, d2, 8);
        RDNA4LOG("scanout: CRC MISMATCH #%d: seq %llu surface %u slot %d step %u:%s%s slot!=src rows %d (y: %u %u %u %u), src-unstable rows %d (y: %u %u %u %u), %s, check cost %.0f us (slot read %.0f us)", lg, (unsigned long long)seq, r->sid, slot, r->step,
               (f & RDNA4CRC_F_SLOT_NE) ? " SLOT_NE_SRC" : "", (f & RDNA4CRC_F_UNSTABLE) ? " SRC_UNSTABLE" : "", n1, d1[0], n1 > 1 ? d1[1] : 0, n1 > 2 ? d1[2] : 0, n1 > 3 ? d1[3] : 0,
               n2, d2[0], n2 > 1 ? d2[1] : 0, n2 > 2 ? d2[2] : 0, n2 > 3 ? d2[3] : 0,
               later ? "a LATER command buffer for this surface was submitted" : "NO later command buffer: written outside our command buffers", dtn / 1000.0, ts / 1000.0);
    }
    return r;
}

@interface RDNA4CommandBuffer : _MTLCommandBuffer {
    VkCommandBuffer _vk; VkFence _fence; BOOL _ended; RDNA4CommandQueue *_nq;
    NSMutableArray *_refs; RDNA4Buffer *_ring; NSUInteger _ringUsed; VkDescriptorPool _pool; NSError *_encErr, *_prepareErr;
    VkDescriptorSet _argumentSets[3]; uint32_t _argumentTokenCounts[5];
    RDNA4Buffer *_argumentTable; NSUInteger _argumentTableOffset, _argumentTableBytes;
    __weak NSSet<RDNA4Buffer *> *_bufferAddressResources;
    NSMutableArray<RDNA4CommandSegment *> *_segments; RDNA4CommandSegment *_curSegment;
    uint64_t _segmentSerial;
    NSUInteger _nextSegment; uint64_t _startTime, _readyTime; BOOL _prepared, _hasEvents, _inFlight, _submitAttempted, _cbSubmitLogged, _dispSeqLogged;
    BOOL _scheduleAfterPublish;
    pthread_mutex_t _smu;   // serializes scheduler advancement against completion-queue state changes
    _Atomic int _scheduledDelivered, _terminalClaimed;
    NSMutableArray *_iot, *_iow, *_eventDirty;   // 11h.6: whole-CB roots touched / display candidates written; IOSurface writes not yet flushed at a segment boundary
    uint64_t _dseq;   // S5.2a/#12: frame sequence, taken at submission (under the submit lock) = GPU order
    uint32_t _dsid;   // #12: IOSurface id of the display surface this cb writes (0 = unknown)
    int _dcls; uint64_t _dcid, _dbid;   // #12 D2: write class of the presented frame; chain id of this frame and of its base (0 = full copy)
    int _dslot;   // S5.2a: the scanout slot this command buffer's appended copy targets (-1 = none)
    int _dinst;   // bundle 13: the instance that slot belongs to (0 = the DP's, 2 = the monitor B's)
    BOOL _dcrc, _dlate; RDNA4Texture *_dtex;   // CRC source or a slotless display write awaiting a deferred copy
    BOOL _shaderFallback, _displayIncomplete;
    rdna4_display_commit _displayCommits[4];
    struct rdna4dw { __unsafe_unretained RDNA4Texture *t; uint32_t mask; const void *lastpso; char fns[128]; rdna4df_wr_t wr; } _dw[4]; int _ndw;   // native #12 P1: per display surface written, how it was written
    uint64_t _tBeg; BOOL _frame;   // native #12 T1: creation time; wrote a display surface (a "frame")
    uint64_t _cbid, _tcommit; uint32_t _cbw[RDNA4CB_MAXW], _cbr[RDNA4CB_MAXR], _ncbw, _ncbr; int _cbcls; uint32_t _cbdsid;   // Stage 0b: log id, commit time, IOSurface ids written / read (sampled or loaded), display write class+1 and sid
    NSUInteger _prot;   // gap census: protection options
    uint64_t _pser;   // P1: fence serial taken when this command buffer was created (0 = pool OFF); closed when its fence completed / it failed to submit / it was dropped unsubmitted
    struct rdna4ojob { rdna4occ *o; VkQueryPool qp; uint32_t base; } *_oj; uint32_t _noj, _coj; NSMutableArray *_ojb; NSMutableArray *_qpools; uint32_t _qpUsed; VkQueryPool _qpCur;   // build 18 (P2): per-pass occlusion jobs (+ their visibility buffers), the query pools of this command buffer, the chunk cursor of the newest one
    rdna4uc_list _uc; BOOL _ucLive; rdna4ia _ia;   // build 16: F3 the surfaces this command buffer use-counts while in flight (rdna4UCRelease gives them back, exactly once); P1 the per-range aliasing state
    RDNA4NativeJournal _native;
    NSMutableArray *_residencySnapshots;
    NSMutableSet *_residencyHeapResources;
}
- (VkCommandBuffer)rdna4BlitVK;
- (void)rdna4NativeRecord:(RDNA4NativeOp)op;
- (BOOL)rdna4NativeEligible;
- (int)rdna4NativeExecute:(bool *)fallback;
- (void)rdna4PoolDone;
- (void)rdna4ArgumentWrite:(id)buffer range:(NSRange)range candidates:(NSSet *)candidates fillOnly:(BOOL)fillOnly;
- (void)rdna4ArgumentWriteRanges:(id)buffer ranges:(NSArray<NSValue *> *)ranges candidates:(NSSet *)candidates;
- (void)rdna4ArgumentIntent:(id)intent;
- (void)rdna4CancelUnsubmittedArgumentIntents;
- (rdna4occ *)rdna4OccNew:(RDNA4Buffer *)vb accumulate:(int)acc pool:(VkQueryPool *)qp base:(uint32_t *)base;   // build 18 (P2)
- (void)rdna4OccResolve;   // build 18 (P2): called from rdna4PoolDone
- (void)rdna4PrepareForSubmission;
- (BOOL)rdna4AdvanceWithQueue:(RDNA4CommandQueue *)queue;
- (BOOL)rdna4OpenSegment:(NSError **)err;
- (BOOL)rdna4EndCurrentSegment:(NSError **)err;
- (BOOL)rdna4HasArgumentWrites;
- (void)rdna4ArgumentCheckpoint;
- (void)rdna4UpdateFence:(id)f;
- (void)rdna4WaitForFence:(id)f;
- (int)rdna4WaitsReady:(RDNA4CommandSegment *)segment error:(NSError **)err;
- (void)rdna4SignalSegment:(RDNA4CommandSegment *)segment;
- (void)rdna4ReleaseSegmentDescriptorPools:(RDNA4CommandSegment *)segment;
- (void)rdna4FlushEventWrites:(VkCommandBuffer)vk;
- (void)rdna4FailPendingSignals;
- (void)rdna4FailPendingFenceUpdates:(VkResult)fault;
- (void)rdna4EnsureScheduledWithError:(NSError *)err;
- (BOOL)rdna4ClaimTerminal;
- (void)rdna4DeliverTerminalSuccessWithQueue:(RDNA4CommandQueue *)queue;
- (void)rdna4DeliverTerminalFailure:(NSError *)err queue:(RDNA4CommandQueue *)queue unresolved:(BOOL)unresolved fault:(VkResult)fault;
- (void)rdna4TerminalSuccessWithQueue:(RDNA4CommandQueue *)queue;
- (void)rdna4TerminalFailure:(NSError *)err queue:(RDNA4CommandQueue *)queue unresolved:(BOOL)unresolved fault:(VkResult)fault;
- (void)rdna4FenceWait:(RDNA4CommandSegment *)segment queue:(RDNA4CommandQueue *)queue;
- (void)rdna4FenceDone:(RDNA4CommandSegment *)segment queue:(RDNA4CommandQueue *)queue r:(VkResult)r t1:(uint64_t)t1 slices:(int)slices delivered:(BOOL)delivered werr:(NSError *)werr;
- (uint64_t)rdna4TBegin;
- (BOOL)rdna4IsFrame;
- (BOOL)rdna4IOFirstTouch:(RDNA4Texture *)t;
- (rdna4ia_act)rdna4IOAlias:(RDNA4Texture *)t need:(BOOL)need write:(BOOL)w;   // build 16 (P1): the aliasing decision for this touch
- (RDNA4Texture *)rdna4IOTex:(uintptr_t)key;
- (void)rdna4IOUnwrite:(RDNA4Texture *)t;
- (void)rdna4UCRelease;
- (void)rdna4IOWritten:(RDNA4Texture *)t;
- (void)rdna4CBTraceSurface:(RDNA4Texture *)texture use:(const char *)use;
- (void)rdna4NoteHostWriteback:(RDNA4Texture *)texture;
- (void)rdna4NoteSurfaceOverwrite:(RDNA4Texture *)texture width:(uint32_t)width height:(uint32_t)height level:(uint32_t)level;
- (void)rdna4CBRead:(RDNA4Texture *)t;   // Stage 0b: t's IOSurface is sampled / loaded by this command buffer
- (void)rdna4CBSubmit:(uint64_t)tq;    // Stage 0b: call with the submit lock held, right before vkQueueSubmit (after rdna4DispSeq)
- (void)rdna4CBDropWithError:(NSError *)error fault:(VkResult)fault;
- (void)rdna4CBFence:(int)vkr;
- (void)rdna4DispDraw:(RDNA4Texture *)t bits:(uint32_t)bits rect:(rdna4df_rect_t)r w:(uint32_t)w h:(uint32_t)h clear:(BOOL)clr src:(RDNA4Texture *)src vp:(MTLViewport)vp sc:(MTLScissorRect)sc;   // native #12 D1: the extent of one draw
- (void)rdna4DispNote:(RDNA4Texture *)t bits:(uint32_t)bits pso:(RDNA4RenderPipelineState *)p;   // native #12 P1: a write of kind `bits` to t (ignored unless t is a display surface)
- (void)rdna4NoteShaderFallback;
- (VkCommandBuffer)vk;
- (BOOL)rdna4Finish:(NSError **)err;      // vkEndCommandBuffer
- (BOOL)rdna4EncodeOrderedHostAction:(BOOL (^)(NSError **))action error:(NSError **)error;
- (void)rdna4ClearHostActions;
- (void)rdna4DispSeq;
- (void)rdna4DisplayCommit;
- (void)rdna4DisplayUncommit;
- (void)rdna4DispDone:(VkResult)r;     // S5.2a: the appended copy ended (r) or never ran; presents on VK_SUCCESS only; idempotent
- (VkFence)fence;
- (void)setFence:(VkFence)f;
- (void)rdna4Retain:(id)o;
- (void)rdna4RetainArgument:(id)object;
- (void)rdna4RetainEncoded:(id)object;
- (uint64_t)rdna4SegmentSerial;
- (void)rdna4UseResource:(id)resource usage:(NSUInteger)usage;
- (void)rdna4UseHeap:(id)heap;
- (void)useResidencySet:(id)residencySet;
- (void)useResidencySets:(const id __unsafe_unretained *)residencySets count:(NSUInteger)count;
- (void)rdna4AttachResidencySnapshot:(id)snapshot;
- (void)rdna4AttachQueueResidencySnapshots;
- (NSSet *)rdna4ResidencyDeclaredResources;
- (void)rdna4Fail:(NSString *)why;
- (NSError *)rdna4Error;
- (BOOL)rdna4Bytes:(const void *)p length:(NSUInteger)n buffer:(RDNA4Buffer * __strong *)ob offset:(NSUInteger *)oo;
- (VkDescriptorSet)rdna4AllocSet:(VkDescriptorSetLayout)l;
- (BOOL)rdna4FillSet:(VkDescriptorSet)set bindings:(const RDNA4PB *)pb count:(uint32_t)n state:(const RDNA4StageState *)st stage:(const char *)nm;
- (BOOL)rdna4PrepareArguments:(const RDNA4ArgumentTables *)tables resources:(NSSet *)resources writable:(NSSet *)writable scan:(const RDNA4ArgumentScanPrepared *)scan textures:(void (^)(RDNA4Texture *, VkImageLayout, BOOL))prepare;
- (void)rdna4BindArguments:(const RDNA4ArgumentTables *)tables point:(VkPipelineBindPoint)point layout:(VkPipelineLayout)layout;
@end

#include "rdna4_encoder_bindings.inc"

static pthread_mutex_t rdna4_event_state_mu = PTHREAD_MUTEX_INITIALIZER;
static NSMapTable *rdna4_plain_event_values;
static NSMapTable *rdna4_event_failures;

static NSMapTable *rdna4_plain_event_map(void) {
    if (!rdna4_plain_event_values)
        rdna4_plain_event_values = [NSMapTable mapTableWithKeyOptions:NSPointerFunctionsWeakMemory | NSPointerFunctionsObjectPointerPersonality
                                                        valueOptions:NSPointerFunctionsStrongMemory];
    return rdna4_plain_event_values;
}

static NSMapTable *rdna4_event_failure_map(void) {
    if (!rdna4_event_failures)
        rdna4_event_failures = [NSMapTable mapTableWithKeyOptions:NSPointerFunctionsWeakMemory | NSPointerFunctionsObjectPointerPersonality
                                                    valueOptions:NSPointerFunctionsStrongMemory];
    return rdna4_event_failures;
}

static BOOL rdna4_event_is_shared(id event) {
    return [event respondsToSelector:@selector(signaledValue)] &&
        [event respondsToSelector:@selector(setSignaledValue:)];
}

static uint64_t rdna4_event_value(id event) {
    if (rdna4_event_is_shared(event)) return [(id<MTLSharedEvent>)event signaledValue];
    pthread_mutex_lock(&rdna4_event_state_mu);
    NSNumber *value = [rdna4_plain_event_map() objectForKey:event];
    pthread_mutex_unlock(&rdna4_event_state_mu);
    return value ? value.unsignedLongLongValue : 0;
}

static void rdna4_event_set_value(id event, uint64_t value) {
    if (rdna4_event_is_shared(event)) {
        [(id<MTLSharedEvent>)event setSignaledValue:value];
        return;
    }
    pthread_mutex_lock(&rdna4_event_state_mu);
    NSMapTable *values = rdna4_plain_event_map();
    NSNumber *old = [values objectForKey:event];
    if (!old || old.unsignedLongLongValue < value) [values setObject:@(value) forKey:event];
    pthread_mutex_unlock(&rdna4_event_state_mu);
}

static void rdna4_event_fail_signals(NSArray *signals) {
    if (!signals.count) return;
    pthread_mutex_lock(&rdna4_event_state_mu);
    NSMapTable *failures = rdna4_event_failure_map();
    for (NSArray *s in signals) {
        NSNumber *old = [failures objectForKey:s[0]];
        uint64_t value = [s[1] unsignedLongLongValue];
        if (!old || old.unsignedLongLongValue < value) [failures setObject:@(value) forKey:s[0]];
    }
    pthread_mutex_unlock(&rdna4_event_state_mu);
}

static BOOL rdna4_event_failed(id event, uint64_t value) {
    pthread_mutex_lock(&rdna4_event_state_mu);
    NSNumber *failed = rdna4_event_failures ? [rdna4_event_failures objectForKey:event] : nil;
    pthread_mutex_unlock(&rdna4_event_state_mu);
    return failed && failed.unsignedLongLongValue >= value;
}

static pthread_mutex_t rdna4_schedule_mu = PTHREAD_MUTEX_INITIALIZER;
static NSHashTable *rdna4_schedule_queues;
static dispatch_queue_t rdna4_schedule_q;
static BOOL rdna4_schedule_active, rdna4_schedule_dirty, rdna4_schedule_retry;
static void rdna4_schedule_kick(void);

static void rdna4_schedule_register(RDNA4CommandQueue *queue) {
    pthread_mutex_lock(&rdna4_schedule_mu);
    if (!rdna4_schedule_queues) rdna4_schedule_queues = [NSHashTable weakObjectsHashTable];
    if (!rdna4_schedule_q) rdna4_schedule_q = dispatch_queue_create("rdna4.event-scheduler", DISPATCH_QUEUE_SERIAL);
    [rdna4_schedule_queues addObject:queue];
    pthread_mutex_unlock(&rdna4_schedule_mu);
}

static void rdna4_schedule_unregister(RDNA4CommandQueue *queue) {
    pthread_mutex_lock(&rdna4_schedule_mu);
    [rdna4_schedule_queues removeObject:queue];
    pthread_mutex_unlock(&rdna4_schedule_mu);
}

static void rdna4_schedule_kick(void) {
    dispatch_queue_t queue = nil;
    BOOL launch = NO;
    pthread_mutex_lock(&rdna4_schedule_mu);
    if (!rdna4_schedule_q) rdna4_schedule_q = dispatch_queue_create("rdna4.event-scheduler", DISPATCH_QUEUE_SERIAL);
    queue = rdna4_schedule_q;
    if (rdna4_schedule_active) rdna4_schedule_dirty = YES;
    else { rdna4_schedule_active = YES; rdna4_schedule_dirty = NO; launch = YES; }
    pthread_mutex_unlock(&rdna4_schedule_mu);
    if (!launch) return;
    dispatch_async(queue, ^{
        for (;;) {
            NSArray *queues = nil;
            BOOL waiting = NO;
            pthread_mutex_lock(&rdna4_schedule_mu);
            queues = rdna4_schedule_queues ? rdna4_schedule_queues.allObjects : @[];
            pthread_mutex_unlock(&rdna4_schedule_mu);
            for (RDNA4CommandQueue *q in queues) waiting |= [q rdna4AdvanceHead];
            pthread_mutex_lock(&rdna4_schedule_mu);
            if (rdna4_schedule_dirty) {
                rdna4_schedule_dirty = NO;
                pthread_mutex_unlock(&rdna4_schedule_mu);
                continue;
            }
            rdna4_schedule_active = NO;
            BOOL retry = waiting && !rdna4_schedule_retry;
            if (retry) rdna4_schedule_retry = YES;
            dispatch_queue_t q = rdna4_schedule_q;
            pthread_mutex_unlock(&rdna4_schedule_mu);
            if (retry) {
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_MSEC), q, ^{
                    pthread_mutex_lock(&rdna4_schedule_mu);
                    rdna4_schedule_retry = NO;
                    pthread_mutex_unlock(&rdna4_schedule_mu);
                    rdna4_schedule_kick();
                });
            }
            return;
        }
    });
}

@implementation RDNA4CommandQueue
- (instancetype)initWithDevice:(id)dev descriptor:(id)desc error:(NSError **)err {
    id qd = desc ? [desc copy] : [[NSClassFromString(@"MTLCommandQueueDescriptor") alloc] init];
    if ([[qd valueForKey:@"maxCommandBufferCount"] unsignedIntegerValue] == 0) [qd setValue:@64 forKey:@"maxCommandBufferCount"];   // base asserts on 0
    self = [super initWithDevice:dev descriptor:qd];
    if (!self) { if (err) *err = rdna4_err(50, @"_MTLCommandQueue initWithDevice:descriptor: returned nil"); return nil; }
    pthread_mutex_init(&_qmu, NULL);
    _pending = [NSMutableArray array];
    _residencySets = [NSMutableArray array];
    // The base deallocator needs its dispatch objects even when RADV is unavailable.
    if (!rdna4_radv_open(err)) return nil;
    VkCommandPoolCreateInfo cpc = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = RDNA4R.qfi,
                                    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT };
    VkResult r = vkCreateCommandPool(RDNA4R.dev, &cpc, NULL, &_pool);
    if (r != VK_SUCCESS) { if (err) *err = rdna4_err(51, [NSString stringWithFormat:@"vkCreateCommandPool = %d", r]); return nil; }
    _ndev = dev; _cq = dispatch_queue_create("rdna4.completion", DISPATCH_QUEUE_SERIAL);
    rdna4_schedule_register(self);
    RDNA4LOG("RDNA4CommandQueue %p: pool %p", (__bridge void *)self, (void *)_pool);
    return self;
}
- (void)dealloc {
    rdna4_schedule_unregister(self);
    if (RDNA4R.ok && _pool) vkDestroyCommandPool(RDNA4R.dev, _pool, NULL);
    pthread_mutex_destroy(&_qmu);
}
RDNA4_DNR(RDNA4CommandQueue)
- (VkCommandPool)pool { return _pool; }
- (id)device { return _ndev; }
- (dispatch_queue_t)rdna4CompletionQueue { return _cq; }
- (void)rdna4Kick { rdna4_schedule_kick(); }
- (void)rdna4DidComplete:(RDNA4CommandBuffer *)cb {
    BOOL removed = NO;
    pthread_mutex_lock(&_qmu);
    if (_pending.firstObject == cb) { [_pending removeObjectAtIndex:0]; removed = YES; }
    pthread_mutex_unlock(&_qmu);
    if (!removed) RDNA4LOG("completion for a command buffer that is no longer this queue's head");
    rdna4_schedule_kick();
}
- (BOOL)rdna4AdvanceHead {
    RDNA4CommandBuffer *head = nil;
    pthread_mutex_lock(&_qmu);
    if (_pending.count) head = _pending.firstObject;
    pthread_mutex_unlock(&_qmu);
    return head ? [head rdna4AdvanceWithQueue:self] : NO;
}
// ---- gap census: MTLCommandQueueSPI, sent by SkyLight right after -newCommandQueue (census rdt1). Priorities are recorded; there is one hardware queue. ----
// Return values as measured on an Apple-silicon Mac (mtlprobe apigaps): setGPUPriority: YES, the background setters NO.
- (BOOL)setGPUPriority:(NSUInteger)p { _gprio = p; RDNA4_ONCE("queue setGPUPriority: recorded only"); return YES; }
- (BOOL)setGPUPriority:(NSUInteger)p offset:(uint16_t)o { (void)o; _gprio = p; return YES; }
- (BOOL)setBackgroundGPUPriority:(NSUInteger)p { _bprio = p; return NO; }
- (BOOL)setBackgroundGPUPriority:(NSUInteger)p offset:(uint16_t)o { (void)o; _bprio = p; return NO; }
- (BOOL)_setGPUPriority:(NSUInteger)p backgroundPriority:(NSUInteger)b { _gprio = p; _bprio = b; RDNA4_ONCE("queue _setGPUPriority:backgroundPriority: recorded only"); return NO; }
- (NSUInteger)getGPUPriority { return _gprio; }
- (NSUInteger)getBackgroundGPUPriority { return _bprio; }
- (void)setCompletionQueue:(id)q { _compQ = q; RDNA4_ONCE("queue setCompletionQueue: recorded only (completion handlers run on the bundle's completion thread)"); }
- (void)addResidencySet:(id)residencySet {
    if (!residencySet) return;
    if (![residencySet respondsToSelector:@selector(rdna4CommittedSnapshot)] ||
        ![residencySet respondsToSelector:@selector(device)] ||
        [residencySet device] != _ndev) {
        RDNA4_ONCE("queue addResidencySet: invalid residency set or device mismatch; ignored");
        return;
    }
    pthread_mutex_lock(&_qmu);
    if ([_residencySets indexOfObjectIdenticalTo:residencySet] == NSNotFound)
        [_residencySets addObject:residencySet];
    pthread_mutex_unlock(&_qmu);
}
- (void)addResidencySets:(const id __unsafe_unretained *)residencySets count:(NSUInteger)count {
    if (!residencySets || !count) return;
    for (NSUInteger i = 0; i < count; i++) [self addResidencySet:residencySets[i]];
}
- (void)removeResidencySet:(id)residencySet {
    if (!residencySet) return;
    pthread_mutex_lock(&_qmu);
    [_residencySets removeObjectIdenticalTo:residencySet];
    pthread_mutex_unlock(&_qmu);
}
- (void)removeResidencySets:(const id __unsafe_unretained *)residencySets count:(NSUInteger)count {
    if (!residencySets || !count) return;
    for (NSUInteger i = 0; i < count; i++) [self removeResidencySet:residencySets[i]];
}
- (NSArray *)rdna4ResidencySetSnapshots {
    NSArray *sets = nil;
    pthread_mutex_lock(&_qmu);
    if (_residencySets.count) sets = [_residencySets copy];
    pthread_mutex_unlock(&_qmu);
    if (!sets.count) return @[];
    NSMutableArray *snaps = [NSMutableArray arrayWithCapacity:sets.count];
    for (id<RDNA4ResidencySetInternal> rs in sets) {
        id<RDNA4ResidencySnapshotInternal> snap = [rs rdna4CommittedSnapshot];
        if (snap) [snaps addObject:snap];
    }
    return snaps;
}
- (id)commandBuffer {
    RDNA4LOGR("queue commandBuffer");
    RDNA4CommandBuffer *cb = [[RDNA4CommandBuffer alloc] initWithQueue:self retainedReferences:YES];
    [cb rdna4AttachQueueResidencySnapshots];
    return cb;
}
- (id)commandBufferWithUnretainedReferences {
    RDNA4LOGR("queue commandBufferWithUnretainedReferences");
    RDNA4CommandBuffer *cb = [[RDNA4CommandBuffer alloc] initWithQueue:self retainedReferences:NO];
    [cb rdna4AttachQueueResidencySnapshots];
    return cb;
}
- (id)commandBufferWithDescriptor:(id)d {
    (void)d; RDNA4LOGR("queue commandBufferWithDescriptor:");
    RDNA4CommandBuffer *cb = [[RDNA4CommandBuffer alloc] initWithQueue:self retainedReferences:YES];
    [cb rdna4AttachQueueResidencySnapshots];
    return cb;
}
- (void)submitCommandBuffers:(id __unsafe_unretained const *)cbs count:(NSUInteger)n {
    RDNA4LOGR("queue submitCommandBuffers:count:%lu", (unsigned long)n);
    for (NSUInteger i = 0; i < n; i++) {
        RDNA4CommandBuffer *cb = (RDNA4CommandBuffer *)cbs[i];
        if (!cb) continue;
        uint64_t t0 = rdna4_now();
        rdna4t_add(&T1.enc, t0 - [cb rdna4TBegin]);
        [cb rdna4PrepareForSubmission];
        BOOL isFrame = [cb rdna4IsFrame];
        if (isFrame) { atomic_fetch_add(&rdna4_t1_frames, 1); uint64_t lf = atomic_exchange(&rdna4_t1_lastframe, t0); if (lf) rdna4t_add(&T1.fgap, t0 - lf); }
        pthread_mutex_lock(&_qmu);
        [_pending addObject:cb];
        pthread_mutex_unlock(&_qmu);
    }
    if (n) rdna4_schedule_kick();
}
// Base version runs right after submitCommandBuffers:count: (F4) and would complete before the GPU is done.
- (void)completeCommandBuffers:(id __unsafe_unretained *)cbs count:(NSUInteger)n {
    (void)cbs; RDNA4LOG("queue completeCommandBuffers:count:%lu (deferred to the completion thread)", (unsigned long)n);
}
@end


// 11h.6 IOSurface coherency. Path (a) stays in GENERAL after its first submitted transition. Path (b) uploads at first use and copies
// back after writes. Uploads run outside render passes, and both paths make writes visible to the host before completing.
// Both run at GPU execution time, so CPU writes made before commit are seen and the CPU sees the result after the completion handler / waitUntilCompleted.
static void rdna4_ios_linear_init(VkCommandBuffer cmd, RDNA4Texture *texture, RDNA4Buffer *saved, NSUInteger bytes) {
    rdna4_full_barrier(cmd);
    VkBufferCopy copy = { .srcOffset = [texture rdna4IOSOffset], .dstOffset = [saved rdna4VkOffset], .size = bytes };
    vkCmdCopyBuffer(cmd, [texture rdna4IOSBuffer], [saved vkBuffer], 1, &copy);
    rdna4_full_barrier(cmd);
    rdna4_image_transition(cmd, texture, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    VkBufferImageCopy region = { .bufferOffset = [saved rdna4VkOffset],
        .bufferRowLength = (uint32_t)([texture rdna4IOSBytesPerRow] / [texture bytesPerPixel]),
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { (uint32_t)[texture width], (uint32_t)[texture height], 1 } };
    vkCmdCopyBufferToImage(cmd, [saved vkBuffer], [texture vkImage], VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    rdna4_full_barrier(cmd);
    VkMemoryBarrier host = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, NULL, 0, NULL);
}
static void rdna4_ios_upload(RDNA4CommandBuffer *cb, RDNA4Texture *t) {
    VkCommandBuffer cmd = [cb vk];
    rdna4_full_barrier(cmd);
    if ([t rdna4IOSLinear]) return;
    rdna4_image_transition(cmd, t, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy bic = { .bufferOffset = [t rdna4IOSOffset], .bufferRowLength = (uint32_t)([t rdna4IOSBytesPerRow] / [t bytesPerPixel]), .bufferImageHeight = 0,
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { (uint32_t)[t width], (uint32_t)[t height], 1 } };
    vkCmdCopyBufferToImage(cmd, [t rdna4IOSBuffer], [t vkImage], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
    rdna4_full_barrier(cmd);
}
static BOOL rdna4_ios_writeback(VkCommandBuffer cmd, RDNA4Texture *t) {
    if (rdna4nc_drop_write(t, RDNA4NC_WRITEBACK, "IOSurface write-back", "BlitCommandEncoder")) return NO;
    if (![t rdna4IsIOS]) return YES;   // P6: a base-less display texture is in _iow but has no host copy
    if (![t rdna4IOSLinear]) {
        rdna4_tex_to(cmd, t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy bic = { .bufferOffset = [t rdna4IOSOffset], .bufferRowLength = (uint32_t)([t rdna4IOSBytesPerRow] / [t bytesPerPixel]), .bufferImageHeight = 0,
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { (uint32_t)[t width], (uint32_t)[t height], 1 } };
        vkCmdCopyImageToBuffer(cmd, [t vkImage], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, [t rdna4IOSBuffer], 1, &bic);
    }
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    return YES;
}
static void rdna4_host_visibility_barrier(VkCommandBuffer cmd) {
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}
// P6: the D-copy of a display texture that has only a VkImage (protected IOSurface, no CPU mapping): image -> scanout slot buffer. Layout: the image is moved to TRANSFER_SRC_OPTIMAL with rdna4_tex_to
// (a full-memory barrier from its current tracked layout, which is what the last render pass / blit left it in; [t layout] is updated, so a later pass starts from TRANSFER_SRC like after rdna4_ios_writeback);
// the caller brackets this with rdna4_full_barrier (writes before, slot writes visible after). Same plan as rdna4s_record_copy: full = one region of the whole image; D2 partial = the base slot first
// (buffer -> buffer), a WAW barrier, then the rect from the image at the same byte offsets (rdna4df_img_region). The image is the plane's size and 4 bytes per texel (checked before the slot is taken).
static void rdna4s_record_copy_img(VkCommandBuffer cmd, RDNA4Texture *t, const rdna4df_plan_t *pl) {
    int slot = pl->slot; rdna4df_imgreg_t g;
    const rdna4x_desc_t *const xd = rdna4x_desc((uint32_t)pl->inst);   // bundle 15: the instance's geometry for an HDMI plan
    const uint32_t gw = xd ? xd->w : RDNA4S.pw, gh = xd ? xd->h : RDNA4S.ph, gp = xd ? xd->pitch_bytes : RDNA4S.pitch;
    VkDeviceSize all = (VkDeviceSize)gp * gh;
    rdna4df_rect_t full = { 0, 0, (int32_t)gw, (int32_t)gh };
    rdna4_tex_to(cmd, t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    if (pl->base >= 0) {
        VkBufferCopy bc = { .srcOffset = 0, .dstOffset = 0, .size = all }; vkCmdCopyBuffer(cmd, RDNA4S.buf[pl->base], RDNA4S.buf[slot], 1, &bc);
        VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);   // WAW: the region copy must land after the base copy
    }
    if (!rdna4df_img_region(pl->base >= 0 ? pl->rect : full, gw, gh, gp, 4, &g)) return;   // empty partial rectangle: only the base copy (same as the buffer path); full frames were validated by rdna4df_img_ok
    VkBufferImageCopy bic = { .bufferOffset = g.buf_off, .bufferRowLength = g.row_len, .bufferImageHeight = 0, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageOffset = { (int32_t)g.x, (int32_t)g.y, 0 }, .imageExtent = { g.w, g.h, 1 } };
    vkCmdCopyImageToBuffer(cmd, [t vkImage], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, xd ? RDNA4XI(pl->inst).buf[slot] : RDNA4S.buf[slot], 1, &bic);   // the planned instance's slot buffer
    atomic_fetch_add(&rdna4s_imgcopies, 1);
}
// Build 16 (P1): the touch of an IOSurface texture `t` by command buffer `cb`: retains it, takes the F3 use count, and does what the aliasing rules (rdna4_ioalias.h) demand before the caller's own commands:
//   * a PATH (b) writer that another wrapper of the same surface range touches is copied back NOW (and leaves the end-of-command-buffer write-back list), so the pages are current;
//   * a path (b) toucher whose image is stale (another wrapper wrote since) uploads again, even if it was touched before in this command buffer.
// endPass (may be NULL) is called, once, before the first recorded command: a render encoder ends its Vulkan render pass there (a copy cannot be recorded inside one).
// WindowServer and the kill file /private/tmp/rdna4m-noioalias keep build 14's rule exactly (upload at the first touch that needs the contents) and only COUNT the cases the rules would have acted on.
// Returns YES when commands were recorded. The caller guarantees [t rdna4IsIOS].
static BOOL rdna4_ios_touch(RDNA4CommandBuffer *cb, RDNA4Texture *t, BOOL need, BOOL write, void (^endPass)(void)) {
    const BOOL first = [cb rdna4IOFirstTouch:t];
    t = [t rootResource];   // Views share one image: a second first-use upload would overwrite earlier GPU writes.
    const rdna4ia_act d = [cb rdna4IOAlias:t need:need write:write];
    BOOL did = NO;
    if (!rdna4_ioalias_acts()) {
        if (d.flush) atomic_fetch_add(&RDNA4LED.iaWsFlush, 1);
        if (d.reupload) atomic_fetch_add(&RDNA4LED.iaWsReup, 1);
        if (first && need) { if (endPass) endPass(); rdna4_ios_upload(cb, t); did = YES; }
        return did;
    }
    if (d.flush) {
        RDNA4Texture *x = [cb rdna4IOTex:d.flush];
        if (x && [x rdna4IsIOS]) {
            if (endPass) endPass();
            if (!rdna4_ios_writeback([cb vk], x)) {
                [cb rdna4CBTraceSurface:x use:"writeback-refused"];
                [cb rdna4Fail:@"IOSurface alias write-back destination is read-only"];
                return NO;
            }
            [cb rdna4NoteHostWriteback:x];
            rdna4_full_barrier([cb vk]);       // ... and the transfer write must be visible to whatever the GPU does next with those pages
            [cb rdna4IOUnwrite:x];             // the end-of-command-buffer write-back must not copy the (now older) image over a later writer
            atomic_fetch_add(&RDNA4LED.iaFlush, 1); did = YES;
        }
    }
    if (d.upload) {
        if (endPass) endPass();
        rdna4_ios_upload(cb, t);
        if (d.reupload) atomic_fetch_add(&RDNA4LED.iaReup, 1);
        did = YES;
    }
    return did;
}
// Call OUTSIDE a render pass. needContents NO = the caller clears the whole texture, so the upload is skipped.
static void rdna4_ios_usek(RDNA4CommandBuffer *cb, RDNA4Texture *t, BOOL needContents, BOOL write, uint32_t kind) {
    if (![t rdna4IsIOS]) {
        if ([t rdna4DispImgOnly]) {   // P6: a protected display surface has no host copy to upload or write back; it only needs to be retained and recorded as written so the D-copy reads its image
            (void)[cb rdna4IOFirstTouch:t];
            if (needContents) [cb rdna4CBRead:t];
            if (write) { [cb rdna4IOWritten:t]; if (kind) [cb rdna4DispNote:t bits:kind pso:nil]; }
        }
        return;
    }
    (void)rdna4_ios_touch(cb, t, needContents, write, NULL);   // build 16 (P1): the upload / flush decision; was: first touch && needContents -> upload
    if (needContents) [cb rdna4CBRead:t];
    if (write) { [cb rdna4IOWritten:t]; if (kind) [cb rdna4DispNote:t bits:kind pso:nil]; }
}
static void rdna4_ios_use(RDNA4CommandBuffer *cb, RDNA4Texture *t, BOOL needContents, BOOL write) { rdna4_ios_usek(cb, t, needContents, write, 0); }

