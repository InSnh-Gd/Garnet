// A display write can finish encoding while every scanout slot is still busy.
// Keep only the newest completed source per display and copy it after a slot becomes reusable.
@interface RDNA4LateFrame : NSObject {
@public
    RDNA4Texture *texture;
    IOSurfaceRef surface;
    uint64_t seq, commit;
    uint32_t sid;
    int inst, cls;
    unsigned tries;
}
@end
@implementation RDNA4LateFrame
- (void)dealloc { if (surface) { IOSurfaceDecrementUseCount(surface); CFRelease(surface); } }
@end

static RDNA4LateFrame *rdna4late_pending[3];   // present queue only
static void rdna4s_late_run(RDNA4LateFrame *frame) {
    if (rdna4late_pending[frame->inst] != frame) return;
    pthread_mutex_lock(&RDNA4S.mu);
    rdna4df_t *sm = frame->inst ? &RDNA4XI(frame->inst).sm : &RDNA4S.sm;
    const BOOL current = rdna4df_late_current(sm, frame->seq) &&
        !rdna4dc_commit_newer(RDNA4S.commits, frame->sid, frame->commit);
    pthread_mutex_unlock(&RDNA4S.mu);
    if (!current) { rdna4late_pending[frame->inst] = nil; return; }

    rdna4df_plan_t plan;
    // A deferred source contains the complete current surface; never chain it onto an older slot.
    int slot = rdna4s_plan(NULL, frame->inst, 1, &plan);
    if (slot < 0) {
        if (++frame->tries < RDNA4DF_RETRY_MAX_ATTEMPTS) {
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 16 * NSEC_PER_MSEC), rdna4s_present_queue(), ^{ rdna4s_late_run(frame); });
        } else {
            rdna4cbl_skip(frame->sid, frame->seq, -1, "late-no-slot", 0);
            rdna4late_pending[frame->inst] = nil;
        }
        return;
    }
    rdna4late_pending[frame->inst] = nil;
    pthread_mutex_lock(&RDNA4S.mu);
    const rdna4x_desc_t *xd = rdna4x_desc((uint32_t)frame->inst);
    const uint32_t w = xd ? xd->w : RDNA4S.pw, h = xd ? xd->h : RDNA4S.ph, pitch = xd ? xd->pitch_bytes : RDNA4S.pitch;
    const BOOL valid = rdna4df_late_current(sm, frame->seq) &&
        !rdna4dc_commit_newer(RDNA4S.commits, frame->sid, frame->commit) &&
        [frame->texture width] == w && [frame->texture height] == h &&
        [frame->texture bytesPerPixel] == 4 && [frame->texture rdna4IOSBytesPerRow] == pitch &&
        [frame->texture rdna4DispBuf] != VK_NULL_HANDLE;
    if (valid) {
        rdna4df_submit(sm, slot, frame->sid, frame->seq);
        sm->sub_commit[slot] = frame->commit;
        sm->deferred_copy[slot] = 1;
    } else rdna4df_skip_content(sm, slot);
    pthread_mutex_unlock(&RDNA4S.mu);
    if (!valid) return;

    NSError *error = nil; BOOL unresolved = NO;
    BOOL ok = rdna4_oneshot(&error, ^(VkCommandBuffer cmd) {
        // Capturing frame also retains the source/use-count if the GPU submission is quarantined.
        rdna4_full_barrier(cmd);
        rdna4s_record_copy(cmd, [frame->texture rdna4DispBuf], [frame->texture rdna4IOSOffset], &plan);
        rdna4_full_barrier(cmd);
    }, &unresolved);
    if (unresolved) {
        pthread_mutex_lock(&RDNA4S.mu);
        if (rdna4df_fail(sm, RDNA4DF_R_ERROR)) {
            if (frame->inst) rdna4x_do_release_locked(frame->inst); else rdna4s_do_release_locked();
        }
        pthread_mutex_unlock(&RDNA4S.mu);
        RDNA4LOG("scanout: deferred copy unresolved: %s", error.localizedDescription.UTF8String);
        return;   // The slot and source stay retained; never reuse memory the GPU may still write.
    }
    rdna4s_complete_run(frame->inst, slot, ok ? VK_SUCCESS : VK_ERROR_UNKNOWN, frame->seq, frame->sid, NULL, frame->cls, frame->commit, 0);
}

static void rdna4s_late_complete(RDNA4Texture *texture, int inst, uint64_t seq, uint32_t sid, int cls, uint64_t commit) {
    if (!texture || inst < 0 || inst > 2 || !seq || !sid) return;
    RDNA4LateFrame *frame = [RDNA4LateFrame new];
    frame->texture = texture; frame->inst = inst; frame->seq = seq; frame->sid = sid; frame->cls = cls; frame->commit = commit;
    IOSurfaceRef surface = [texture rdna4UCSurface];
    if (surface) { frame->surface = (IOSurfaceRef)CFRetain(surface); IOSurfaceIncrementUseCount(surface); }
    dispatch_async(rdna4s_present_queue(), ^{
        RDNA4LateFrame *old = rdna4late_pending[inst];
        if (old && old->seq > seq) return;
        rdna4late_pending[inst] = frame;
        rdna4s_late_run(frame);
    });
}
