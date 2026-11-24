#include "rdna4_fencewait.h"
_Static_assert((int)VK_TIMEOUT == RDNA4FW_TIMEOUT, "VK_TIMEOUT == RDNA4FW_TIMEOUT");

@protocol RDNA4ICBInternal <NSObject>
- (id<MTLDevice>)device;
- (uint64_t)rdna4EnsureGPUBackingAddress;
- (id)rdna4GPUBacking;
- (BOOL)rdna4PrepareForGPUProducerOnCommandBuffer:(id)cb;
- (BOOL)rdna4RegisterCandidatePipeline:(id)pipeline;
@end

@class RDNA4AccelHandle;
@protocol RDNA4AccelInternal <NSObject>
- (RDNA4AccelHandle *)rdna4CurrentHandle;
@end

typedef struct {
    VkFence fence;
    uint64_t cbid;
    uint64_t t1;
} rdna4_fence_wait_ctx;

static dispatch_queue_t rdna4_fence_wait_queue(void) {
    static dispatch_queue_t queue;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        dispatch_queue_attr_t attr = dispatch_queue_attr_make_with_qos_class(
            DISPATCH_QUEUE_CONCURRENT, QOS_CLASS_USER_INTERACTIVE, 0);
        queue = dispatch_queue_create("rdna4.fence-wait", attr);
    });
    return queue;
}

static int rdna4_fence_wait_slice(void *ctx, uint64_t timeoutNs) {
    rdna4_fence_wait_ctx *w = (rdna4_fence_wait_ctx *)ctx;
    return (int)vkWaitForFences(RDNA4R.dev, 1, &w->fence, VK_TRUE, timeoutNs);
}

static void rdna4_fence_wait_pending(void *ctx, int slices) {
    const rdna4_fence_wait_ctx *w = (const rdna4_fence_wait_ctx *)ctx;
    const uint64_t now = rdna4_now();
    const uint64_t elapsedMs = now > w->t1 ? (now - w->t1) / 1000000ULL : 0ULL;
    RDNA4LOG("FENCE PENDING: command buffer %llu has not completed after %llu ms (%d slice(s))",
             (unsigned long long)w->cbid, (unsigned long long)elapsedMs, slices);
}

@implementation RDNA4CommandBuffer
- (BOOL)rdna4IOFirstTouch:(RDNA4Texture *)t {
    RDNA4Texture *backing = [t rootResource];
    if (backing != t) [self rdna4Retain:t];
    t = backing;
    BOOL first = YES;
    if (_curSegment) {
        if (!_curSegment->touches) _curSegment->touches = [NSMutableArray array];
        first = [_curSegment->touches indexOfObjectIdenticalTo:t] == NSNotFound;
        if (first) [_curSegment->touches addObject:t];
    }
    if (!_iot) { _iot = [NSMutableArray array]; _iow = [NSMutableArray array]; }
    if ([_iot indexOfObjectIdenticalTo:t] == NSNotFound) {
        [_iot addObject:t]; [self rdna4Retain:t];
        rdna4_led_latch();
        if ([t rdna4UCSurface]) {   // F3: from the first touch until the command buffer is finished with the GPU the surface is use-counted (rdna4UCRelease); a buffer-backed texture has no surface and no ledger entry
            (void)rdna4uc_take(&_uc, RDNA4LED.ucMode, (void *)[t rdna4UCSurface], rdna4_uc_inc, NULL);
            rdna4_led_touch(t, 1); _ucLive = YES;
        }
    }
    return first;
}
- (rdna4ia_act)rdna4IOAlias:(RDNA4Texture *)t need:(BOOL)need write:(BOOL)w {
    t = [t rootResource];
    uint32_t sid = 0; uint64_t off = 0; [t rdna4AliasKey:&sid off:&off];
    return rdna4ia_touch(&_ia, sid, off, (uintptr_t)(__bridge void *)t, [t rdna4IOSLinear] ? 0 : 1, need ? 1 : 0, w ? 1 : 0);
}
- (RDNA4Texture *)rdna4IOTex:(uintptr_t)key { for (RDNA4Texture *x in _iot) if ((uintptr_t)(__bridge void *)x == key) return x; return nil; }
- (void)rdna4IOUnwrite:(RDNA4Texture *)t {
    [_iow removeObjectIdenticalTo:t];
    [_eventDirty removeObjectIdenticalTo:t];
}   // P1: flushed (copied back) mid-command-buffer: a later write-back must not copy the older image over a newer writer
// F3: release each use count once after completion, device loss, or no submission.
// Unresolved submissions remain retained and cannot reach dealloc.
- (void)rdna4UCRelease {
    if (!_ucLive) return;
    _ucLive = NO;
    rdna4uc_release(&_uc, rdna4_uc_dec, NULL);
    for (RDNA4Texture *t in _iot) if ([t rdna4UCSurface]) rdna4_led_touch(t, -1);
}
- (uint64_t)rdna4TBegin { return _tBeg; }
- (BOOL)rdna4IsFrame { return _frame; }
- (void)rdna4CBTraceSurface:(RDNA4Texture *)texture use:(const char *)use {
    if (!rdna4cbl_on()) return;
    texture = [texture rootResource];
    const BOOL readOnly = [texture rdna4HostReadOnly], display = [texture rdna4IsDisp];
    if ((!readOnly && !display) || [texture pixelFormat] != MTLPixelFormatBGRA8Unorm) return;
    IOSurfaceRef surface = [texture rdna4UCSurface];
    pthread_mutex_lock(&RDNA4CB.mu);
    rdna4cbl_line_locked("T t=%llu cb=%llu tex=%p base=%p sid=%u disp=%d ro=%d size=%lux%lu pitch=%zu use=%s",
        (unsigned long long)rdna4_now(), (unsigned long long)_cbid, (__bridge void *)texture, (const void *)[texture rdna4IBase],
        surface ? IOSurfaceGetID(surface) : 0, display, readOnly, (unsigned long)texture.width, (unsigned long)texture.height,
        [texture rdna4IOSBytesPerRow], use);
    pthread_mutex_unlock(&RDNA4CB.mu);
}
- (void)rdna4IOWritten:(RDNA4Texture *)t {
    t = [t rootResource];
    [self rdna4CBTraceSurface:t use:"write"];
    // IOSurfaces (both path (a) linear and path (b) copy-back) are imported
    // outside IOAccelerator fence tracking. Do not publish the command buffer as
    // scheduled until its GPU fence and host-visibility barrier complete; a
    // WindowServer, CA Whippet, or CPU capture consumer may read the surface as
    // soon as scheduled is visible. The /private/tmp/rdna4m-early-schedule kill
    // file restores early scheduling.
    if ([t rdna4IsIOS] &&
        access("/private/tmp/rdna4m-early-schedule", F_OK) != 0 && !_scheduleAfterPublish) {
        _scheduleAfterPublish = YES;
        RDNA4_ONCE("scheduled publication deferred until IOSurface write completes");
    }
    // rdna4Finish selects the last display write, including a revisit through a view.
    [_iow removeObjectIdenticalTo:t];
    [_iow addObject:t];
    if (!_eventDirty) _eventDirty = [NSMutableArray array];
    if ([_eventDirty indexOfObjectIdenticalTo:t] == NSNotFound) [_eventDirty addObject:t];
    if ([t rdna4IsIOS] && [t rdna4IOSRef]) _ncbw = rdna4cb_addsid(_cbw, _ncbw, RDNA4CB_MAXW, IOSurfaceGetID([t rdna4IOSRef]));
    else if ([t rdna4DispImgOnly]) _ncbw = rdna4cb_addsid(_cbw, _ncbw, RDNA4CB_MAXW, [t rdna4DispSid]);   // P6: the transaction correlation also sees protected display surfaces
}
- (void)rdna4CBRead:(RDNA4Texture *)t {
    [self rdna4CBTraceSurface:t use:"read"];
    IOSurfaceRef surface = [t rdna4IOSRef];
    if (!surface) {
        if (![t rdna4DispImgOnly]) return;
        _ncbr = rdna4cb_addsid(_cbr, _ncbr, RDNA4CB_MAXR, [t rdna4DispSid]);
        RDNA4Texture *root = [t rootResource];
        if (!_curSegment || !(rdna4_is_ws() || rdna4s_testmode()) || [_curSegment->displayImageOverwrites containsObject:root]) return;
        if (!_curSegment->displayImageReads)
            _curSegment->displayImageReads = [NSHashTable hashTableWithOptions:NSPointerFunctionsObjectPointerPersonality];
        [_curSegment->displayImageReads addObject:root];
        return;
    }
    const uint32_t sid = IOSurfaceGetID(surface);
    _ncbr = rdna4cb_addsid(_cbr, _ncbr, RDNA4CB_MAXR, sid);
    if (!_curSegment || !(rdna4_is_ws() || rdna4s_testmode()) ||
        _curSegment->displaySourceOverwrites[@(sid)] == [t rootResource]) return;
    if (!_curSegment->displaySourceReads) _curSegment->displaySourceReads = [NSMutableIndexSet indexSet];
    [_curSegment->displaySourceReads addIndex:sid];
}
// Stage 0b. -commit is stamped, logged, then handed to Metal's own implementation unchanged.
- (void)commit {
    [self rdna4AttachQueueResidencySnapshots];
    _tcommit = rdna4_now();
    [self rdna4DisplayCommit];
    if (rdna4cbl_on()) {
        uint64_t tid = 0; pthread_threadid_np(NULL, &tid);
        char w[160], r[256]; rdna4cbl_sids(w, sizeof w, _cbw, _ncbw); rdna4cbl_sids(r, sizeof r, _cbr, _ncbr);
        pthread_mutex_lock(&RDNA4CB.mu);
        rdna4cb_commit(&RDNA4CB.trk, _cbid, (uint64_t)(uintptr_t)(__bridge void *)_nq, _tcommit, _cbw, _ncbw, _cbr, _ncbr);
        rdna4cbl_line_locked("C t=%llu cb=%llu q=%#llx tid=%llu w=%s r=%s", (unsigned long long)_tcommit, (unsigned long long)_cbid, (unsigned long long)(uintptr_t)(__bridge void *)_nq, (unsigned long long)tid, w, r);
        pthread_mutex_unlock(&RDNA4CB.mu);
    }
    [super commit];
}
- (void)commitAndHold { [self commit]; }
- (void)rdna4DisplayCommit {
    pthread_mutex_lock(&RDNA4S.mu);
    for (int i = 0; i < _ndw; i++) {
        if (!_shaderFallback && !(_dw[i].mask & (RDNA4DF_W_DRAW_FINAL | RDNA4DF_W_DRAW_COMP))) continue;
        const uint32_t sid = [_dw[i].t rdna4DispSid];
        if (!sid || _displayCommits[i].sid) continue;
        rdna4dc_commit_add(&RDNA4S.commits, &_displayCommits[i], sid, _tcommit);
        rdna4dc_commit_invalidate(&RDNA4S.sm, sid, _tcommit);
        for (int inst = 1; inst <= 2; inst++) rdna4dc_commit_invalidate(&RDNA4XI(inst).sm, sid, _tcommit);
    }
    pthread_mutex_unlock(&RDNA4S.mu);
}
- (void)rdna4DisplayUncommit {
    pthread_mutex_lock(&RDNA4S.mu);
    for (int i = 0; i < 4; i++) rdna4dc_commit_remove(&RDNA4S.commits, &_displayCommits[i]);
    pthread_mutex_unlock(&RDNA4S.mu);
}
// WindowServer sends this private selector instead of -commit: its display swap
// follows immediately, so "scheduled" is not enough to make the composited
// surface safe for scanout. Wait for the terminal fence in WindowServer; apps
// keep Metal's normal submit-only contract. The kill file preserves the old
// behaviour for A/B diagnosis without another source change.
- (BOOL)commitAndWaitUntilSubmitted {
    const BOOL ws = rdna4_is_ws();
    [self commit];
    if (ws && access("/private/tmp/rdna4m-no-ws-completion", F_OK) != 0 &&
        ([self rdna4IsFrame] || access("/private/tmp/rdna4m-ws-completion-all", F_OK) == 0)) {
        RDNA4_ONCE("WindowServer commitAndWaitUntilSubmitted: waiting for GPU completion on display frames");
        // A display frame's completion wait is on WindowServer's frame path: when it
        // takes long, the picture on the monitor stops changing for exactly that long.
        // Log it (and how long) so a frozen screen is attributed to this wait instead
        // of being guessed at from missing presents.
        const uint64_t tw = rdna4_now();
        [self waitUntilCompleted];
        const uint64_t waited = rdna4_now() - tw;
        if (waited > 500000000ull) RDNA4LOG("FRAME WAIT STALL: WindowServer waited %.1f ms for command buffer %llu (%lu segment(s), status %lu): the display shows the previous frame until it returns",
                                            (double)waited / 1e6, (unsigned long long)_cbid, (unsigned long)_segments.count,
                                            (unsigned long)[self status]);
    } else {
        if (ws && access("/private/tmp/rdna4m-no-ws-completion", F_OK) == 0)
            RDNA4_ONCE("WindowServer commitAndWaitUntilSubmitted: completion wait disabled");
        [self waitUntilScheduled];
    }
    return [self status] != MTLCommandBufferStatusError;
}
- (void)rdna4CBSubmit:(uint64_t)tq {
    if (!rdna4cbl_on()) return;
    rdna4cb_inv_t inv[RDNA4CB_MAXINV]; uint64_t sq = 0, tid = 0; pthread_threadid_np(NULL, &tid);
    char w[160], r[256]; rdna4cbl_sids(w, sizeof w, _cbw, _ncbw); rdna4cbl_sids(r, sizeof r, _cbr, _ncbr);
    uint64_t t = rdna4_now(), q = (uint64_t)(uintptr_t)(__bridge void *)_nq;
    pthread_mutex_lock(&RDNA4CB.mu);
    uint32_t n = rdna4cb_submit(&RDNA4CB.trk, _cbid, q, t, _cbw, _ncbw, _cbr, _ncbr, inv, RDNA4CB_MAXINV, &sq);
    rdna4cba_submit(&RDNA4CB.activity);
    rdna4cbl_line_locked("S t=%llu tq=%llu cb=%llu q=%#llx tid=%llu sub=%llu dseq=%llu cmt=%llu cls=%s disp=%u slot=%d w=%s r=%s inv=%u", (unsigned long long)t, (unsigned long long)tq, (unsigned long long)_cbid, (unsigned long long)q,
        (unsigned long long)tid, (unsigned long long)sq, (unsigned long long)_dseq, (unsigned long long)_tcommit, _cbcls ? rdna4df_cls_name[_cbcls - 1] : "-", _cbdsid, _dslot, w, r, n);
    for (uint32_t i = 0; i < n && i < RDNA4CB_MAXINV && RDNA4CB.inv_logged < RDNA4_CBLOG_INV_LOGGED; i++) {
        RDNA4CB.inv_logged++;
        rdna4cbl_line_locked("I t=%llu rd_cb=%llu rd_q=%#llx rd_sub=%llu sid=%u wr_cb=%llu wr_q=%#llx wr_commit=%llu", (unsigned long long)t, (unsigned long long)inv[i].reader_id, (unsigned long long)inv[i].reader_queue,
            (unsigned long long)inv[i].reader_sub, inv[i].sid, (unsigned long long)inv[i].writer_id, (unsigned long long)inv[i].writer_queue, (unsigned long long)inv[i].writer_commit);
        RDNA4LOG("cblog: INVERSION #%llu: cb %llu (queue %#llx, submit #%llu) reads IOSurface %u whose writer cb %llu (queue %#llx, committed earlier) is not yet submitted", (unsigned long long)RDNA4CB.inv_logged,
            (unsigned long long)inv[i].reader_id, (unsigned long long)inv[i].reader_queue, (unsigned long long)inv[i].reader_sub, inv[i].sid, (unsigned long long)inv[i].writer_id, (unsigned long long)inv[i].writer_queue);
    }
    pthread_mutex_unlock(&RDNA4CB.mu);
}
- (void)rdna4CBDropWithError:(NSError *)error fault:(VkResult)fault {
    if (!rdna4cbl_on()) return;
    const char *phase = error && error == _encErr ? "encode" : "pre-submit";
    pthread_mutex_lock(&RDNA4CB.mu);
    rdna4cb_drop(&RDNA4CB.trk, _cbid);
    rdna4cba_result(&RDNA4CB.activity, 0);
    rdna4cbl_line_locked("D t=%llu cb=%llu phase=%s code=%ld vk=%d", (unsigned long long)rdna4_now(),
        (unsigned long long)_cbid, phase, (long)error.code, (int)fault);
    pthread_mutex_unlock(&RDNA4CB.mu);
}
- (void)rdna4CBFence:(int)vkr {
    if (!rdna4cbl_on()) return;
    pthread_mutex_lock(&RDNA4CB.mu);
    rdna4cba_result(&RDNA4CB.activity, vkr == VK_SUCCESS);
    rdna4cbl_line_locked("F t=%llu cb=%llu q=%#llx vk=%d", (unsigned long long)rdna4_now(), (unsigned long long)_cbid, (unsigned long long)(uintptr_t)(__bridge void *)_nq, vkr);
    pthread_mutex_unlock(&RDNA4CB.mu);
}
// native #12 P1: records HOW a display surface is written by this command buffer (OR-ed bits per surface; the pipeline names of every draw, deduplicated).
- (void)rdna4NoteShaderFallback { _shaderFallback = YES; }
- (void)rdna4DispNote:(RDNA4Texture *)t bits:(uint32_t)bits pso:(RDNA4RenderPipelineState *)p {
    t = [t rootResource];
    if (![t rdna4IsDisp]) return;
    int k = -1; for (int i = 0; i < _ndw; i++) if (_dw[i].t == t) { k = i; break; }
    if (k < 0) { if (_ndw >= 4) return; k = _ndw++; _dw[k].t = t; _dw[k].mask = 0; _dw[k].lastpso = NULL; _dw[k].fns[0] = 0; memset(&_dw[k].wr, 0, sizeof _dw[k].wr); }
    _dw[k].mask |= bits;
    if (bits & RDNA4DF_W_CLEAR) rdna4df_dmg_set_full(&_dw[k].wr.all);   // a clear load wrote the whole surface (for D2 it counts only when a presentable draw follows in the same pass: see rdna4DispDraw)
    if (bits & (RDNA4DF_W_BLIT | RDNA4DF_W_COMPUTE)) { rdna4df_dmg_set_full(&_dw[k].wr.all); rdna4df_dmg_set_full(&_dw[k].wr.pres); }   // unknown extent: the whole surface
    if (p && _dw[k].lastpso != (__bridge const void *)p) {
        _dw[k].lastpso = (__bridge const void *)p;
        if (RDNA4DO.on && atomic_load(&rdna4_dw_logged) >= 20) return;   // P5b: fns is only ever printed by the first 20 "scanout: display write" lines; nothing else reads it
        char one[140]; snprintf(one, sizeof one, "%s/%s ", [p rdna4VName], [p rdna4FName]);
        if (!strstr(_dw[k].fns, one) && strlen(_dw[k].fns) + strlen(one) < sizeof _dw[k].fns) strlcat(_dw[k].fns, one, sizeof _dw[k].fns);
    }
}
// native #12 D1: one draw on display surface t. `all` collects every draw (statistics); `pres` only the presentable ones (GPUPass, SkyLight composite; never ColorFill) plus, for the first such draw
// of a pass whose load action was Clear, the whole surface. The first presentable draw's first sampled IOSurface texture is the "source" (what GPUPass reads).
- (void)rdna4DispDraw:(RDNA4Texture *)t bits:(uint32_t)bits rect:(rdna4df_rect_t)r w:(uint32_t)w h:(uint32_t)h clear:(BOOL)clr src:(RDNA4Texture *)src vp:(MTLViewport)vp sc:(MTLScissorRect)sc {
    t = [t rootResource]; src = [src rootResource];
    int k = -1; for (int i = 0; i < _ndw; i++) if (_dw[i].t == t) { k = i; break; }
    if (k < 0) return;
    rdna4df_wr_t *wr = &_dw[k].wr;
    rdna4df_dmg_add_draw(&wr->all, r, w, h);
    if (!wr->vp_set) { wr->vp_set = 1; wr->vp[0] = vp.originX; wr->vp[1] = vp.originY; wr->vp[2] = vp.width; wr->vp[3] = vp.height; wr->sc[0] = (int64_t)sc.x; wr->sc[1] = (int64_t)sc.y; wr->sc[2] = (int64_t)sc.width; wr->sc[3] = (int64_t)sc.height; }
    if (bits & (RDNA4DF_W_DRAW_FINAL | RDNA4DF_W_DRAW_COMP)) {
        rdna4df_dmg_add_draw(&wr->pres, r, w, h);
        if (clr) rdna4df_dmg_set_full(&wr->pres);
        if (!wr->src_kind && src && [src rdna4IOSRef]) {
            uint32_t a = IOSurfaceGetID([src rdna4IOSRef]), b = [t rdna4IOSRef] ? IOSurfaceGetID([t rdna4IOSRef]) : 0;
            wr->src_sid = a; wr->src_w = (uint32_t)src.width; wr->src_h = (uint32_t)src.height;
            wr->src_kind = a == b ? 3 : [src rdna4IsDisp] ? 2 : 1;
        }
    }
}
- (instancetype)initWithQueue:(id)q retainedReferences:(BOOL)r {
    self = [super initWithQueue:q retainedReferences:r];
    if (!self) return nil;
    pthread_mutex_init(&_smu, NULL);
    _cbid = atomic_fetch_add(&rdna4cb_ids, 1) + 1; rdna4cbl_start();
    _dslot = -1; _nq = q; _refs = [NSMutableArray array];
    _residencySnapshots = [NSMutableArray array];
    _residencyHeapResources = [NSMutableSet set];
    _segments = [NSMutableArray array];
    NSError *segmentError = nil;
    if (![self rdna4OpenSegment:&segmentError]) {
        RDNA4LOG("command buffer segment allocation failed: %s", segmentError.localizedDescription.UTF8String);
        return nil;
    }
    _tBeg = rdna4_now();
    _pser = rdna4_fence_open();   // P1: fence-gated pool/backing reuse; unconditional for heap retirement when the pool is OFF
    if (!_pser) {
        RDNA4LOG("command buffer refused: GPU retirement timeline is unavailable (allocation failure)");
        return nil;
    }
    RDNA4LOGR("RDNA4CommandBuffer %p: vk %p begun", (__bridge void *)self, (void *)_vk);
    return self;
}
- (void)dealloc {
    [self rdna4DisplayUncommit];
    [self rdna4ClearHostActions];
    if (!_submitAttempted) [self rdna4CancelUnsubmittedArgumentIntents];
    [self rdna4FailPendingFenceUpdates:VK_ERROR_UNKNOWN];
    [self rdna4UCRelease]; rdna4uc_free(&_uc); rdna4ia_destroy(&_ia);   // build 16
    free(_native.ops);
    for (uint32_t j = 0; j < _noj; j++) free(_oj[j].o); free(_oj);   // build 18 (P2): bookkeeping of passes that never resolved
    if (RDNA4R.ok) {
        if (_pser && !_fence) rdna4_fence_close(_pser);   // P1: never submitted (a submitted one is closed by its completion block; a hung one stays open)
        for (RDNA4CommandSegment *segment in _segments)
            for (NSValue *v in segment->descriptorPools) {
                VkDescriptorPool dp_ = (VkDescriptorPool)v.pointerValue;
                if (!(RDNA4P.on && rdna4_dp_give(dp_)))
                    vkDestroyDescriptorPool(RDNA4R.dev, dp_, NULL);
            }
        for (NSValue *v in _qpools) vkDestroyQueryPool(RDNA4R.dev, (VkQueryPool)v.pointerValue, NULL);   // build 18 (P2)
        if (_dslot >= 0) { int s = _dslot; _dslot = -1; rdna4s_complete(_dinst, s, VK_ERROR_UNKNOWN, 0, 0, NULL, 0, 0); }   // never presented
        for (RDNA4CommandSegment *segment in _segments) if (segment->fence) vkDestroyFence(RDNA4R.dev, segment->fence, NULL);
        if (_segments.count && _nq) {
            VkCommandBuffer *commands = calloc(_segments.count, 2 * sizeof *commands);
            if (commands) {
                uint32_t count = 0;
                for (RDNA4CommandSegment *segment in _segments) {
                    if (segment->vk) commands[count++] = segment->vk;
                    if (segment->linearInit) commands[count++] = segment->linearInit;
                }
                if (count) {
                    rdna4_qlock();
                    vkFreeCommandBuffers(RDNA4R.dev, [_nq pool], count, commands);
                    pthread_mutex_unlock(&RDNA4R.qlock);
                }
                free(commands);
            }
        }
    }
    pthread_mutex_destroy(&_smu);
}
RDNA4_DNR(RDNA4CommandBuffer)
- (BOOL)rdna4OpenSegment:(NSError **)err {
    VkCommandBuffer vk = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = [_nq pool],
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    rdna4_qlock();
    VkResult r = vkAllocateCommandBuffers(RDNA4R.dev, &ai, &vk);
    pthread_mutex_unlock(&RDNA4R.qlock);
    if (r != VK_SUCCESS) {
        if (err) *err = rdna4_err(55, [NSString stringWithFormat:@"vkAllocateCommandBuffers(segment) = %d", r]);
        return NO;
    }
    RDNA4CommandSegment *segment = [RDNA4CommandSegment new];
    segment->vk = vk;
    [_segments addObject:segment];
    _curSegment = segment; _vk = vk;
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    r = vkBeginCommandBuffer(vk, &bi);
    if (r != VK_SUCCESS) {
        if (err) *err = rdna4_err(55, [NSString stringWithFormat:@"vkBeginCommandBuffer(segment) = %d", r]);
        return NO;
    }
    /*
     * A descriptor pool cannot be shared across command buffers while either
     * one can be in flight. The previous segment keeps its pools until its
     * fence; this segment starts with a fresh allocation pool.
     */
    _pool = VK_NULL_HANDLE;
    ++_segmentSerial;
    if (!_segmentSerial) ++_segmentSerial;
    return YES;
}

- (BOOL)rdna4EndCurrentSegment:(NSError **)err {
    if (!_curSegment || _curSegment->ended) return YES;
    BOOL signals = _curSegment->signals.count != 0;
    BOOL writes = _eventDirty.count != 0;
    [self rdna4FlushEventWrites:_curSegment->vk];
    if (_encErr) { if (err) *err = _encErr; return NO; }
    rdna4_full_barrier(_curSegment->vk);
    if (!rdna4_argument_summary_owners_encode(_curSegment->argumentIntents,
            _curSegment->vk, &_curSegment->argumentSummaryAttempts, err)) {
        if (!_encErr) _encErr = err && *err ? *err :
            rdna4_err(68, @"Private argument summary encoding failed");
        return NO;
    }
    if (_curSegment->argumentSummaryAttempts.count) _native.blocked = true;
    if (signals || writes) rdna4_host_visibility_barrier(_curSegment->vk);
    VkResult r = vkEndCommandBuffer(_curSegment->vk);
    _curSegment->ended = YES;
    if (r == VK_SUCCESS) {
        // A segment is a host-coherence boundary. Its alias state cannot outlive the
        // write-back above: the next segment starts from the current surface pages.
        rdna4ia_destroy(&_ia);
        rdna4ia_init(&_ia);
        return YES;
    }
    NSError *endError = rdna4_err(55, [NSString stringWithFormat:@"vkEndCommandBuffer(segment) = %d", r]);
    if (!_encErr) _encErr = endError;
    if (err) *err = endError;
    return NO;
}

- (BOOL)rdna4HasArgumentWrites {
    return _curSegment && !_curSegment->ended &&
        rdna4_argument_summary_owners_needed(_curSegment->argumentIntents);
}
- (void)rdna4ArgumentCheckpoint {
    if (_encErr || ![self rdna4HasArgumentWrites]) return;
    NSError *error = nil;
    // A segment boundary allows retirement before a later event-blocked writer.
    // Recording never submits or waits for a producer here.
    _hasEvents = YES;
    _native.blocked = true;
    if (![self rdna4EndCurrentSegment:&error] || ![self rdna4OpenSegment:&error])
        [self rdna4Fail:error.localizedDescription ?: @"argument checkpoint failed"];
}

- (BOOL)rdna4EncodeOrderedHostAction:(BOOL (^)(NSError **))action error:(NSError **)error {
    if (_ended || _prepared || !_curSegment || _curSegment->ended) {
        NSError *failure = rdna4_err(66, @"ordered host action encoded after command-buffer encoding ended");
        if (error) *error = failure;
        [self rdna4Fail:failure.localizedDescription];
        return NO;
    }
    if (!action) {
        NSError *failure = rdna4_err(66, @"ordered host action block is nil");
        if (error) *error = failure;
        [self rdna4Fail:failure.localizedDescription];
        return NO;
    }
    if (_encErr) {
        if (error) *error = _encErr;
        return NO;
    }

    // This is a scheduling barrier even when the preceding Vulkan segment is empty:
    // the action must not run through the whole-command-buffer native fast path.
    _hasEvents = YES;
    _native.blocked = true;
    NSError *segmentError = nil;
    if (![self rdna4EndCurrentSegment:&segmentError]) {
        if (error) *error = segmentError;
        [self rdna4Fail:(segmentError.localizedDescription ?: @"cannot end segment before ordered host action")];
        return NO;
    }

    RDNA4CommandSegment *host = nil;
    @try {
        host = [RDNA4CommandSegment new];
        host->hostOnly = YES;
        host->ended = YES;
        host->hasWork = YES;
        host->hostAction = [action copy];
    } @catch (NSException *exception) {
        NSError *failure = rdna4_err(66, [NSString stringWithFormat:@"ordered host action allocation failed: %@",
                                         exception.reason ?: @"out of memory"]);
        if (error) *error = failure;
        [self rdna4Fail:failure.localizedDescription];
        return NO;
    }
    @try {
        [_segments addObject:host];
    } @catch (NSException *exception) {
        NSError *failure = rdna4_err(66, [NSString stringWithFormat:@"ordered host action enqueue failed: %@",
                                         exception.reason ?: @"out of memory"]);
        if (error) *error = failure;
        [self rdna4Fail:failure.localizedDescription];
        return NO;
    }

    if (![self rdna4OpenSegment:&segmentError]) {
        if (error) *error = segmentError;
        [self rdna4Fail:(segmentError.localizedDescription ?: @"cannot open segment after ordered host action")];
        return NO;
    }
    return YES;
}

- (void)rdna4ClearHostActions {
    for (RDNA4CommandSegment *segment in _segments)
        if (segment->hostOnly) segment->hostAction = nil;
}

- (void)rdna4NoteHostWriteback:(RDNA4Texture *)texture {
    if (!_curSegment) return;
    if (![texture rdna4IsIOS]) {
        if (![texture rdna4DispImgOnly] || !(rdna4_is_ws() || rdna4s_testmode())) return;
        if (!_curSegment->displayImageWrites)
            _curSegment->displayImageWrites = [NSHashTable hashTableWithOptions:NSPointerFunctionsObjectPointerPersonality];
        [_curSegment->displayImageWrites addObject:[texture rootResource]];
        return;
    }
    if ([texture rdna4IOSLinear]) _curSegment->hostLinearWrites++;
    else _curSegment->hostWritebackCopies++;
    IOSurfaceRef surface = [texture rdna4IOSRef];
    if (!surface || !([texture rdna4IsDisp] || rdna4_is_ws() || rdna4s_testmode())) return;
    if (!_curSegment->displaySourceWrites) _curSegment->displaySourceWrites = [NSMutableIndexSet indexSet];
    [_curSegment->displaySourceWrites addIndex:IOSurfaceGetID(surface)];
}

- (void)rdna4NoteSurfaceOverwrite:(RDNA4Texture *)texture width:(uint32_t)width height:(uint32_t)height level:(uint32_t)level {
    if (!_curSegment || level || !(rdna4_is_ws() || rdna4s_testmode()) || [texture rootResource] != texture) return;
    if ([texture rdna4DispImgOnly]) {
        if (width != texture.width || height != texture.height) return;
        if (!_curSegment->displayImageOverwrites)
            _curSegment->displayImageOverwrites = [NSHashTable hashTableWithOptions:NSPointerFunctionsObjectPointerPersonality];
        [_curSegment->displayImageOverwrites addObject:texture];
        return;
    }
    IOSurfaceRef surface = [texture rdna4IOSRef];
    if (!surface || IOSurfaceGetPlaneCount(surface) > 1 || width != IOSurfaceGetWidth(surface) ||
        height != IOSurfaceGetHeight(surface) || width != texture.width || height != texture.height) return;
    if (!_curSegment->displaySourceOverwrites) _curSegment->displaySourceOverwrites = [NSMutableDictionary dictionary];
    _curSegment->displaySourceOverwrites[@(IOSurfaceGetID(surface))] = texture;
}

- (void)rdna4FlushEventWrites:(VkCommandBuffer)vk {
    if (!_eventDirty.count) return;
    // A CPU listener may touch the pages between segments, and a copy-path consumer
    // cannot wait for terminal completion. Make the pages current before the fence.
    RDNA4LOGR("segment boundary: host write-back for %lu IOSurface texture(s)", (unsigned long)_eventDirty.count);
    for (RDNA4Texture *t in _eventDirty) {
        if (!rdna4_ios_writeback(vk, t)) {
            [self rdna4CBTraceSurface:t use:"writeback-refused"];
            [self rdna4Fail:@"IOSurface write-back destination is read-only"];
            return;
        }
        [self rdna4NoteHostWriteback:t];
    }
    [_eventDirty removeAllObjects];
}

- (void)rdna4UpdateFence:(id)f {
    if (_ended || !_curSegment) { [self rdna4Fail:@"updateFence after the command buffer was finished"]; return; }
    if ([self rdna4Error]) return;
    if (![f isKindOfClass:[RDNA4Fence class]] || [(RDNA4Fence *)f device] != [self device]) {
        [self rdna4Fail:@"updateFence requires an RDNA4Fence from this device"];
        return;
    }
    RDNA4FenceCheckpoint *checkpoint = [(RDNA4Fence *)f rdna4UpdateCheckpointForCB:_cbid];
    if (!checkpoint) { [self rdna4Fail:@"updateFence generation overflow"]; return; }
    [self rdna4Retain:f];
    [_curSegment->fenceUpdates addObject:checkpoint];
    _curSegment->hasWork = YES;
    _hasEvents = YES;
    _native.blocked = true;
    rdna4_full_barrier(_vk);
}

- (void)rdna4WaitForFence:(id)f {
    if (_ended || !_curSegment) { [self rdna4Fail:@"waitForFence after the command buffer was finished"]; return; }
    if ([self rdna4Error]) return;
    if (![f isKindOfClass:[RDNA4Fence class]] || [(RDNA4Fence *)f device] != [self device]) {
        [self rdna4Fail:@"waitForFence requires an RDNA4Fence from this device"];
        return;
    }
    RDNA4FenceCheckpoint *checkpoint = [(RDNA4Fence *)f rdna4WaitCheckpoint];
    int state = atomic_load_explicit(&checkpoint->state, memory_order_acquire);
    if ((checkpoint->producerCBId && checkpoint->producerCBId == _cbid) || state == RDNA4FenceCheckpointReady) {
        // Intra-CB or already-satisfied fence dependency is ordered by an in-CB barrier; avoid splitting VkCommandBuffer segments.
        [self rdna4Retain:f];
        _curSegment->hasWork = YES;
        _hasEvents = YES;
        _native.blocked = true;
        rdna4_full_barrier(_vk);
        return;
    }
    BOOL empty = !_curSegment->hasWork && !_curSegment->waits.count && !_curSegment->signals.count &&
                 !_curSegment->fenceWaits.count && !_curSegment->fenceUpdates.count &&
                 !_curSegment->argumentIntents.count;
    NSError *error = nil;
    if (!empty && (![self rdna4EndCurrentSegment:&error] || ![self rdna4OpenSegment:&error])) {
        [self rdna4Fail:error.localizedDescription ?: @"cannot split the command buffer at waitForFence"];
        return;
    }
    [_curSegment->fenceWaits addObject:checkpoint];
    [self rdna4Retain:f];
    _curSegment->hasWork = YES;
    _hasEvents = YES;
    _native.blocked = true;
    rdna4_full_barrier(_vk);
}

// MTLSharedEvent values live on the event. A plain MTLEvent has no public value API, so a weak token
// map holds the maximum value signaled by this backend. Each event call is a command-buffer segment
// boundary: the preceding segment carries signals, the following segment carries waits. A queue-local
// scheduler advances only that queue's head, while all logical queues share one ready-scan thread.
- (void)encodeSignalEvent:(id)e value:(uint64_t)v {
    if (_ended || !_curSegment) { [self rdna4Fail:@"encodeSignalEvent after the command buffer was finished"]; return; }
    if (!rdna4_event_is_shared(e) && ![e conformsToProtocol:@protocol(MTLEvent)]) {
        [self rdna4Fail:@"encodeSignalEvent: the object is not a Metal event"]; return;
    }
    _hasEvents = YES; _native.blocked = true;
    [_curSegment->signals addObject:@[e, @(v)]];
    [self rdna4Retain:e];
    NSError *error = nil;
    if (![self rdna4EndCurrentSegment:&error] || ![self rdna4OpenSegment:&error])
        [self rdna4Fail:error.localizedDescription ?: @"cannot split the command buffer at encodeSignalEvent"];
}

- (void)encodeWaitForEvent:(id)e value:(uint64_t)v { [self encodeWaitForEvent:e value:v timeout:60000]; }
- (void)encodeWaitForEvent:(id)e value:(uint64_t)v timeout:(uint32_t)ms {
    if (_ended || !_curSegment) { [self rdna4Fail:@"encodeWaitForEvent after the command buffer was finished"]; return; }
    if (!rdna4_event_is_shared(e) && ![e conformsToProtocol:@protocol(MTLEvent)]) {
        [self rdna4Fail:@"encodeWaitForEvent: the object is not a Metal event"]; return;
    }
    _hasEvents = YES; _native.blocked = true;
    BOOL empty = !_curSegment->hasWork && !_curSegment->waits.count && !_curSegment->signals.count &&
                 !_curSegment->fenceWaits.count && !_curSegment->fenceUpdates.count &&
                 !_curSegment->argumentIntents.count;
    NSError *error = nil;
    if (!empty && (![self rdna4EndCurrentSegment:&error] || ![self rdna4OpenSegment:&error])) {
        [self rdna4Fail:error.localizedDescription ?: @"cannot split the command buffer at encodeWaitForEvent"];
        return;
    }
    NSMutableArray *wait = [@[e, @(v), @(ms), @0] mutableCopy];
    [_curSegment->waits addObject:wait];
    [self rdna4Retain:e];
}

- (int)rdna4WaitsReady:(RDNA4CommandSegment *)segment error:(NSError **)err {
    BOOL fencePending = NO;
    for (RDNA4FenceCheckpoint *checkpoint in segment->fenceWaits) {
        int state = atomic_load_explicit(&checkpoint->state, memory_order_acquire);
        if (state == RDNA4FenceCheckpointReady) continue;
        if (state == RDNA4FenceCheckpointFailed) {
            int fault = atomic_load_explicit(&checkpoint->fault, memory_order_relaxed);
            if (err) *err = rdna4_err(59, [NSString stringWithFormat:
                @"waitForFence: producer update generation %llu failed (VkResult %d)",
                (unsigned long long)checkpoint->generation, fault]);
            return -1;
        }
        fencePending = YES;
    }
    for (NSMutableArray *w in segment->waits) {
        id event = w[0];
        uint64_t value = [w[1] unsignedLongLongValue];
        uint32_t timeoutMS = [w[2] unsignedIntValue];
        if (rdna4_event_value(event) >= value) continue;
        if (rdna4_event_failed(event, value)) {
            if (err) *err = rdna4_err(58, [NSString stringWithFormat:@"encodeWaitForEvent: producer for value %llu failed", (unsigned long long)value]);
            return -1;
        }
        uint64_t now = rdna4_now(), started = [w[3] unsignedLongLongValue];
        if (!started) { started = now; [w replaceObjectAtIndex:3 withObject:@(started)]; }
        if (now - started >= (uint64_t)timeoutMS * 1000000ULL) {
            if (err) *err = rdna4_err(57, [NSString stringWithFormat:@"encodeWaitForEvent: value %llu not signaled within %u ms", (unsigned long long)value, timeoutMS]);
            return -1;
        }
        return 0;
    }
    if (fencePending) return 0;
    return 1;
}

- (void)rdna4SignalSegment:(RDNA4CommandSegment *)segment {
    if ((segment->hostWritebackCopies || segment->hostLinearWrites) && rdna4cbl_on()) {
        pthread_mutex_lock(&RDNA4CB.mu);
        rdna4cbl_line_locked("W t=%llu cb=%llu dseq=%llu copies=%lu linear=%lu", (unsigned long long)rdna4_now(),
            (unsigned long long)_cbid, (unsigned long long)_dseq, (unsigned long)segment->hostWritebackCopies,
            (unsigned long)segment->hostLinearWrites);
        rdna4cba_writeback(&RDNA4CB.activity);
        pthread_mutex_unlock(&RDNA4CB.mu);
    }
    // Completion is published only after both acceptance and a successful completion path.
    for (id intent in segment->argumentIntents) rdna4_argument_sampler_complete(intent);
    [segment->argumentIntents removeAllObjects];
    for (id attempt in segment->argumentSummaryAttempts) {
        NSError *error = nil;
        if (!rdna4_argument_summary_owners_finish(attempt, YES,
                segment->argumentSubmissionEpoch, &error)) {
            if (!_encErr) _encErr = error ?: rdna4_err(68,
                @"Private argument summary publication failed");
            RDNA4LOG("argument summary retained prior owners: %s",
                error.localizedDescription.UTF8String ?: "publication failed");
        }
    }
    segment->argumentSummaryAttempts = nil;
    // These immutable descriptor snapshots are used only by this segment.
    [segment->argumentPools removeAllObjects];
    [segment->argumentRefs removeAllObjects];
    [segment->commandRefs removeAllObjects];
    if (segment->vk || segment->linearInit) {
        VkCommandBuffer commands[2];
        uint32_t count = 0;
        if (segment->vk) commands[count++] = segment->vk;
        if (segment->linearInit) commands[count++] = segment->linearInit;
        rdna4_qlock();
        vkFreeCommandBuffers(RDNA4R.dev, [_nq pool], count, commands);
        pthread_mutex_unlock(&RDNA4R.qlock);
        if (_vk == segment->vk) _vk = VK_NULL_HANDLE;
        segment->vk = VK_NULL_HANDLE;
        segment->linearInit = VK_NULL_HANDLE;
    }
    [self rdna4ReleaseSegmentDescriptorPools:segment];
    for (RDNA4FenceCheckpoint *checkpoint in segment->fenceUpdates) [checkpoint rdna4Ready];
    for (NSArray *s in segment->signals) {
        id event = s[0];
        uint64_t value = [s[1] unsignedLongLongValue];
        rdna4_event_set_value(event, value);
        RDNA4LOG("event signaled to %llu at its encoded segment", (unsigned long long)value);
    }
}

- (void)rdna4ReleaseSegmentDescriptorPools:(RDNA4CommandSegment *)segment {
    if (!segment || !segment->descriptorPools.count) return;
    NSArray<NSValue *> *pools = [segment->descriptorPools copy];
    [segment->descriptorPools removeAllObjects];
    if (!RDNA4R.ok) return;
    for (NSValue *value in pools) {
        VkDescriptorPool pool = (VkDescriptorPool)value.pointerValue;
        if (_pool == pool) _pool = VK_NULL_HANDLE;
        if (!(RDNA4P.on && rdna4_dp_give(pool)))
            vkDestroyDescriptorPool(RDNA4R.dev, pool, NULL);
    }
}

- (void)rdna4FailPendingSignals {
    for (RDNA4CommandSegment *segment in _segments) {
        if (!segment->done && !segment->pruned) rdna4_event_fail_signals(segment->signals);
    }
}

- (void)rdna4FailPendingFenceUpdates:(VkResult)fault {
    if (fault == VK_SUCCESS) fault = VK_ERROR_UNKNOWN;
    for (RDNA4CommandSegment *segment in _segments)
        for (RDNA4FenceCheckpoint *checkpoint in segment->fenceUpdates)
            [checkpoint rdna4Fail:fault];
}

- (NSUInteger)protectionOptions { return _prot; }
- (void)setProtectionOptions:(NSUInteger)o { _prot = o; RDNA4_ONCE("cb setProtectionOptions: recorded only (no protected content)"); }
- (VkCommandBuffer)vk { _native.blocked = true; if (_curSegment) _curSegment->hasWork = YES; return _vk; }
- (VkCommandBuffer)rdna4BlitVK { if (_curSegment) _curSegment->hasWork = YES; return _vk; }
- (void)rdna4NativeRecord:(RDNA4NativeOp)op { rdna4_native_record(&_native, op); }
- (BOOL)rdna4NativeEligible {
    return !_native.blocked && _native.count && !_iot.count &&
        rdna4_vkQueueWaitIdle && rdna4_native_bridge.copy && rdna4_native_bridge.fill && rdna4_native_bridge.wait;
}
- (int)rdna4NativeExecute:(bool *)fallback {
    return rdna4_native_execute(&_native, rdna4_native_bridge, RDNA4R.dev, fallback);
}
- (id)device { return [_nq device]; }
- (VkFence)fence { return _fence; }
- (void)setFence:(VkFence)f { _fence = f; }
- (void)rdna4Retain:(id)o { if (o) [_refs addObject:o]; }
- (void)rdna4RetainArgument:(id)object {
    if (!object) return;
    if (!_curSegment || _curSegment->ended) {
        [self rdna4Fail:@"argument snapshot has no open command segment"];
        return;
    }
    [_curSegment->argumentRefs addObject:object];
}
- (void)rdna4RetainEncoded:(id)object {
    if (!object) return;
    if (!_curSegment || _curSegment->ended) {
        [self rdna4Fail:@"encoded resource snapshot has no open command segment"];
        return;
    }
    [_curSegment->commandRefs addObject:object];
}
- (uint64_t)rdna4SegmentSerial { return _segmentSerial; }
- (void)rdna4UseResource:(id)resource usage:(NSUInteger)usage {
    if (_encErr) return;
    Class icbCls = objc_getClass("RDNA4IndirectCommandBuffer");
    BOOL isICB = icbCls && [resource isKindOfClass:icbCls];
    Class asCls = objc_getClass("RDNA4AccelerationStructure");
    BOOL isAS = asCls && [resource isKindOfClass:asCls];
    if ((!(isICB || isAS || [resource isKindOfClass:[RDNA4Buffer class]] || [resource isKindOfClass:[RDNA4Texture class]])) ||
        [(id<MTLResource>)resource device] != [self device] || !usage || (usage & ~(NSUInteger)7)) {
        [self rdna4Fail:@"useResource has an invalid resource, device or usage"];
        return;
    }
    [self rdna4RetainEncoded:resource];
    if (isICB) {
        id backing = [(id<RDNA4ICBInternal>)resource rdna4GPUBacking];
        if (backing) [self rdna4RetainEncoded:backing];
    } else if (isAS) {
        id handle = [(id<RDNA4AccelInternal>)resource rdna4CurrentHandle];
        if (handle) [self rdna4RetainEncoded:handle];
    }
}
- (void)rdna4UseHeap:(id)heap {
    if (_encErr) return;
    Class spHeapCls = objc_getClass("RDNA4SparseHeap");
    BOOL isSparseHeap = spHeapCls && [heap isKindOfClass:spHeapCls];
    if ((!(isSparseHeap || [heap isKindOfClass:[RDNA4Heap class]])) || [heap device] != [self device]) {
        [self rdna4Fail:@"useHeap has an invalid heap or device"]; return;
    }
    [self rdna4RetainEncoded:heap];
    if (!isSparseHeap) {
        for (id resource in [heap rdna4LiveResources]) if (![resource isAliasable]) [self rdna4RetainEncoded:resource];
    }
}
- (void)rdna4AttachResidencySnapshot:(id)snapshot {
    if (!snapshot || ![snapshot respondsToSelector:@selector(rdna4Allocations)]) return;
    if (!_residencySnapshots) _residencySnapshots = [NSMutableArray array];
    if (!_residencyHeapResources) _residencyHeapResources = [NSMutableSet set];
    if ([_residencySnapshots indexOfObjectIdenticalTo:snapshot] == NSNotFound)
        [_residencySnapshots addObject:snapshot];
    for (id alloc in [(id<RDNA4ResidencySnapshotInternal>)snapshot rdna4Allocations]) {
        if ([alloc isKindOfClass:[RDNA4Heap class]]) {
            for (id resource in [(RDNA4Heap *)alloc rdna4LiveResources]) {
                if (![resource isAliasable]) [_residencyHeapResources addObject:resource];
            }
        }
    }
}
- (void)rdna4AttachQueueResidencySnapshots {
    if (!_nq) return;
    for (id snap in [_nq rdna4ResidencySetSnapshots])
        [self rdna4AttachResidencySnapshot:snap];
}
- (void)useResidencySet:(id)residencySet {
    if (_encErr || !residencySet) return;
    if (![residencySet respondsToSelector:@selector(rdna4CommittedSnapshot)] ||
        ![residencySet respondsToSelector:@selector(device)] ||
        [residencySet device] != [self device]) {
        [self rdna4Fail:@"useResidencySet has an invalid residency set or device"];
        return;
    }
    [self rdna4AttachResidencySnapshot:[(id<RDNA4ResidencySetInternal>)residencySet rdna4CommittedSnapshot]];
}
- (void)useResidencySets:(const id __unsafe_unretained *)residencySets count:(NSUInteger)count {
    if (_encErr || !count) return;
    if (!residencySets) {
        [self rdna4Fail:@"useResidencySets:count: has a NULL array"];
        return;
    }
    for (NSUInteger i = 0; i < count; i++) [self useResidencySet:residencySets[i]];
}
- (NSSet *)rdna4ResidencyDeclaredResources {
    [self rdna4AttachQueueResidencySnapshots];
    if (!_residencySnapshots.count && !_residencyHeapResources.count) return [NSSet set];
    NSMutableSet *out = [NSMutableSet set];
    for (id<RDNA4ResidencySnapshotInternal> snap in _residencySnapshots) {
        for (id obj in [snap rdna4Allocations]) [out addObject:obj];
    }
    for (id obj in _residencyHeapResources) [out addObject:obj];
    return out;
}
- (void)rdna4Fail:(NSString *)why {
    RDNA4LOG("ENCODE ERROR (command buffer will complete with an error): %s", why.UTF8String);
    if (!_encErr) _encErr = rdna4_err(66, why);
}
- (NSError *)rdna4Error { return _encErr; }
- (void)rdna4DispDone:(VkResult)r {
    if (_dlate) {
        _dlate = NO;
        if (r == VK_SUCCESS) rdna4s_late_complete(_dtex, _dinst, _dseq, _dsid, _dcls, _tcommit);
        _dtex = nil;
    }
    [self rdna4DisplayUncommit];
    int s = _dslot; if (s < 0) return; _dslot = -1;
    rdna4crc_rec_t *rec = (_dcrc && r == VK_SUCCESS && _dtex && _dinst == 0) ? rdna4s_crc_check(_dtex, s, _dseq) : NULL;   // bundle 13: the CRC diagnostic is the DP's alone
    _dtex = nil;
    rdna4s_complete(_dinst, s, r, _dseq, _dsid, rec, _dcls, _tcommit);
}
- (void)rdna4DispSourceWrites:(RDNA4CommandSegment *)segment {
    NSIndexSet *sources = segment->displaySourceWrites;
    NSIndexSet *reads = segment->displaySourceReads;
    if (!sources.count && !reads.count && !segment->displayImageWrites.count && !segment->displayImageReads.count) return;
    const uint64_t seq = sources.count ? atomic_fetch_add(&RDNA4S.seq, 1) + 1 : 0;
    pthread_mutex_lock(&RDNA4S.mu);
    const uint64_t now = rdna4_now();
    // SkyLight ping-pongs composite surfaces (e.g. 1 <-> 3) without MTLLoadActionClear,
    // so unbounded taint permanently suppresses scanout after any single transient fallback.
    // Expire taint after a fallback-quiet window and cap continuous suppression.
    if (RDNA4S.incompleteLastFallbackNs && now >= RDNA4S.incompleteLastFallbackNs &&
        now - RDNA4S.incompleteLastFallbackNs >= RDNA4DF_INCOMPLETE_QUIET_NS) {
        if (RDNA4S.incompleteSources.count || RDNA4S.incompleteImages.count) {
            RDNA4LOG("scanout: clearing incomplete display taint (%lu surface(s), %lu image(s)) after %.0f ms without shader fallback",
                     (unsigned long)RDNA4S.incompleteSources.count,
                     (unsigned long)RDNA4S.incompleteImages.count,
                     (double)(now - RDNA4S.incompleteLastFallbackNs) / 1e6);
            [RDNA4S.incompleteSources removeAllIndexes];
            [RDNA4S.incompleteImages removeAllObjects];
        }
        RDNA4S.incompleteSinceNs = 0;
        RDNA4S.incompleteLastFallbackNs = 0;
        RDNA4S.incompleteCapLogged = NO;
    }
    BOOL suppressFallback = _shaderFallback;
    if (_shaderFallback) {
        if (!RDNA4S.incompleteSinceNs) RDNA4S.incompleteSinceNs = now;
        RDNA4S.incompleteLastFallbackNs = now;
    }
    if (RDNA4S.incompleteSinceNs && now >= RDNA4S.incompleteSinceNs &&
        now - RDNA4S.incompleteSinceNs >= RDNA4DF_INCOMPLETE_MAX_NS) {
        suppressFallback = NO;
        if (RDNA4S.incompleteSources.count || RDNA4S.incompleteImages.count || !RDNA4S.incompleteCapLogged) {
            if (!RDNA4S.incompleteCapLogged) {
                RDNA4S.incompleteCapLogged = YES;
                RDNA4LOG("scanout: shader-fallback suppression cap (%.0f ms) reached; resuming presentation",
                         (double)(now - RDNA4S.incompleteSinceNs) / 1e6);
            }
            [RDNA4S.incompleteSources removeAllIndexes];
            [RDNA4S.incompleteImages removeAllObjects];
        }
    }
    if (!_displayIncomplete && RDNA4S.incompleteSources.count) {
        for (NSUInteger sid = reads.firstIndex; reads && sid != NSNotFound; sid = [reads indexGreaterThanIndex:sid]) {
            if (![RDNA4S.incompleteSources containsIndex:sid]) continue;
            _displayIncomplete = YES;
            RDNA4LOGR("scanout: command buffer %llu reads incomplete IOSurface %lu", (unsigned long long)_cbid, (unsigned long)sid);
            break;
        }
    }
    if (!_displayIncomplete && segment->displayImageReads.count &&
        [RDNA4S.incompleteImages intersectsHashTable:segment->displayImageReads]) {
        _displayIncomplete = YES;
        RDNA4LOGR("scanout: command buffer %llu reads an incomplete GPU-only IOSurface image", (unsigned long long)_cbid);
    }
    const BOOL incomplete = suppressFallback || _displayIncomplete;
    for (RDNA4Texture *image in segment->displayImageWrites) {
        if (incomplete) {
            if (!RDNA4S.incompleteImages) RDNA4S.incompleteImages = [NSHashTable hashTableWithOptions:
                NSPointerFunctionsWeakMemory | NSPointerFunctionsObjectPointerPersonality];
            [RDNA4S.incompleteImages addObject:image];
        } else if ([segment->displayImageOverwrites containsObject:image]) {
            [RDNA4S.incompleteImages removeObject:image];
        }
    }
    if (incomplete && sources.count && !RDNA4S.incompleteSources) RDNA4S.incompleteSources = [NSMutableIndexSet indexSet];
    for (NSUInteger sid = sources.firstIndex; sources && sid != NSNotFound; sid = [sources indexGreaterThanIndex:sid]) {
        if (incomplete) [RDNA4S.incompleteSources addIndex:sid];
        else if (segment->displaySourceOverwrites[@(sid)]) [RDNA4S.incompleteSources removeIndex:sid];
        const BOOL rejected = [RDNA4S.incompleteSources containsIndex:sid];
        for (int inst = 0; inst <= 2; ++inst) {
            rdna4df_t *sm = inst ? &RDNA4XI(inst).sm : &RDNA4S.sm;
            if (rejected) rdna4df_submit_rejected_write(sm, (uint32_t)sid, seq);
            else rdna4df_invalidate_deferred_source(sm, (uint32_t)sid, seq);
        }
    }
    pthread_mutex_unlock(&RDNA4S.mu);
}
- (void)rdna4DispSeq {
    if (_shaderFallback && _ndw) {
        _dseq = atomic_fetch_add(&RDNA4S.seq, 1) + 1;
        pthread_mutex_lock(&RDNA4S.mu);
        for (int writeIndex = 0; writeIndex < _ndw; ++writeIndex) {
            const uint32_t sid = [_dw[writeIndex].t rdna4DispSid];
            rdna4df_submit_rejected_write(&RDNA4S.sm, sid, _dseq);
            for (int inst = 1; inst <= 2; ++inst)
                rdna4df_submit_rejected_write(&RDNA4XI(inst).sm, sid, _dseq);
        }
        pthread_mutex_unlock(&RDNA4S.mu);
        return;
    }
    if (_dslot >= 0 || _dlate || (_dcrc && _dtex)) _dseq = atomic_fetch_add(&RDNA4S.seq, 1) + 1;
    if ((_dslot >= 0 || _dlate) && (_dsid || _dcid)) {
        pthread_mutex_lock(&RDNA4S.mu);
        rdna4df_t *const dsm = rdna4x_desc((uint32_t)_dinst) ? &RDNA4XI(_dinst).sm : &RDNA4S.sm;
        const BOOL targetIncomplete = [_dtex rdna4DispImgOnly] ? [RDNA4S.incompleteImages containsObject:_dtex]
            : [RDNA4S.incompleteSources containsIndex:_dsid];
        if (_displayIncomplete || targetIncomplete) {
            _dlate = NO;
            _dcrc = NO;
            _cbcls = RDNA4DF_C_SHADER_FALLBACK + 1;
            if (_dslot >= 0) {
                dsm->poison[_dslot] = 1;
                dsm->head = -1;
                if (_dcid) {
                    dsm->last_sub_id = _dcid;
                    dsm->last_sub_valid = 0;
                }
            }
            pthread_mutex_unlock(&RDNA4S.mu);
            rdna4cbl_skip(_dsid, _dseq, _dslot, "incomplete-source", 0);
            return;
        }
        if (_dsid) rdna4df_submit(dsm, _dslot, _dsid, _dseq);
        if (_dslot >= 0) {
            dsm->sub_commit[_dslot] = _tcommit;
            if (rdna4dc_commit_newer(RDNA4S.commits, _dsid, _tcommit)) dsm->commit_sup[_dslot] = 1;
        }
        rdna4df_chain_submit(dsm, _dslot, _dcid, _dbid);
        pthread_mutex_unlock(&RDNA4S.mu);
    }
    if (_dcrc && _dtex && [_dtex rdna4IOSRef]) rdna4s_crc_note(IOSurfaceGetID([_dtex rdna4IOSRef]), _dseq);   // CRC diagnostic: which surface this submission wrote
}   // call with the submit lock held, right before vkQueueSubmit
- (void)rdna4PrepareForSubmission {
    if (_prepared) return;
    _startTime = rdna4_now();
    NSError *error = nil;
    if (![self rdna4Finish:&error]) _prepareErr = error ?: _encErr ?: rdna4_err(55, @"command buffer preparation failed");
    _prepared = YES;
}
- (BOOL)rdna4Finish:(NSError **)err {
    if (_ended) return _encErr == nil;
    _ended = YES;
    if (_encErr) { if (err) *err = _encErr; return NO; }   // never submit a command buffer whose encoding failed
    // Only writes since the previous segment boundary. A signal segment already flushed and
    // cleared _eventDirty, so a CPU listener's later page changes cannot be overwritten here.
    [self rdna4FlushEventWrites:_vk];
    if (_encErr) { if (err) *err = _encErr; return NO; }
    {   // S5.2a: append barrier + copy (display texture's import -> a free scanout slot) + barrier; the completion block presents it
        RDNA4Texture *dt = nil; int nd = 0; uint32_t dmask = 0; const char *dfns = ""; BOOL dfinal = NO; const rdna4df_wr_t *dwr = NULL;
        int dmgon = rdna4s_damage_on();
        for (RDNA4Texture *t in _iow) if ([t rdna4IsDisp]) {   // the presented surface: the last presentable one (FINAL pass; with damage on also a SkyLight composite pass) in write order, else the last display surface written
            nd++; uint32_t m = 0; const char *f = ""; const rdna4df_wr_t *w = NULL;
            for (int i = 0; i < _ndw; i++) if (_dw[i].t == t) { m = _dw[i].mask; f = _dw[i].fns; w = &_dw[i].wr; break; }
            BOOL fin = (m & RDNA4DF_W_DRAW_FINAL) != 0 || (dmgon && (m & RDNA4DF_W_DRAW_COMP) != 0);
            if (!dt || fin || !dfinal) { dt = t; dmask = m; dfns = f; dfinal = fin; dwr = w; }
        }
        if (_shaderFallback) dmask |= RDNA4DF_W_SHADER_FALLBACK;
        if (nd > 1) { uint64_t m = atomic_fetch_add(&RDNA4S.multi, 1) + 1; if (m <= 8) RDNA4LOG("scanout: command buffer wrote %d display surfaces; the LAST one in write order is presented (study: frame-order ambiguity, #%llu)", nd, (unsigned long long)m); }
        if (dt) {
            _frame = YES; _cbcls = rdna4df_class(dmask) + 1; _cbdsid = [dt rdna4DispSid];
            BOOL imgsrc = [dt rdna4DispImgOnly];   // P6: a protected display surface: copy from its VkImage
            int pinst = 0, ptent = 0;   // bundle 13/15: the instance this frame is PLANNED for (0 = the DP, 1 = the monitor A, 2 = the monitor B) and whether the plan is only tentative (unknown surface: instance 0 only)
            BOOL take = rdna4s_disp_account(dmask, [dt rdna4DispSid], dfns, atomic_fetch_add(&RDNA4S.cbno, 1) + 1, dwr, &pinst, &ptent);   // native #12 P1/P2/D1: only a presentable frame takes a slot
            rdna4df_plan_t pl; pl.slot = -1; pl.inst = 0; pl.tent = 0;
            const rdna4x_desc_t *const pxd = rdna4x_desc((uint32_t)pinst);   // bundle 15: the planned instance's geometry (the DP's plane for instance 0)
            const uint32_t gw_ = pxd ? pxd->w : RDNA4S.pw, gh_ = pxd ? pxd->h : RDNA4S.ph, gp_ = pxd ? pxd->pitch_bytes : RDNA4S.pitch;
            // bundle 17 (R-B1): the width / height check applies to EVERY frame, not only the image-sourced ones; a buffer-sourced frame (the surface's pages) also needs bytes-per-row == the plan's pitch (the copy reads pitch*height bytes of the surface)
            if (take && !((imgsrc ? rdna4df_img_ok(gw_, gh_, gp_, 4) : (BOOL)YES) && [dt bytesPerPixel] == 4 && [dt width] == gw_ && [dt height] == gh_ && [dt rdna4Levels] == 1 && (imgsrc || [dt rdna4IOSBytesPerRow] == gp_))) {   // refuse before a slot is taken
                static _Atomic int rl; if (atomic_fetch_add(&rl, 1) < 8) RDNA4LOG("scanout: display surface %u cannot be copied to the slot (%lux%lu bpp %lu bpr %lu, %s; plan %ux%u pitch %u): frame not copied", [dt rdna4DispSid], (unsigned long)[dt width], (unsigned long)[dt height], (unsigned long)[dt bytesPerPixel], (unsigned long)(imgsrc ? 0 : [dt rdna4IOSBytesPerRow]), imgsrc ? "no CPU mapping" : "buffer source", gw_, gh_, gp_);
                take = NO; }
            int sl = take ? rdna4s_plan(dwr, pinst, ptent, &pl) : -1;
            // A mapped display copy reads the IOSurface pages (whether path (a) or (b)), so a CPU update
            // after the signal is visible without touching the texture. Only a protected, base-less
            // display surface copies from its image, and it has no CPU mapping to refresh.
            if (sl >= 0) { rdna4_full_barrier(_vk); if (imgsrc) rdna4s_record_copy_img(_vk, dt, &pl); else rdna4s_record_copy(_vk, [dt rdna4DispBuf], [dt rdna4IOSOffset], &pl); rdna4_full_barrier(_vk); _dtex = dt; _dslot = sl; _dinst = pl.inst; _dcls = rdna4df_class(dmask); _dcid = pl.id; _dbid = pl.baseid; _dsid = [dt rdna4DispSid]; }
            else if (take && !imgsrc) { _dlate = YES; _dtex = dt; _dinst = pinst; _dcls = rdna4df_class(dmask); _dsid = [dt rdna4DispSid]; }
            if (rdna4s_crc_on() && pinst == RDNA4X_PLAN_DP) { _dcrc = YES; _dtex = dt; }   // bundle 13: the CRC diagnostic reads the DP's slot: never a monitor B frame
        }
    }
    NSError *endError = nil;
    if (![self rdna4EndCurrentSegment:&endError]) { [self rdna4DispDone:VK_ERROR_UNKNOWN]; if (err) *err = endError; return NO; }
    if (_segments.count > 1 && _curSegment == _segments.lastObject && !_curSegment->hasWork &&
        !_curSegment->waits.count && !_curSegment->signals.count && !_curSegment->fenceWaits.count &&
        !_curSegment->fenceUpdates.count && !_curSegment->argumentIntents.count &&
        !_iow.count && !_iot.count)
        _curSegment->pruned = YES;
    for (RDNA4CommandSegment *segment in _segments.reverseObjectEnumerator) {
        if (segment->pruned) continue;
        segment->terminal = YES;
        break;
    }
    return YES;
}

- (void)rdna4EnsureScheduledWithError:(NSError *)err {
    if (_scheduleAfterPublish && err == nil) return;
    if (atomic_exchange(&_scheduledDelivered, 1)) return;
    [self didScheduleWithStartTime:_startTime endTime:rdna4_now() error:err];
    RDNA4LOGR("didSchedule sent (ok=%d)", err == nil);
}

- (BOOL)rdna4ClaimTerminal {
    return atomic_exchange(&_terminalClaimed, 1) == 0;
}
- (void)rdna4DeliverTerminalSuccessWithQueue:(RDNA4CommandQueue *)queue {
    rdna4_capture_surfaces(_iot, _cbid);
    [self rdna4ClearHostActions];
    dispatch_async([queue rdna4CompletionQueue], ^{
        self->_scheduleAfterPublish = NO;
        [self rdna4EnsureScheduledWithError:nil];
        [self rdna4CBFence:VK_SUCCESS];
        [self rdna4DispDone:VK_SUCCESS];
        [self rdna4PoolDone];
        [queue commandBufferDidComplete:self startTime:self->_startTime completionTime:rdna4_now() error:self->_encErr];
        [queue rdna4DidComplete:self];
        rdna4_mark_clean();
    });
}

- (void)rdna4DeliverTerminalFailure:(NSError *)err queue:(RDNA4CommandQueue *)queue unresolved:(BOOL)unresolved fault:(VkResult)fault {
    [self rdna4ClearHostActions];
    [self rdna4CancelUnsubmittedArgumentIntents];
    [self rdna4FailPendingFenceUpdates:fault];
    [self rdna4FailPendingSignals];
    if (unresolved) rdna4_quarantine(self, fault == VK_SUCCESS ? VK_ERROR_UNKNOWN : fault);
    dispatch_async([queue rdna4CompletionQueue], ^{
        [self rdna4EnsureScheduledWithError:err];
        if (!unresolved) {
            [self rdna4PoolDone];
            if (self->_cbSubmitLogged) [self rdna4CBFence:(int)(fault == VK_SUCCESS ? VK_ERROR_UNKNOWN : fault)];
            else [self rdna4CBDropWithError:err fault:(fault == VK_SUCCESS ? VK_ERROR_UNKNOWN : fault)];
            [self rdna4DispDone:VK_ERROR_UNKNOWN];
        } else {
            [self rdna4CBFence:(int)fault];
        }
        [queue commandBufferDidComplete:self startTime:self->_startTime completionTime:rdna4_now() error:err];
        [queue rdna4DidComplete:self];
    });
}
- (void)rdna4TerminalSuccessWithQueue:(RDNA4CommandQueue *)queue {
    if (![self rdna4ClaimTerminal]) return;
    [self rdna4DeliverTerminalSuccessWithQueue:queue];
}
- (void)rdna4TerminalFailure:(NSError *)err queue:(RDNA4CommandQueue *)queue unresolved:(BOOL)unresolved fault:(VkResult)fault {
    if (![self rdna4ClaimTerminal]) return;
    [self rdna4DeliverTerminalFailure:err queue:queue unresolved:unresolved fault:fault];
}

// The completion of a segment: the fence result `r`, the delivered flag of the test
// hook and the (optional) error the wait already built. Runs ON the serial completion
// queue, so every delivery stays ordered.
- (void)rdna4FenceDone:(RDNA4CommandSegment *)segment queue:(RDNA4CommandQueue *)queue r:(VkResult)r t1:(uint64_t)t1 slices:(int)slices
             delivered:(BOOL)delivered werr:(NSError *)werr {
    if (r != VK_SUCCESS && !werr)
        werr = rdna4_err(54, [NSString stringWithFormat:@"the GPU fence = %d%s", r, r == VK_ERROR_DEVICE_LOST ? " (DEVICE_LOST)" : r == VK_TIMEOUT ? " (TIMEOUT: HUNG?)" : ""]);
    { uint64_t tg_ = rdna4_now() - t1; rdna4t_add(&T1.gpu, tg_); if (self->_frame) rdna4t_add(&T1.fgpu, tg_); }
    RDNA4LOGR("segment fence wait done = %d after %d slice(s)", r, slices);
    BOOL terminal = NO;
    BOOL terminalClaimed = NO;
    pthread_mutex_lock(&self->_smu);
    if (r == VK_SUCCESS) segment->done = YES;
    terminal = segment->terminal;
    if (!delivered && (r != VK_SUCCESS || terminal)) terminalClaimed = [self rdna4ClaimTerminal];
    pthread_mutex_unlock(&self->_smu);
    if (r == VK_SUCCESS) {
        [self rdna4SignalSegment:segment];
        if (delivered) {
            [self rdna4CBFence:VK_SUCCESS];
            [self rdna4DispDone:VK_SUCCESS];
            [self rdna4PoolDone];
            [queue rdna4DidComplete:self];
            rdna4_mark_clean();
        } else if (terminalClaimed) {
            [self rdna4DeliverTerminalSuccessWithQueue:queue];
        } else if (!terminal) {
            pthread_mutex_lock(&self->_smu);
            self->_inFlight = NO;
            pthread_mutex_unlock(&self->_smu);
            [queue rdna4Kick];
        }
        return;
    }
    if (delivered) {
        [self rdna4FailPendingFenceUpdates:r];
        rdna4_quarantine(self, r);
        [queue rdna4DidComplete:self];
        return;
    }
    if (terminalClaimed) [self rdna4DeliverTerminalFailure:werr queue:queue unresolved:YES fault:r];
}
// Production wait: run bounded vkWaitForFences slices on a concurrent waiter queue
// so a stuck fence never blocks the serial completion queue or other queues, and
// normal completions wake immediately from kernel n1c_wait (no 2 ms timer floor).
- (void)rdna4FenceWait:(RDNA4CommandSegment *)segment queue:(RDNA4CommandQueue *)queue {
    static _Atomic int testUsed;
    static long testMs = -1;
    if (testMs < 0) { const char *e = getenv("RDNA4M_TEST_TIMEOUT_MS"); testMs = e ? atol(e) : 0; }
    BOOL testWait = testMs > 0 && atomic_exchange(&testUsed, 1) == 0;
    long tms = testMs;
    uint64_t t1 = _readyTime;
    uint64_t tenq = rdna4_now();
    if (!testWait) {
        dispatch_queue_t completionQueue = [queue rdna4CompletionQueue];
        const rdna4_fence_wait_ctx waitCtx = { .fence = segment->fence, .cbid = _cbid, .t1 = t1 };
        // Transfer +1 refs to completionQueue so the waiter thread never retains or
        // deallocates self/segment/queue after handoff.
        CFTypeRef cbRef = CFBridgingRetain(self);
        CFTypeRef segRef = CFBridgingRetain(segment);
        CFTypeRef qRef = CFBridgingRetain(queue);
        dispatch_async(rdna4_fence_wait_queue(), ^{
            rdna4t_add(&T1.wake, rdna4_now() - tenq);
            rdna4_fence_wait_ctx ctx = waitCtx;
            int slices = 0;
            const VkResult r = (VkResult)rdna4fw_wait_bounded(
                rdna4_fence_wait_slice, &ctx, 0, RDNA4FW_MAX_SLICES, RDNA4FW_SLICE_NS,
                rdna4_fence_wait_pending, &slices);
            if (r == VK_TIMEOUT) {
                const uint64_t now = rdna4_now();
                const uint64_t elapsedMs = now > t1 ? (now - t1) / 1000000ULL : 0ULL;
                RDNA4LOG("FENCE STALL: command buffer %llu: the GPU fence did not signal within %llu ms (%d slice(s)); failing this submission",
                         (unsigned long long)ctx.cbid, (unsigned long long)elapsedMs, slices);
            }
            dispatch_async(completionQueue, ^{
                RDNA4CommandBuffer *cb = (__bridge_transfer RDNA4CommandBuffer *)cbRef;
                RDNA4CommandSegment *seg = (__bridge_transfer RDNA4CommandSegment *)segRef;
                RDNA4CommandQueue *q = (__bridge_transfer RDNA4CommandQueue *)qRef;
                [cb rdna4FenceDone:seg queue:q r:r t1:t1 slices:slices delivered:NO werr:nil];
            });
        });
        return;
    }
    dispatch_async([queue rdna4CompletionQueue], ^{
        rdna4t_add(&T1.wake, rdna4_now() - tenq);
        NSError *werr = nil;
        VkResult r = vkWaitForFences(RDNA4R.dev, 1, &segment->fence, VK_TRUE, (uint64_t)tms * 1000000ULL);
        int slices = 1;
        BOOL delivered = NO;
        RDNA4LOG("TEST_TIMEOUT: first fence wait of %ld ms = %d", tms, r);
        if (r == VK_TIMEOUT && !self->_hasEvents && atomic_exchange(&self->_terminalClaimed, 1) == 0) {
            werr = rdna4_err(54, [NSString stringWithFormat:@"vkWaitForFences = %d (TIMEOUT after %ld ms; test hook, GPU still busy)", r, tms]);
            [queue commandBufferDidComplete:self startTime:t1 completionTime:rdna4_now() error:werr];
            RDNA4LOG("TEST_TIMEOUT: commandBufferDidComplete(error) sent; still waiting the fence before release");
            delivered = YES;
        }
        if (r == VK_TIMEOUT) {
            rdna4_fence_wait_ctx ctx = { .fence = segment->fence, .cbid = self->_cbid, .t1 = t1 };
            r = (VkResult)rdna4fw_wait_bounded(
                rdna4_fence_wait_slice, &ctx, slices, RDNA4FW_MAX_SLICES, RDNA4FW_SLICE_NS,
                NULL, &slices);
        }
        [self rdna4FenceDone:segment queue:queue r:r t1:t1 slices:slices delivered:delivered werr:werr];
    });
}

static NSUInteger rdna4_linear_image_span(RDNA4Texture *texture) {
    return ([texture height] - 1) * [texture rdna4IOSBytesPerRow] + [texture width] * [texture bytesPerPixel];
}

- (BOOL)rdna4PrepareLinearImages:(RDNA4CommandSegment *)segment error:(NSError **)error {
    NSUInteger scratchLength = 0;
    for (RDNA4Texture *texture in segment->touches) {
        if (![texture rdna4IOSLinearNeedsInit]) continue;
        const NSUInteger bytes = rdna4_linear_image_span(texture);
        scratchLength = MAX(scratchLength, bytes);
    }
    if (!scratchLength) return YES;
    RDNA4Buffer *saved = [[RDNA4Buffer alloc] initWithDevice:[self device] length:scratchLength
        options:MTLResourceStorageModePrivate | RDNA4_OPT_NOZERO error:error];
    if (!saved) return NO;
    [segment->commandRefs addObject:saved];

    VkCommandBufferAllocateInfo allocation = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = [_nq pool], .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer init = VK_NULL_HANDLE;
    rdna4_qlock();
    VkResult result = vkAllocateCommandBuffers(RDNA4R.dev, &allocation, &init);
    pthread_mutex_unlock(&RDNA4R.qlock);
    if (result == VK_SUCCESS) {
        segment->linearInit = init;
        VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        result = vkBeginCommandBuffer(init, &begin);
    }
    if (result != VK_SUCCESS) {
        *error = rdna4_err(55, [NSString stringWithFormat:@"IOSurface initialization command begin = %d", result]);
        return NO;
    }
    for (RDNA4Texture *texture in segment->touches)
        if ([texture rdna4IOSLinearNeedsInit])
            rdna4_ios_linear_init(init, texture, saved, rdna4_linear_image_span(texture));
    result = vkEndCommandBuffer(init);
    if (result == VK_SUCCESS) return YES;
    *error = rdna4_err(55, [NSString stringWithFormat:@"IOSurface initialization command end = %d", result]);
    return NO;
}

- (BOOL)rdna4AdvanceWithQueue:(RDNA4CommandQueue *)queue {
    pthread_mutex_lock(&_smu);
    if (atomic_load(&_terminalClaimed) || _inFlight) {
        pthread_mutex_unlock(&_smu);
        return NO;
    }
    if (_prepareErr) {
        NSError *error = _prepareErr;
        BOOL claimed = [self rdna4ClaimTerminal];
        pthread_mutex_unlock(&_smu);
        if (claimed) [self rdna4DeliverTerminalFailure:error queue:queue unresolved:NO fault:VK_ERROR_UNKNOWN];
        return NO;
    }
    if (!_prepared) {
        NSError *error = rdna4_err(55, @"command buffer reached the scheduler without preparation");
        BOOL claimed = [self rdna4ClaimTerminal];
        pthread_mutex_unlock(&_smu);
        if (claimed) [self rdna4DeliverTerminalFailure:error queue:queue unresolved:NO fault:VK_ERROR_UNKNOWN];
        return NO;
    }
    RDNA4CommandSegment *segment = nil;
    while (_nextSegment < _segments.count) {
        segment = _segments[_nextSegment];
        if (!segment->pruned) break;
        _nextSegment++;
    }
    if (!segment || _nextSegment >= _segments.count) {
        NSError *error = rdna4_err(55, @"command buffer has no submit-ready segment");
        BOOL claimed = [self rdna4ClaimTerminal];
        pthread_mutex_unlock(&_smu);
        if (claimed) [self rdna4DeliverTerminalFailure:error queue:queue unresolved:NO fault:VK_ERROR_UNKNOWN];
        return NO;
    }
    VkResult fault = (VkResult)atomic_load(&rdna4_submit_fault);
    if (fault != VK_SUCCESS) {
        NSError *error = rdna4_err(53, @"An earlier GPU submission is unresolved; restart the process before submitting again");
        BOOL claimed = [self rdna4ClaimTerminal];
        pthread_mutex_unlock(&_smu);
        if (claimed) [self rdna4DeliverTerminalFailure:error queue:queue unresolved:NO fault:fault];
        return NO;
    }
    NSError *error = nil;
    int ready = [self rdna4WaitsReady:segment error:&error];
    if (ready == 0) {
        pthread_mutex_unlock(&_smu);
        return YES;
    }
    if (ready < 0) {
        BOOL claimed = [self rdna4ClaimTerminal];
        pthread_mutex_unlock(&_smu);
        if (claimed) [self rdna4DeliverTerminalFailure:error queue:queue unresolved:NO fault:VK_ERROR_UNKNOWN];
        return NO;
    }

    if (segment->hostOnly) {
        if (segment->hostActionAccepted) {
            BOOL terminal = segment->terminal;
            BOOL terminalClaimed = terminal ? [self rdna4ClaimTerminal] : NO;
            segment->done = YES;
            _nextSegment++;
            pthread_mutex_unlock(&_smu);
            if (terminal) {
                if (terminalClaimed) [self rdna4DeliverTerminalSuccessWithQueue:queue];
            } else {
                [queue rdna4Kick];
            }
            return NO;
        }
        if (segment->hostActionRunning) {
            pthread_mutex_unlock(&_smu);
            return YES;
        }

        BOOL (^action)(NSError **) = segment->hostAction;
        if (!action) {
            NSError *failure = rdna4_err(67, @"ordered host action has no runnable block");
            segment->hostActionAccepted = YES;
            segment->done = YES;
            _nextSegment++;
            BOOL claimed = [self rdna4ClaimTerminal];
            pthread_mutex_unlock(&_smu);
            if (claimed) [self rdna4DeliverTerminalFailure:failure queue:queue unresolved:NO fault:VK_ERROR_UNKNOWN];
            return NO;
        }

        segment->hostActionRunning = YES;
        segment->hostAction = nil;
        _inFlight = YES;
        pthread_mutex_unlock(&_smu);

        NSError *actionError = nil;
        BOOL accepted = NO;
        @try {
            accepted = action(&actionError);
        } @catch (NSException *exception) {
            actionError = rdna4_err(67, [NSString stringWithFormat:@"ordered host action raised: %@",
                                         exception.reason ?: @"unknown exception"]);
        }

        pthread_mutex_lock(&_smu);
        segment->hostActionRunning = NO;
        segment->hostActionAccepted = YES;
        segment->done = YES;
        _nextSegment++;
        _inFlight = NO;
        if (!accepted) {
            NSError *failure = actionError ?: rdna4_err(67, @"ordered host action was rejected without an error");
            BOOL claimed = [self rdna4ClaimTerminal];
            pthread_mutex_unlock(&_smu);
            if (claimed) [self rdna4DeliverTerminalFailure:failure queue:queue unresolved:NO fault:VK_ERROR_UNKNOWN];
            return NO;
        }

        BOOL terminal = segment->terminal;
        BOOL terminalClaimed = terminal ? [self rdna4ClaimTerminal] : NO;
        pthread_mutex_unlock(&_smu);
        if (terminal) {
            if (terminalClaimed) [self rdna4DeliverTerminalSuccessWithQueue:queue];
        } else {
            [queue rdna4Kick];
        }
        return NO;
    }

    if (![self rdna4PrepareLinearImages:segment error:&error]) {
        BOOL claimed = [self rdna4ClaimTerminal];
        pthread_mutex_unlock(&_smu);
        if (claimed) [self rdna4DeliverTerminalFailure:error ?: rdna4_err(55, @"IOSurface initialization failed")
            queue:queue unresolved:NO fault:VK_ERROR_UNKNOWN];
        return NO;
    }

    VkFenceCreateInfo fc = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence = VK_NULL_HANDLE;
    VkResult r = vkCreateFence(RDNA4R.dev, &fc, NULL, &fence);
    if (r != VK_SUCCESS) {
        error = rdna4_err(52, [NSString stringWithFormat:@"vkCreateFence = %d", r]);
        BOOL claimed = [self rdna4ClaimTerminal];
        pthread_mutex_unlock(&_smu);
        if (claimed) [self rdna4DeliverTerminalFailure:error queue:queue unresolved:NO fault:r];
        return NO;
    }
    segment->fence = fence;
    _fence = fence;
    VkCommandBuffer commands[] = { segment->linearInit, segment->vk };
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = segment->linearInit ? 2 : 1, .pCommandBuffers = segment->linearInit ? commands : commands + 1 };
    BOOL submitAttempted = NO, nativeDone = NO;
    uint64_t argumentSubmissionEpoch = 0;
    uint64_t tLock = rdna4_now(); rdna4_qlock(); tLock = rdna4_now() - tLock;
    if (tLock > 250000000ull) RDNA4LOG("SUBMIT STALL: the queue lock was held for %.1f ms before command buffer %llu (a frame thread waits here: the display freezes)", (double)tLock / 1e6, (unsigned long long)_cbid);
    r = (VkResult)atomic_load(&rdna4_submit_fault);
    if (r == VK_SUCCESS) {
        [self rdna4DispSourceWrites:segment];
        if (_shaderFallback || (segment->terminal && !_dispSeqLogged)) {
            [self rdna4DispSeq];
            _dispSeqLogged = YES;
        }
        if (!_cbSubmitLogged) {
            [self rdna4CBSubmit:_startTime];
            _cbSubmitLogged = YES;
        }
        if (!_hasEvents && [self rdna4NativeEligible]) {
            uint64_t tIdle = rdna4_now(); r = rdna4_vkQueueWaitIdle(RDNA4R.q); tIdle = rdna4_now() - tIdle;
            if (tIdle > 250000000ull) RDNA4LOG("SUBMIT STALL: vkQueueWaitIdle took %.1f ms on the native-buffer route (command buffer %llu)", (double)tIdle / 1e6, (unsigned long long)_cbid);
            if (r == VK_SUCCESS) {
                bool fallback = false;
                int nr = [self rdna4NativeExecute:&fallback];
                nativeDone = !fallback;
                if (nr) {
                    r = nr == -ETIMEDOUT ? VK_TIMEOUT : VK_ERROR_DEVICE_LOST;
                    error = rdna4_err(53, [NSString stringWithFormat:@"native buffer operation = %d", nr]);
                }
            }
            if (r == VK_SUCCESS && nativeDone)
                argumentSubmissionEpoch = rdna4_argument_next_submission_epoch();
            if (r != VK_SUCCESS) {
                submitAttempted = YES;
                atomic_store(&rdna4_submit_fault, (int)r);
            }
        }
        if (r == VK_SUCCESS && !nativeDone) {
            submitAttempted = YES;
            r = vkQueueSubmit(RDNA4R.q, 1, &si, fence);
            if (r != VK_SUCCESS) atomic_store(&rdna4_submit_fault, (int)r);
            else {
                if (segment->linearInit)
                    for (RDNA4Texture *texture in segment->touches)
                        if ([texture rdna4IOSLinearNeedsInit]) [texture rdna4IOSLinearDidSubmit];
                argumentSubmissionEpoch = rdna4_argument_next_submission_epoch();
            }
        }
    }
    pthread_mutex_unlock(&RDNA4R.qlock);
    RDNA4LOGR("GPU segment submission result = %d (route=%s)", r, nativeDone ? "native-buffer" : "vulkan");
    if (submitAttempted || nativeDone) _submitAttempted = YES;
    if (r != VK_SUCCESS && !error) error = rdna4_err(53, [NSString stringWithFormat:@"GPU submission = %d", r]);
    if (r != VK_SUCCESS) {
        // A failed submit may have reached the GPU; only later, unattempted
        // segments can roll back their ownership while this one is quarantined.
        segment->retirementUnknown = submitAttempted;
        BOOL claimed = [self rdna4ClaimTerminal];
        pthread_mutex_unlock(&_smu);
        if (!claimed) return NO;
        [self rdna4EnsureScheduledWithError:error];
        [self rdna4DeliverTerminalFailure:error queue:queue unresolved:submitAttempted fault:r];
        return NO;
    }

    segment->submitted = YES;
    segment->argumentSubmissionEpoch = argumentSubmissionEpoch;
    // Encoding order is not submission order: attach the epoch only after the
    // native path or vkQueueSubmit has accepted this segment.
    _nextSegment++;
    _inFlight = YES;
    _readyTime = rdna4_now();
    pthread_mutex_unlock(&_smu);
    // Buffer owner publication may wait for a setter or refinement. Never do
    // that while holding the command-buffer submit lock.
    for (id intent in segment->argumentIntents)
        rdna4_argument_sampler_submit(intent, argumentSubmissionEpoch);
    [self rdna4EnsureScheduledWithError:nil];
    if (nativeDone) {
        dispatch_async([queue rdna4CompletionQueue], ^{
            BOOL terminalClaimed = NO;
            pthread_mutex_lock(&self->_smu);
            segment->done = YES;
            BOOL terminal = segment->terminal;
            if (terminal) terminalClaimed = [self rdna4ClaimTerminal];
            pthread_mutex_unlock(&self->_smu);
            [self rdna4SignalSegment:segment];
            if (terminal) {
                if (terminalClaimed) [self rdna4DeliverTerminalSuccessWithQueue:queue];
            } else {
                pthread_mutex_lock(&self->_smu);
                self->_inFlight = NO;
                pthread_mutex_unlock(&self->_smu);
                [queue rdna4Kick];
            }
        });
    } else {
        [self rdna4FenceWait:segment queue:queue];
    }
    return NO;
}

// build 18 (P2): hand a render pass its occlusion bookkeeping and RDNA4OCC_MAXQ reset-able query indices from this command buffer's pool (a pool holds 2 chunks; a new pool is made when it is used up).
// Returns the rdna4occ (owned by this command buffer; never nil once the buffer is usable). *qp == VK_NULL_HANDLE: no pool could be made, so the pass answers "visible" for every offset it selects.
- (rdna4occ *)rdna4OccNew:(RDNA4Buffer *)vb accumulate:(int)acc pool:(VkQueryPool *)qp base:(uint32_t *)base {
    rdna4occ *o = calloc(1, sizeof *o);
    if (!o) { *qp = VK_NULL_HANDLE; return NULL; }
    rdna4occ_init(o, (uint64_t)[vb length], acc);
    *qp = VK_NULL_HANDLE; *base = 0;
    if (RDNA4R.occOK) {
        if (!_qpCur || _qpUsed + RDNA4OCC_MAXQ > 2 * RDNA4OCC_MAXQ) {
            VkQueryPoolCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_OCCLUSION, .queryCount = 2 * RDNA4OCC_MAXQ };
            VkQueryPool np = VK_NULL_HANDLE; VkResult r = vkCreateQueryPool(RDNA4R.dev, &qi, NULL, &np);
            if (r == VK_SUCCESS) { if (!_qpools) _qpools = [NSMutableArray array]; [_qpools addObject:[NSValue valueWithPointer:(void *)np]]; _qpCur = np; _qpUsed = 0; }
            else { RDNA4LOG("occlusion: vkCreateQueryPool = %d: this pass answers visible", r); _qpCur = VK_NULL_HANDLE; }
        }
        if (_qpCur) { *qp = _qpCur; *base = _qpUsed; _qpUsed += RDNA4OCC_MAXQ; }
    }
    if (*qp == VK_NULL_HANDLE) rdna4occ_dead(o);
    if (_noj == _coj) { uint32_t nc = _coj ? _coj * 2 : 4; void *nn = realloc(_oj, nc * sizeof *_oj); if (!nn) { free(o); *qp = VK_NULL_HANDLE; return NULL; } _oj = nn; _coj = nc; }
    _oj[_noj].o = o; _oj[_noj].qp = *qp; _oj[_noj].base = *base; _noj++;
    if (!_ojb) _ojb = [NSMutableArray array];
    [_ojb addObject:vb]; [self rdna4Retain:vb];
    return o;
}
// The GPU is finished with this command buffer (or it never ran): write every selected offset of every visibility buffer, once. Results are read WITHOUT waiting (a lost device must not hang the completion thread):
// only when the fence reads VK_SUCCESS are the queries read, with availability; anything else answers "visible" (the fail-safe in rdna4_occ.h).
- (void)rdna4OccResolve {
    if (!_noj) return;
    BOOL ran = NO;
    if (RDNA4R.ok && RDNA4R.occOK && _fence != VK_NULL_HANDLE) ran = vkGetFenceStatus(RDNA4R.dev, _fence) == VK_SUCCESS;
    uint32_t nj = _noj; _noj = 0;
    for (uint32_t j = 0; j < nj; j++) {
        rdna4occ *o = _oj[j].o; RDNA4Buffer *vb = _ojb[j]; uint8_t *base = (uint8_t *)[vb contents];
        uint64_t res[RDNA4OCC_MAXQ]; BOOL ok = ran && _oj[j].qp != VK_NULL_HANDLE;
        if (ok && o->nq) {
            uint64_t raw[2 * RDNA4OCC_MAXQ];   // (result, availability) pairs
            VkResult r = vkGetQueryPoolResults(RDNA4R.dev, _oj[j].qp, _oj[j].base, o->nq, sizeof raw, raw, 2 * sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
            if (r != VK_SUCCESS) ok = NO;
            if (ok && !rdna4occ_read(o, raw, res)) ok = NO;   // an interval that is not available answers visible
        }
        if (!ok) RDNA4LOGR("occlusion: pass with %u offset(s) answered VISIBLE (fail-safe: fence %s, %u interval(s), overflow %d)", o->nu, ran ? "done" : "not done", o->nq, o->overflow);
        for (uint32_t u = 0; u < o->nu && base; u++) {
            uint64_t *slot = (uint64_t *)(void *)(base + o->u[u].off);
            *slot = rdna4occ_final(o, u, ok ? res : NULL, ok, *slot);
        }
        RDNA4LOGR("occlusion: resolved %u interval(s) into %u offset(s) (%s)", o->nq, o->nu, o->accumulate ? "Accumulate" : "Reset");
        free(o); _oj[j].o = NULL;
    }
    [_ojb removeAllObjects];
}
- (void)rdna4ArgumentIntent:(id)intent {
    if (!intent) { [self rdna4Fail:@"argument write intent allocation failed"]; return; }
    if (!_curSegment || _curSegment->ended) {
        rdna4_argument_sampler_abandon(intent);
        [self rdna4Fail:@"argument write intent has no open command segment"];
        return;
    }
    [_curSegment->argumentIntents addObject:intent];
    _curSegment->hasWork = YES;
}
- (void)rdna4ArgumentWrite:(id)buffer range:(NSRange)range candidates:(NSSet *)candidates fillOnly:(BOOL)fillOnly {
    if (!_curSegment || _curSegment->ended) {
        [self rdna4Fail:@"argument write intent has no open command segment"];
        return;
    }
    if (!buffer || !range.length) {
        [self rdna4Fail:@"argument write intent has an empty range"];
        return;
    }
    for (id intent in _curSegment->argumentIntents) {
        if (rdna4_argument_sampler_intent_matches(intent, buffer, range, fillOnly)) {
            rdna4_argument_sampler_add_candidates(intent, candidates);
            return;
        }
    }
    RDNA4ArgumentWriteIntent *created = rdna4_argument_sampler_intent(buffer, range, candidates,
                                                                     fillOnly);
    if (!created) {
        [self rdna4Fail:@"argument write intent range is invalid"];
        return;
    }
    [self rdna4RetainArgument:buffer];
    [self rdna4ArgumentIntent:created];
}
- (void)rdna4ArgumentWriteRanges:(id)buffer ranges:(NSArray<NSValue *> *)ranges candidates:(NSSet *)candidates {
    if (!_curSegment || _curSegment->ended) {
        [self rdna4Fail:@"argument write intent has no open command segment"];
        return;
    }
    if (!buffer || ![ranges isKindOfClass:[NSArray class]] || !ranges.count) {
        [self rdna4Fail:@"argument write intent has no ranges"];
        return;
    }
    NSMutableArray<NSValue *> *normalized = [NSMutableArray arrayWithCapacity:ranges.count];
    for (id value in ranges) {
        if (![value isKindOfClass:[NSValue class]]) {
            [self rdna4Fail:@"argument write intent has a non-range value"];
            return;
        }
        NSRange range = [(NSValue *)value rangeValue];
        if (!range.length || range.location > [buffer length] ||
            range.length > [buffer length] - range.location) {
            [self rdna4Fail:@"argument write intent range is invalid"];
            return;
        }
        [normalized addObject:[NSValue valueWithRange:range]];
    }
    [normalized sortUsingComparator:^NSComparisonResult(NSValue *left, NSValue *right) {
        NSRange a = left.rangeValue, b = right.rangeValue;
        if (a.location != b.location)
            return a.location < b.location ? NSOrderedAscending : NSOrderedDescending;
        return a.length < b.length ? NSOrderedAscending :
            a.length > b.length ? NSOrderedDescending : NSOrderedSame;
    }];
    NSMutableArray<NSValue *> *merged = [NSMutableArray arrayWithCapacity:normalized.count];
    for (NSValue *value in normalized) {
        NSRange next = value.rangeValue;
        if (merged.count && next.location <= NSMaxRange(merged.lastObject.rangeValue)) {
            NSRange current = merged.lastObject.rangeValue;
            NSUInteger end = MAX(NSMaxRange(current), NSMaxRange(next));
            merged[merged.count - 1] =
                [NSValue valueWithRange:NSMakeRange(current.location,
                                                    end - current.location)];
        } else {
            [merged addObject:value];
        }
    }
    NSRange first = merged.firstObject.rangeValue;
    NSRange last = merged.lastObject.rangeValue;
    NSRange envelope = NSMakeRange(first.location, NSMaxRange(last) - first.location);
    for (id intent in _curSegment->argumentIntents) {
        if (rdna4_argument_sampler_intent_matches_ranges(
                intent, buffer, envelope, merged, NO)) {
            rdna4_argument_sampler_add_candidates(intent, candidates);
            return;
        }
    }
    RDNA4ArgumentWriteIntent *created = rdna4_argument_sampler_intent_ranges(
        buffer, envelope, merged, candidates, NO);
    if (!created) {
        [self rdna4Fail:@"argument write intent ranges are invalid"];
        return;
    }
    [self rdna4RetainArgument:buffer];
    [self rdna4ArgumentIntent:created];
}
- (void)rdna4CancelUnsubmittedArgumentIntents {
    for (RDNA4CommandSegment *segment in _segments) {
        if (segment->submitted || segment->retirementUnknown) continue;
        for (id intent in segment->argumentIntents) rdna4_argument_sampler_abandon(intent);
        [segment->argumentIntents removeAllObjects];
        segment->argumentSummaryAttempts = nil;
        [segment->argumentPools removeAllObjects];
        [segment->argumentRefs removeAllObjects];
        [segment->commandRefs removeAllObjects];
        [self rdna4ReleaseSegmentDescriptorPools:segment];
    }
}
- (void)rdna4PoolDone {
    [self rdna4UCRelease]; [self rdna4OccResolve]; if (_pser) rdna4_fence_close(_pser);
    [_refs removeAllObjects];
    [_residencySnapshots removeAllObjects];
    [_residencyHeapResources removeAllObjects];
    _ring = nil;
    _argumentTable = nil;
    _ringUsed = 0;
    _argumentTableBytes = 0;
    _pool = VK_NULL_HANDLE;
    for (RDNA4CommandSegment *segment in _segments)
        [self rdna4ReleaseSegmentDescriptorPools:segment];
}
// Copies bytes into the command buffer's upload ring (Shared memory, 256-byte aligned: covers every storage/uniform offset alignment).
- (BOOL)rdna4Bytes:(const void *)p length:(NSUInteger)n buffer:(RDNA4Buffer * __strong *)ob offset:(NSUInteger *)oo {
    if ((n && !p) || n > NSUIntegerMax - 255) {
        [self rdna4Fail:@"upload byte count or source is invalid"]; return NO;
    }
    NSUInteger need = ((n ? n : 1) + 255) & ~(NSUInteger)255;
    if (!_ring || _ringUsed > [_ring length] || need > [_ring length] - _ringUsed) {
        NSError *e = nil; NSUInteger sz = need > 262144 ? need : 262144;
        RDNA4Buffer *nb = [[RDNA4Buffer alloc] initWithDevice:[_nq device] length:sz options:MTLResourceStorageModeShared | (RDNA4P.on ? RDNA4_OPT_NOZERO : 0) error:&e];   // P1: slab range / recycled, not a kernel BO per command buffer; every byte is written before use
        if (!nb) { [self rdna4Fail:[NSString stringWithFormat:@"upload ring: %@", e.localizedDescription]]; return NO; }
        _ring = nb; _ringUsed = 0; [self rdna4RetainArgument:nb];
    }
    memcpy((uint8_t *)[_ring contents] + _ringUsed, p, n);
    *ob = _ring; *oo = _ringUsed; _ringUsed += need;
    return YES;
}
- (VkDescriptorSet)rdna4AllocSet:(VkDescriptorSetLayout)l {
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!_pool) {
            VkDescriptorPool reuse_ = rdna4_dp_take();   // P1: a fence-cleared, reset pool of an earlier command buffer (VK_NULL_HANDLE when OFF)
            if (reuse_) _pool = reuse_;
            else {
                // A 256-set pool with room for 1024 storage buffers is RADV memory proportional to
                // maxSets x descriptors. One such pool per command buffer can fail under memory
                // pressure (login: many pipelines + imports at once) and a failed pool fails the
                // whole command buffer, i.e. a frame is never drawn. Degrade the bound instead:
                // the numbers are upper bounds, rdna4AllocSet already replaces an exhausted pool.
                static const uint32_t sets[3] = { 256, 64, 16 }, div[3] = { 1, 4, 16 };
                VkResult r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
                for (int tier = 0; tier < 3; tier++) {
                    VkDescriptorPoolSize sz[9] = {
                        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1024 / div[tier] }, { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 256 / div[tier] },
                        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1024 / div[tier] }, { VK_DESCRIPTOR_TYPE_SAMPLER, 512 / div[tier] },
                        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 256 / div[tier] }, { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 256 / div[tier] },
                        { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1024 / div[tier] }, { VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 256 / div[tier] },
                        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 256 / div[tier] } };
                    VkDescriptorPoolCreateInfo pc = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = sets[tier],
                                                      .poolSizeCount = sizeof sz / sizeof *sz, .pPoolSizes = sz };
                    r = vkCreateDescriptorPool(RDNA4R.dev, &pc, NULL, &_pool);
                    if (r == VK_SUCCESS) break;
                    _pool = VK_NULL_HANDLE;
                    if (r != VK_ERROR_OUT_OF_DEVICE_MEMORY && r != VK_ERROR_OUT_OF_HOST_MEMORY) break;
                }
                if (r != VK_SUCCESS) { [self rdna4Fail:[NSString stringWithFormat:@"vkCreateDescriptorPool = %d (all three pool sizes refused)", r]]; return VK_NULL_HANDLE; }
                rdna4_dp_created();
            }
            if (!_curSegment || _curSegment->ended) {
                if (!(RDNA4P.on && rdna4_dp_give(_pool)))
                    vkDestroyDescriptorPool(RDNA4R.dev, _pool, NULL);
                _pool = VK_NULL_HANDLE;
                [self rdna4Fail:@"descriptor pool has no open command segment"];
                return VK_NULL_HANDLE;
            }
            [_curSegment->descriptorPools
                addObject:[NSValue valueWithPointer:(void *)_pool]];
        }
        VkDescriptorSetAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = _pool, .descriptorSetCount = 1, .pSetLayouts = &l };
        VkDescriptorSet s = VK_NULL_HANDLE; VkResult r = vkAllocateDescriptorSets(RDNA4R.dev, &ai, &s);
        if (r == VK_SUCCESS) return s;
        _pool = VK_NULL_HANDLE;   // exhausted or fragmented: the next attempt makes a new pool
    }
    [self rdna4Fail:@"vkAllocateDescriptorSets failed twice (descriptor pool)"];
    return VK_NULL_HANDLE;
}
#include "rdna4_argument_table_commands.inc"
// Writes the descriptors of one stage's set from the encoder's bound state (see the binding-model comment).
- (BOOL)rdna4FillSet:(VkDescriptorSet)set bindings:(const RDNA4PB *)pb count:(uint32_t)n state:(const RDNA4StageState *)st stage:(const char *)nm {
    enum { MAXW = 256, MAXI = 1024 };
    if (n > MAXW) { [self rdna4Fail:[NSString stringWithFormat:@"%s stage has %u descriptors (> %d)", nm, n, MAXW]]; return NO; }
    VkWriteDescriptorSet w[MAXW]; VkDescriptorBufferInfo bi[MAXI]; VkDescriptorImageInfo ii[MAXI];
    VkBufferView texelViews[MAXI]; uint32_t nbi = 0, nii = 0, ntv = 0; id dev = [self device];
    for (uint32_t i = 0; i < n; i++) {
        const RDNA4PB *p = &pb[i];
        if (p->count > MAXI - nbi || p->count > MAXI - nii || p->count > MAXI - ntv) { [self rdna4Fail:[NSString stringWithFormat:@"%s binding %u: too many array descriptors", nm, p->binding]]; return NO; }
        w[i] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = p->binding, .descriptorCount = p->count, .descriptorType = p->type };
        switch (p->type) {
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: {
            if (p->samplerCoordinateFlags) {
                uint32_t flags[96] = {0};
                BOOL assigned[32] = {NO};
                for (uint32_t q = 0; q < n; ++q) {
                    const RDNA4PB *source = &pb[q];
                    if (source->type != VK_DESCRIPTOR_TYPE_SAMPLER) continue;
                    for (uint32_t k = 0; k < source->count; ++k) {
                        uint64_t logical = 0;
                        if (!rdna4_coordinate_band_index(source, k, &logical)) {
                            [self rdna4Fail:[NSString stringWithFormat:
                                @"%s sampler binding %u count %u crosses the 32-slot coordinate band",
                                nm, source->binding, source->count]];
                            return NO;
                        }
                        BOOL pixel = NO;
                        if (source->stat) {
                            pixel = source->pixelCoordinates;
                        } else {
                            id sampler = source->embedded
                                ? rdna4_embedded_resource(source, k, st, NO, dev)
                                : (logical < 16 ? st->smp[logical] : nil);
                            if (!sampler) {
                                if (source->embedded) {
                                    [self rdna4Fail:[NSString stringWithFormat:@"%s sampler(%llu): embedded argument-buffer token is missing, invalid, or was released before encoding", nm, (unsigned long long)logical]];
                                    return NO;
                                }
                                continue;
                            }
                            pixel = rdna4_sampler_object_pixel(sampler);
                            [self rdna4RetainArgument:sampler];
                        }
                        RDNA4CoordinateFlagResult merged =
                            rdna4_coordinate_flag_merge(flags, assigned, logical,
                                                        pixel);
                        if (merged != RDNA4CoordinateFlagOK) {
                            [self rdna4Fail:[NSString stringWithFormat:
                                @"%s sampler coordinate %s at logical index %llu",
                                nm,
                                merged == RDNA4CoordinateFlagConflict
                                    ? "sources disagree" : "index exceeds 32 slots",
                                (unsigned long long)logical]];
                            return NO;
                        }
                    }
                }
                RDNA4Buffer *table = nil; NSUInteger offset = 0;
                if (p->stageResourceMetadata) {
                    for (unsigned index = 32; index < 96; ++index) flags[index] = UINT32_MAX;
                    for (unsigned index = 0; index < p->nullShapeCount; ++index) {
                        unsigned access = p->nullShapeAccess[index];
                        uint32_t ordinal = p->nullShapeRemap[index];
                        if (!p->stageTokenTable && rdna4ns_access_is_image(access)) {
                            uint32_t base = _argumentTokenCounts[access == RDNA4NS_ACCESS_STORAGE];
                            if (ordinal > UINT32_MAX - base) {
                                [self rdna4Fail:@"null-shape descriptor index overflow"];
                                return NO;
                            }
                            ordinal += base;
                        } else if (p->stageTokenTable &&
                                   rdna4ns_access_is_texel(access)) {
                            unsigned category = access == RDNA4NS_ACCESS_STORAGE_TEXEL
                                ? 4u : 3u;
                            uint32_t base = _argumentTokenCounts[category];
                            if (base && rdna4ns_texel_null_index(
                                    base, ordinal, &ordinal) != RDNA4NS_OK) {
                                [self rdna4Fail:@"null texel descriptor index overflow"];
                                return NO;
                            }
                        }
                        flags[32 + index] = ordinal;
                    }
                }
                NSUInteger bytes = p->stageResourceMetadata ? sizeof flags : 32 * sizeof *flags;
                if (![self rdna4Bytes:flags length:bytes buffer:&table offset:&offset]) return NO;
                w[i].pBufferInfo = &bi[nbi];
                bi[nbi++] = (VkDescriptorBufferInfo){[table vkBuffer],
                    [table rdna4VkOffset] + offset, bytes};
                break;
            }
            if (p->argumentTable) {
                if (!_argumentTable) { [self rdna4Fail:@"GPU argument table was not prepared"]; return NO; }
                w[i].pBufferInfo = &bi[nbi];
                bi[nbi++] = (VkDescriptorBufferInfo){[_argumentTable vkBuffer], [_argumentTable rdna4VkOffset] + _argumentTableOffset, _argumentTableBytes};
                break;
            }
            if (p->addressTable) {
                rdna4_buffer_table addresses = {0};
                for (uint32_t j = 0; j < 31; ++j) {
                    RDNA4BufSlot slot = st->buf[j];
                    if (!slot.b) continue;
                    uint64_t base = [slot.b gpuAddress];
                    if ([slot.b device] != dev || !rdna4_buffer_table_set(&addresses, j, base, slot.b.length, slot.off, [slot.b rdna4HostReadOnly])) {
                        [self rdna4Fail:@"buffer-address table contains an invalid bound resource"];
                        return NO;
                    }
                    [self rdna4RetainEncoded:slot.b];
                }
                RDNA4Buffer *table = nil; NSUInteger offset = 0;
                size_t tableBytes = p->addressTableVersion == RDNA4_BUFFER_TABLE_VERSION ? sizeof addresses : sizeof addresses.address;
                NSData *registry = nil;
                if (p->addressTableVersion == RDNA4_BUFFER_REGISTRY_VERSION) {
                    [self rdna4AttachQueueResidencySnapshots];
                    NSMutableSet *resources = [_bufferAddressResources mutableCopy] ?: [NSMutableSet set];
                    for (uint32_t j = 0; j < 31; ++j)
                        if (st->buf[j].b) [resources addObject:st->buf[j].b];
                    for (id snap in _residencySnapshots) {
                        if ([snap respondsToSelector:@selector(rdna4AddBuffersToSet:device:)])
                            [(id<RDNA4ResidencySnapshotInternal>)snap rdna4AddBuffersToSet:resources device:dev];
                    }
                    for (id obj in _residencyHeapResources) {
                        if ([obj isKindOfClass:[RDNA4Buffer class]] && [(RDNA4Buffer *)obj device] == dev &&
                            ![obj isAliasable])
                            [resources addObject:obj];
                    }
                    if (!rdna4_buffer_registry_size(resources.count,
                            RDNA4R.lim.maxStorageBufferRange, &tableBytes)) {
                        [self rdna4Fail:@"buffer-address registry exceeds the backend buffer range or host allocation extent"];
                        return NO;
                    }
                    void *storage = malloc(tableBytes);
                    if (!storage) { [self rdna4Fail:@"cannot allocate buffer-address registry"]; return NO; }
                    BOOL valid = rdna4_buffer_registry_init(storage, tableBytes, &addresses,
                        resources.count, RDNA4R.lim.maxStorageBufferRange);
                    uint64_t row = 0;
                    for (RDNA4Buffer *buffer in resources) {
                        if (!valid) break;
                        valid = [buffer device] == dev && ![buffer isAliasable] &&
                            rdna4_buffer_registry_set(storage, tableBytes, row++,
                                [buffer gpuAddress], buffer.length, [buffer rdna4HostReadOnly]);
                        [self rdna4RetainArgument:buffer];
                    }
                    if (!valid) {
                        free(storage);
                        [self rdna4Fail:@"buffer-address registry contains an invalid resource or overflowing range"];
                        return NO;
                    }
                    registry = [NSData dataWithBytesNoCopy:storage length:tableBytes freeWhenDone:YES];
                }
                if (![self rdna4Bytes:registry ? registry.bytes : &addresses length:tableBytes buffer:&table offset:&offset]) return NO;
                w[i].pBufferInfo = &bi[nbi];
                bi[nbi++] = (VkDescriptorBufferInfo){ [table vkBuffer], [table rdna4VkOffset] + offset, tableBytes };
                break;
            }
            if (p->binding >= 32 && !p->argumentRoot) { [self rdna4Fail:[NSString stringWithFormat:@"%s binding %u: buffer descriptor outside the [[buffer(n)]] band 0..31 (synthetic address table? not supported)", nm, p->binding]]; return NO; }
            w[i].pBufferInfo = &bi[nbi];
            NSUInteger align = p->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ? RDNA4R.lim.minUniformBufferOffsetAlignment : RDNA4R.lim.minStorageBufferOffsetAlignment;
            for (uint32_t k = 0; k < p->count; k++) {
                uint32_t ix = (p->argumentRoot ? p->embeddedBuffer : p->binding) + k; RDNA4BufSlot s = ix < 31 ? st->buf[ix] : (RDNA4BufSlot){ nil, 0 }; RDNA4Buffer *b = s.b; NSUInteger off = s.off;
                if (!b) { RDNA4LOGR("WARN %s buffer(%u) is used by the shader but was never bound: zero-filled dummy", nm, ix); b = rdna4_dummy_buffer(dev); off = 0; }
                if (!b) { [self rdna4Fail:@"no dummy buffer"]; return NO; }
                uint64_t vkoff, viewSize;
                uint64_t viewLimit = p->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ? RDNA4R.lim.maxUniformBufferRange : RDNA4R.lim.maxStorageBufferRange;
                if ([b device] != dev || !rdna4_buffer_view(b.length, off, [b rdna4VkOffset], align, viewLimit, &vkoff, &viewSize)) {
                    [self rdna4Fail:[NSString stringWithFormat:@"%s buffer(%u) descriptor range or alignment is invalid", nm, ix]]; return NO;
                }
                [self rdna4RetainEncoded:b];
                bi[nbi++] = (VkDescriptorBufferInfo){ [b vkBuffer], vkoff, viewSize };
            }
            break; }
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: {
            BOOL writable = p->type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
            uint32_t base = writable ? 480 : 32;
            if (p->binding < base || p->binding >= base + 128) {
                [self rdna4Fail:@"texel descriptor outside the texture binding band"]; return NO;
            }
            w[i].pTexelBufferView = &texelViews[ntv];
            for (uint32_t k = 0; k < p->count; ++k) {
                uint32_t index = p->binding - base + k;
                RDNA4Texture *t = p->embedded ? rdna4_embedded_resource(p, k, st, YES, dev) :
                    (index < 128 ? st->tex[index] : nil);
                if (![t isKindOfClass:[RDNA4Texture class]] || [t device] != dev ||
                    ![t rdna4IsTexel] || ![t rdna4TexelView] || [t isAliasable] ||
                    (writable && ![t rdna4TexelWritable])) {
                    [self rdna4Fail:@"texel descriptor needs a live compatible texture buffer"]; return NO;
                }
                unsigned component = [t rdna4IsUInt] ? RDNA4NS_COMP_UINT :
                    [t rdna4IsSInt] ? RDNA4NS_COMP_SINT : RDNA4NS_COMP_FLOAT;
                if (p->texelComponentKinds != (1u << component)) {
                    [self rdna4Fail:@"texture-buffer numeric class disagrees with the shader"]; return NO;
                }
                [self rdna4RetainEncoded:t];
                texelViews[ntv++] = [t rdna4TexelView];
            }
            break;
        }
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            if (p->binding < 32 || p->binding >= 160) { [self rdna4Fail:[NSString stringWithFormat:@"%s binding %u: sampled image outside the texture band 32..159", nm, p->binding]]; return NO; }
            w[i].pImageInfo = &ii[nii];
            for (uint32_t k = 0; k < p->count; k++) {
                uint32_t ix = p->binding - 32 + k;
                RDNA4Texture *t = p->embedded ? (RDNA4Texture *)rdna4_embedded_resource(p, k, st, YES, dev) : (ix < 128 ? st->tex[ix] : nil);
                if (!t && p->embedded) { [self rdna4Fail:[NSString stringWithFormat:@"%s texture(%u): embedded argument-buffer token is missing, invalid, or was released before encoding", nm, ix]]; return NO; }
                if (!t) { RDNA4LOGR("WARN %s texture(%u) is used by the shader but was never bound: 1x1 dummy", nm, ix); t = rdna4_dummy_texture(dev); }
                if (!t) { [self rdna4Fail:@"no dummy texture"]; return NO; }
                if ([t rdna4IsTexel]) { [self rdna4Fail:@"sampled image binding cannot use a texture buffer"]; return NO; }
                unsigned sampleKind = [t rdna4Samples] > 1 ? 2u : 1u;
                unsigned expectedKinds = p->imageSampleKinds ? p->imageSampleKinds : 1u;
                if (expectedKinds != sampleKind) {
                    [self rdna4Fail:[NSString stringWithFormat:
                        @"%s texture(%u): %lu samples do not match the shader image's multisample type", nm, ix, (unsigned long)[t rdna4Samples]]];
                    return NO;
                }
                [self rdna4RetainEncoded:t];
                ii[nii++] = (VkDescriptorImageInfo){ VK_NULL_HANDLE, [t vkView],
                    rdna4_use_layout(t, p->gpuArguments ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) };
            }
            break;
        case VK_DESCRIPTOR_TYPE_SAMPLER:
            if (!p->comparisonSampler && (p->binding < 160 || p->binding >= 192)) { [self rdna4Fail:[NSString stringWithFormat:@"%s binding %u: sampler outside the normal/comparison bands", nm, p->binding]]; return NO; }
            w[i].pImageInfo = &ii[nii];
            for (uint32_t k = 0; k < p->count; k++) {
                VkSampler vs = p->stat;
                if (!vs) {
                    uint32_t ix = p->binding - (p->comparisonSampler ? 736 : 160) + k;
                    id s = p->embedded ? rdna4_embedded_resource(p, k, st, NO, dev) : (ix < 16 ? st->smp[ix] : nil);
                    if (!s && p->embedded) { [self rdna4Fail:[NSString stringWithFormat:@"%s sampler(%u): embedded argument-buffer token is missing, invalid, or was released before encoding", nm, ix]]; return NO; }
                    if (!s) { RDNA4LOGR("WARN %s sampler(%u) is used by the shader but was never bound: default nearest/clamp sampler", nm, ix); s = rdna4_dummy_sampler(dev); }
                    if (!s) { [self rdna4Fail:@"no dummy sampler"]; return NO; }
                    NSError *error = nil;
                    vs = rdna4_sampler_object_handle(s, p->comparisonSampler,
                        p->normalizedCoordinates, &error);
                    if (!vs) { [self rdna4Fail:error.localizedDescription ?: @"sampler representation unavailable"]; return NO; }
                    [self rdna4RetainArgument:s];
                }
                ii[nii++] = (VkDescriptorImageInfo){ vs, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
            }
            break;
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            if (p->binding < 480 || p->binding >= 608) { [self rdna4Fail:[NSString stringWithFormat:@"%s binding %u: storage image outside the band 480..607", nm, p->binding]]; return NO; }
            w[i].pImageInfo = &ii[nii];
            for (uint32_t k = 0; k < p->count; k++) {
                uint32_t ix = p->binding - 480 + k;
                RDNA4Texture *t = p->embedded ? (RDNA4Texture *)rdna4_embedded_resource(p, k, st, YES, dev) : (ix < 128 ? st->tex[ix] : nil);
                if (!t) { [self rdna4Fail:[NSString stringWithFormat:@"%s: writable texture(%u) is used by the shader but was never bound", nm, ix]]; return NO; }
                if ([t rdna4IsTexel]) { [self rdna4Fail:@"storage image binding cannot use a texture buffer"]; return NO; }
                [self rdna4RetainEncoded:t];
                ii[nii++] = (VkDescriptorImageInfo){ VK_NULL_HANDLE, [t rdna4StorageView], VK_IMAGE_LAYOUT_GENERAL };   // storage images: identity-swizzle full-layer view
            }
            break;
        case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:   // 11e-2: [[color(n)]] = binding 192+n = the encoder's colour attachment n, read in place (GENERAL)
            if (p->binding < 192 || p->binding >= 200) { [self rdna4Fail:[NSString stringWithFormat:@"%s binding %u: input attachment outside the colour band 192..199", nm, p->binding]]; return NO; }
            w[i].pImageInfo = &ii[nii];
            for (uint32_t k = 0; k < p->count; k++) {
                uint32_t ix = p->binding - 192 + k; RDNA4Texture *t = ix < 8 ? st->att[ix] : nil;
                if (!t) { [self rdna4Fail:[NSString stringWithFormat:@"%s: [[color(%u)]] is read by the shader but the pass has no colour attachment %u", nm, ix, ix]]; return NO; }
                [self rdna4RetainEncoded:t];
                ii[nii++] = (VkDescriptorImageInfo){ VK_NULL_HANDLE, [t rdna4AttViewLevel:0], VK_IMAGE_LAYOUT_GENERAL };
            }
            break;
        default: [self rdna4Fail:[NSString stringWithFormat:@"%s binding %u: descriptor type %d not supported (texel buffer)", nm, p->binding, p->type]]; return NO;
        }
    }
    vkUpdateDescriptorSets(RDNA4R.dev, n, w, 0, NULL);
    return YES;
}
@end
@interface RDNA4CommandBuffer (Encoders)
- (id)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)d;
- (id)blitCommandEncoder;
- (id)computeCommandEncoder;
- (id)resourceStateCommandEncoder;
- (id)resourceStateCommandEncoderWithDescriptor:(id)d;
- (id)accelerationStructureCommandEncoder;
- (id)accelerationStructureCommandEncoderWithDescriptor:(id)d;
@end

