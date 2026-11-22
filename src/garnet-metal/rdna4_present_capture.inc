#ifndef RDNA4_PRESENT_CAPTURE_FLAG
#define RDNA4_PRESENT_CAPTURE_FLAG "/private/tmp/rdna4m-present-capture"
#endif
#ifndef RDNA4_PRESENT_FULL_FLAG
#define RDNA4_PRESENT_FULL_FLAG "/private/tmp/rdna4m-present-full"
#endif
static BOOL rdna4_capture_scan_matches(const struct rdna4n_scan_status *status, int slot, uint64_t presentId) {
    return status->acquired && !status->flags && status->presents == presentId &&
        status->front_slot == (uint32_t)slot &&
        status->pending_slot == RDNA4N_SCAN_NO_SLOT && status->slot[slot].mc &&
        status->earliest_mc == status->slot[slot].mc && status->plane_mc == status->earliest_mc;
}
static BOOL rdna4_present_capture_on(void) {
    static _Atomic uint64_t next;
    static _Atomic int enabled;
    uint64_t now = rdna4_now(), due = atomic_load(&next);
    if (now >= due && atomic_compare_exchange_strong(&next, &due, now + NSEC_PER_SEC)) {
        struct stat st;
        atomic_store(&enabled, lstat(RDNA4_PRESENT_CAPTURE_FLAG, &st) == 0 && S_ISREG(st.st_mode) &&
            (st.st_uid == 0 || st.st_uid == geteuid()));
    }
    return atomic_load(&enabled) != 0;
}
static BOOL rdna4_capture_scan_status(struct rdna4n_scan_status *status) {
    pthread_mutex_lock(&RDNA4S.mu);
    const BOOL ready = RDNA4S.fs && RDNA4S.sm.state == RDNA4DF_ACTIVE && !RDNA4S.fs(RDNA4R.dev, status);
    pthread_mutex_unlock(&RDNA4S.mu);
    return ready;
}

static const uint8_t *rdna4_capture_source_plane(IOSurfaceRef source, uint32_t width,
                                                  uint32_t height, size_t pitch) {
    if (!source || IOSurfaceGetPixelFormat(source) != RDNA4S_FOURCC_BGRA) return NULL;
    const size_t planes = IOSurfaceGetPlaneCount(source);
    if (planes > 1) return NULL;
    const size_t planeWidth = planes ? IOSurfaceGetWidthOfPlane(source, 0) : IOSurfaceGetWidth(source);
    const size_t planeHeight = planes ? IOSurfaceGetHeightOfPlane(source, 0) : IOSurfaceGetHeight(source);
    const size_t element = planes ? IOSurfaceGetBytesPerElementOfPlane(source, 0) : IOSurfaceGetBytesPerElement(source);
    const size_t rowPitch = planes ? IOSurfaceGetBytesPerRowOfPlane(source, 0) : IOSurfaceGetBytesPerRow(source);
    if (planeWidth != width || planeHeight != height || element != 4 || rowPitch != pitch) return NULL;
    const uint8_t *allocation = IOSurfaceGetBaseAddress(source);
    const uint8_t *plane = planes ? IOSurfaceGetBaseAddressOfPlane(source, 0) : allocation;
    if (!allocation || (uintptr_t)plane < (uintptr_t)allocation) return NULL;
    const rdna4pl_in geometry = { .pbpe = element, .pbpr = pitch, .ph = height,
        .poff = (uintptr_t)plane - (uintptr_t)allocation, .alloc = IOSurfaceGetAllocSize(source) };
    rdna4pl_out bounds;
    return rdna4pl_bound(&geometry, &bounds) ? plane : NULL;
}

static void rdna4_capture_present(int inst, int slot, uint32_t sid, uint64_t seq, int rc, uint64_t presentId) {
    if (rc || inst || slot < 0 || slot >= RDNA4DF_MAX_SLOTS || !rdna4_is_ws() || !rdna4_present_capture_on()) return;
    static _Atomic uint64_t next;
    static _Atomic int writing;
    const uint64_t now = rdna4_now();
    if (now < atomic_load(&next) || atomic_exchange(&writing, 1)) return;
    struct stat flagStat;
    const BOOL full = lstat(RDNA4_PRESENT_FULL_FLAG, &flagStat) == 0 && S_ISREG(flagStat.st_mode) &&
        (flagStat.st_uid == 0 || flagStat.st_uid == geteuid());
    atomic_store(&next, now + (full ? 5 * NSEC_PER_SEC : NSEC_PER_SEC / 2));
    const uint32_t width = RDNA4S.pw, height = RDNA4S.ph, kernelSlot = RDNA4S.sid[slot];
    const VkBuffer slotBuffer = RDNA4S.buf[slot];
    const size_t pitch = RDNA4S.pitch, row = (size_t)width * 4;
    const size_t budget = (full ? 64u : 3u) * 1024u * 1024u;
    const size_t copies = full ? 1u : 3u;
    if (!slotBuffer || kernelSlot >= RDNA4N_SCAN_MAX_SLOTS || !row || pitch < row || row > budget / copies) {
        atomic_store(&writing, 0);
        return;
    }
    size_t rows = full ? height : MIN(height, 64u);
    const size_t maxRows = MIN(budget / (copies * row), 1 + (budget - row) / pitch);
    if (!rows || (full && rows > maxRows)) { atomic_store(&writing, 0); return; }
    rows = MIN(rows, maxRows);
    RDNA4S.sm.pin[slot]++;
    static dispatch_queue_t writer;
    static NSMutableArray *saved;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        writer = dispatch_queue_create("org.garnet.present-capture", DISPATCH_QUEUE_SERIAL);
        saved = [NSMutableArray array];
    });
    dispatch_async(writer, ^{
        @autoreleasepool {
            const uint64_t start = rdna4_now();
            char path[1024];
            const BOOL directoryReady = rdna4_dump_dir_ready(path, sizeof path);
            IOSurfaceRef source = directoryReady && sid ? IOSurfaceLookup(sid) : NULL;
            const uint8_t *base = rdna4_capture_source_plane(source, width, height, pitch);
            const BOOL sourceValid = base != NULL;
            const BOOL captureSource = sourceValid && !full;
            const size_t bytes = rows * row, slotOffset = captureSource ? bytes : 0;
            NSMutableData *data = directoryReady ? [NSMutableData dataWithLength:bytes * (captureSource ? 3 : 1)] : nil;
            uint8_t *out = data.mutableBytes;
            const uint32_t seedBefore = source ? IOSurfaceGetSeed(source) : 0;
            if (out && captureSource) for (size_t y = 0; y < rows; y++) memcpy(out + y * row, base + y * pitch, row);
            struct rdna4n_scan_status scanBefore = {0}, scanAfter = {0};
            BOOL scanReady = NO;
            const uint64_t pollStart = rdna4_now();
            if (out && presentId) for (unsigned attempt = 0; attempt < 100; attempt++) {
                if (!rdna4_capture_scan_status(&scanBefore)) break;
                if (rdna4_capture_scan_matches(&scanBefore, kernelSlot, presentId)) { scanReady = YES; break; }
                const BOOL awaitingFetch = scanBefore.pending_slot == RDNA4N_SCAN_NO_SLOT &&
                    scanBefore.front_slot == kernelSlot && scanBefore.slot[kernelSlot].mc &&
                    scanBefore.plane_mc == scanBefore.slot[kernelSlot].mc;
                if (!scanBefore.acquired || scanBefore.flags || scanBefore.presents != presentId ||
                    (scanBefore.pending_slot != kernelSlot && !awaitingFetch)) break;
                usleep(1000);
            }
            const uint64_t pollCost = rdna4_now() - pollStart;
            BOOL unresolved = NO;
            const BOOL copied = out && rdna4_capture_readback(slotBuffer, row, pitch, rows, out + slotOffset, &unresolved);
            const BOOL scanVerified = copied && scanReady && rdna4_capture_scan_status(&scanAfter) &&
                rdna4_capture_scan_matches(&scanAfter, kernelSlot, presentId) && scanBefore.earliest_mc == scanAfter.earliest_mc &&
                scanBefore.slot[kernelSlot].latched_frame &&
                scanBefore.slot[kernelSlot].latched_frame == scanAfter.slot[kernelSlot].latched_frame;
            if (copied && captureSource) for (size_t y = 0; y < rows; y++) memcpy(out + 2 * bytes + y * row, base + y * pitch, row);
            const uint32_t seedAfter = source ? IOSurfaceGetSeed(source) : 0;
            if (source) CFRelease(source);
            pthread_mutex_lock(&RDNA4S.mu);
            const uint64_t submission = RDNA4S.sm.submit_hi;
            if (!unresolved) RDNA4S.sm.pin[slot]--;
            pthread_mutex_unlock(&RDNA4S.mu);
            const uint64_t cost = rdna4_now() - start;
            const uint64_t copyCost = cost - pollCost;
            if (!full && copyCost > NSEC_PER_SEC / 200)
                atomic_store(&next, now + 100 * MIN(NSEC_PER_SEC / 10, copyCost));
            BOOL okay = NO;
            if (copied) {
                NSString *stem = [NSString stringWithFormat:@"%s/present-%d-%llu-%llu", path, getpid(),
                    (unsigned long long)seq, (unsigned long long)now];
                NSMutableDictionary *record = [@{ @"sequence": @(seq), @"surface": @(sid), @"slot": @(slot),
                    @"present_id": @(presentId), @"capture_mode": full ? @"full" : @"top",
                    @"time_ns": @(now), @"cost_ns": @(cost), @"latest_submission": @(submission),
                    @"width": @(width), @"height": @(height), @"captured_rows": @(rows), @"bytes_per_row": @(row),
                    @"pixel_format": @"BGRA8", @"source_valid": @(sourceValid), @"slot_offset": @(slotOffset),
                    @"source_before_offset": @0, @"source_after_offset": @(2 * bytes),
                    @"source_unchanged": @(captureSource && !memcmp(out, out + 2 * bytes, bytes)),
                    @"source_matches_slot": @(captureSource && !memcmp(out, out + slotOffset, bytes)),
                    @"full_frame": rows == height ? @YES : @NO, @"scanout_verified": @(scanVerified),
                    @"scanout_before_present": @(scanBefore.presents), @"scanout_after_present": @(scanAfter.presents),
                    @"scanout_before_mc": @(scanBefore.earliest_mc), @"scanout_after_mc": @(scanAfter.earliest_mc),
                    @"scanout_slot_mc": @(scanBefore.slot[kernelSlot].mc),
                    @"scanout_latched_frame": @(scanBefore.slot[kernelSlot].latched_frame),
                    @"seed_before": @(seedBefore), @"seed_after": @(seedAfter),
                    @"raw_file": [[stem lastPathComponent] stringByAppendingString:@".raw"] } mutableCopy];
                char boot[64] = {0};
                size_t bootSize = sizeof boot - 1;
                if (sysctlbyname("kern.bootsessionuuid", boot, &bootSize, NULL, 0) == 0)
                    record[@"boot_uuid"] = [NSString stringWithUTF8String:boot];
                if (full) record[@"slot_sha256"] = rdna4_sha256hex(data);
                NSData *json = [NSJSONSerialization dataWithJSONObject:record options:0 error:NULL];
                okay = json && rdna4_dump_write([[stem stringByAppendingString:@".raw"] fileSystemRepresentation], data) &&
                    rdna4_dump_write([[stem stringByAppendingString:@".json"] fileSystemRepresentation], json);
                if (okay) [saved addObject:stem];
                else { unlink([[stem stringByAppendingString:@".raw"] fileSystemRepresentation]); unlink([[stem stringByAppendingString:@".json"] fileSystemRepresentation]); }
                while (saved.count > (full ? 2u : 16u)) {
                    NSString *old = saved.firstObject;
                    unlink([[old stringByAppendingString:@".raw"] fileSystemRepresentation]);
                    unlink([[old stringByAppendingString:@".json"] fileSystemRepresentation]);
                    [saved removeObjectAtIndex:0];
                }
            }
            RDNA4LOG("scanout: %s capture seq %llu slot %d id %llu status=%s bytes=%lu cost=%.1f us verified=%d", full ? "full" : "top", (unsigned long long)seq,
                slot, (unsigned long long)presentId, okay ? "saved" : "failed", (unsigned long)data.length, cost / 1000.0, scanVerified);
            if (!unresolved) atomic_store(&writing, 0);
        }
    });
}
