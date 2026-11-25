// garnet_sparse_io.m - GarnetMetal modular compilation unit.
#if !defined(GARNET_INTERNAL) && !defined(GARNET_METAL_MODULAR_BUILD)
#define GARNET_INTERNAL static
#endif
#ifdef GARNET_METAL_MODULAR_BUILD
#include "garnet_metal_internal.h"
#endif

// ---------------------------------------------------------------------------------------------------------------
// Metal 3 Fast Resource Loading (Task 4.1): RDNA4IOFileHandle, RDNA4IOCommandQueue, RDNA4IOCommandBuffer.
//
// Wave 2B GarnetDevice.m integration:
//   1. Include order in GarnetDevice.m: place after `#include "rdna4_gl_drawable_impl.m"`
//      (after RDNA4Buffer, RDNA4Texture, RDNA4CommandQueue, rdna4_full_barrier, rdna4_oneshot,
//      and rdna4_event_* / rdna4_schedule_kick are defined):
//        #include "rdna4_residency_set_impl.m"
//        #include "rdna4_io_queue_impl.m"
//   2. Device factory selectors on @implementation GarnetDevice:
//        - (id)newIOFileHandleWithURL:(NSURL *)url error:(NSError **)error {
//            return [[RDNA4IOFileHandle alloc] initWithDevice:self url:url
//                                           compressionMethod:RDNA4IO_COMP_NONE
//                                                  compressed:NO error:error];
//        }
//        - (id)newIOFileHandleWithURL:(NSURL *)url compressionMethod:(NSInteger)method error:(NSError **)error {
//            return [[RDNA4IOFileHandle alloc] initWithDevice:self url:url
//                                           compressionMethod:method
//                                                  compressed:YES error:error];
//        }
//        - (id)newIOHandleWithURL:(NSURL *)url error:(NSError **)error {
//            return [self newIOFileHandleWithURL:url error:error];
//        }
//        - (id)newIOHandleWithURL:(NSURL *)url compressionMethod:(NSInteger)method error:(NSError **)error {
//            return [self newIOFileHandleWithURL:url compressionMethod:method error:error];
//        }
//        - (id)newIOCommandQueueWithDescriptor:(id)descriptor error:(NSError **)error {
//            return [[RDNA4IOCommandQueue alloc] initWithDevice:self descriptor:descriptor error:error];
//        }
//   3. Protocols (MTLIOFileHandle, MTLIOCommandQueue, MTLIOCommandBuffer) are attached automatically
//      in +initialize via rdna4_add_protocols() and may also be listed in GarnetDevice +load census.
// ---------------------------------------------------------------------------------------------------------------

_Static_assert(RDNA4_IO_MAX_COMMANDS == RDNA4DC_IO_MAX_COMMANDS,
               "RDNA4_IO_MAX_COMMANDS must match RDNA4DC_IO_MAX_COMMANDS (8192)");

#if defined(__MAC_13_0)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunguarded-availability-new"
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
_Static_assert((int)MTLIOStatusPending == RDNA4IO_STATUS_PENDING &&
               (int)MTLIOStatusCancelled == RDNA4IO_STATUS_CANCELLED &&
               (int)MTLIOStatusError == RDNA4IO_STATUS_ERROR &&
               (int)MTLIOStatusComplete == RDNA4IO_STATUS_COMPLETE, "MTLIOStatus values");
_Static_assert((int)MTLIOErrorURLInvalid == RDNA4IO_ERR_URL_INVALID &&
               (int)MTLIOErrorInternal == RDNA4IO_ERR_INTERNAL, "MTLIOError values");
_Static_assert((int)MTLIOCommandQueueTypeConcurrent == RDNA4IO_QUEUE_CONCURRENT &&
               (int)MTLIOCommandQueueTypeSerial == RDNA4IO_QUEUE_SERIAL, "MTLIOCommandQueueType values");
_Static_assert((int)MTLIOPriorityHigh == RDNA4IO_PRIO_HIGH &&
               (int)MTLIOPriorityNormal == RDNA4IO_PRIO_NORMAL &&
               (int)MTLIOPriorityLow == RDNA4IO_PRIO_LOW, "MTLIOPriority values");
_Static_assert((int)MTLIOCompressionMethodZlib == RDNA4IO_COMP_ZLIB &&
               (int)MTLIOCompressionMethodLZFSE == RDNA4IO_COMP_LZFSE &&
               (int)MTLIOCompressionMethodLZ4 == RDNA4IO_COMP_LZ4 &&
               (int)MTLIOCompressionMethodLZMA == RDNA4IO_COMP_LZMA &&
               (int)MTLIOCompressionMethodLZBitmap == RDNA4IO_COMP_LZBITMAP, "MTLIOCompressionMethod values");
#pragma clang diagnostic pop
#endif

static NSErrorDomain rdna4_io_error_domain(void) {
#if defined(__MAC_13_0)
    if (@available(macOS 13.0, *)) return MTLIOErrorDomain;
#endif
    return @"MTLIOError";
}

static NSError *rdna4_io_err(NSInteger code, NSString *msg) {
    NSInteger c = (code == RDNA4IO_ERR_URL_INVALID) ? RDNA4IO_ERR_URL_INVALID : RDNA4IO_ERR_INTERNAL;
    return [NSError errorWithDomain:rdna4_io_error_domain()
                               code:c
                           userInfo:@{ NSLocalizedDescriptionKey: msg ?: @"I/O error" }];
}

// Serializes Vulkan one-shot staging copies and texture layout tracking across concurrent IO workers
// so RDNA4R.opool external synchronization is strictly upheld while disk pread() runs in parallel.
static pthread_mutex_t rdna4_io_gpu_mu = PTHREAD_MUTEX_INITIALIZER;

static dispatch_queue_t rdna4_io_event_listener_queue(void) {
    static dispatch_queue_t q;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        q = dispatch_queue_create("rdna4.io.event-listener", DISPATCH_QUEUE_SERIAL);
    });
    return q;
}

static NSInteger rdna4_io_desc_int(id obj, const char *selName, NSInteger fallback) {
    SEL s = sel_registerName(selName);
    if (!obj || ![obj respondsToSelector:s]) return fallback;
    return ((NSInteger (*)(id, SEL))objc_msgSend)(obj, s);
}

static NSUInteger rdna4_io_desc_uint(id obj, const char *selName, NSUInteger fallback) {
    SEL s = sel_registerName(selName);
    if (!obj || ![obj respondsToSelector:s]) return fallback;
    return ((NSUInteger (*)(id, SEL))objc_msgSend)(obj, s);
}

static id rdna4_io_desc_obj(id obj, const char *selName) {
    SEL s = sel_registerName(selName);
    if (!obj || ![obj respondsToSelector:s]) return nil;
    return ((id (*)(id, SEL))objc_msgSend)(obj, s);
}

// ---------------------------------------------------------------------------
// RDNA4IOFileHandle (MTLIOFileHandle)
// ---------------------------------------------------------------------------

@implementation RDNA4IOFileHandle
+ (void)initialize {
    if (self == [RDNA4IOFileHandle class]) {
        static dispatch_once_t once;
        dispatch_once(&once, ^{
            const char *n[] = { "MTLIOFileHandle" };
            rdna4_add_protocols(self, n, 1, "RDNA4IOFileHandle");
        });
    }
}
RDNA4_DNR(RDNA4IOFileHandle)

- (instancetype)initWithDevice:(id)dev url:(NSURL *)url
             compressionMethod:(NSInteger)method compressed:(BOOL)compressed
                         error:(NSError **)err {
    self = [super init];
    if (!self) return nil;
    _dev = dev;
    _fd = -1;
    if (!url || ![url isKindOfClass:[NSURL class]] || ![url isFileURL]) {
        if (err) *err = rdna4_io_err(RDNA4IO_ERR_URL_INVALID, @"MTLIOFileHandle requires a valid file URL");
        return nil;
    }
    if (compressed) {
        int mcode = RDNA4IO_ERR_INTERNAL;
        const char *why = NULL;
        rdna4io_reason_t cr = rdna4io_check_compression((int)method, &mcode, &why);
        if (cr != RDNA4IO_OK) {
            if (err) *err = rdna4_io_err(mcode, [NSString stringWithUTF8String:why ? why : rdna4io_reason_text(cr)]);
            return nil;
        }
    }
    const char *path = url.fileSystemRepresentation;
    if (!path || !path[0]) {
        if (err) *err = rdna4_io_err(RDNA4IO_ERR_URL_INVALID, @"MTLIOFileHandle URL has an empty path");
        return nil;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        int e = errno;
        if (err) *err = rdna4_io_err(RDNA4IO_ERR_URL_INVALID,
            [NSString stringWithFormat:@"cannot open I/O file '%s': %s", path, strerror(e)]);
        return nil;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        int e = errno;
        close(fd);
        if (err) *err = rdna4_io_err(RDNA4IO_ERR_URL_INVALID,
            [NSString stringWithFormat:@"cannot stat I/O file '%s': %s", path, strerror(e)]);
        return nil;
    }
    if (!S_ISREG(st.st_mode) || st.st_size < 0) {
        close(fd);
        if (err) *err = rdna4_io_err(RDNA4IO_ERR_URL_INVALID,
            [NSString stringWithFormat:@"I/O URL '%s' is not a regular file", path]);
        return nil;
    }
    _fd = fd;
    _fileSize = (uint64_t)st.st_size;
    return self;
}

- (void)dealloc {
    if (_fd >= 0) { close(_fd); _fd = -1; }
}

- (NSString *)label { return _lbl; }
- (void)setLabel:(NSString *)l { _lbl = [l copy]; }
- (id)rdna4Device { return _dev; }
- (uint64_t)rdna4FileSize { return _fileSize; }

- (BOOL)rdna4ReadInto:(void *)dst size:(size_t)size offset:(uint64_t)offset error:(NSError **)err {
    if (!size) return YES;
    if (!dst || _fd < 0) {
        if (err) *err = rdna4_io_err(RDNA4IO_ERR_INTERNAL, @"invalid file handle or destination pointer");
        return NO;
    }
    if (!rdna4io_range_ok(_fileSize, offset, (uint64_t)size) ||
        offset > (uint64_t)INT64_MAX || (uint64_t)size > (uint64_t)INT64_MAX - offset) {
        if (err) *err = rdna4_io_err(RDNA4IO_ERR_INTERNAL, @"I/O read range exceeds file size");
        return NO;
    }
    uint8_t *out = (uint8_t *)dst;
    size_t done = 0;
    while (done < size) {
        ssize_t n = pread(_fd, out + done, size - done, (off_t)(offset + done));
        if (n < 0) {
            if (errno == EINTR) continue;
            if (err) *err = rdna4_io_err(RDNA4IO_ERR_INTERNAL,
                [NSString stringWithFormat:@"pread failed at offset %llu: %s",
                 (unsigned long long)(offset + done), strerror(errno)]);
            return NO;
        }
        if (n == 0) {
            if (err) *err = rdna4_io_err(RDNA4IO_ERR_INTERNAL,
                [NSString stringWithFormat:@"unexpected EOF at offset %llu",
                 (unsigned long long)(offset + done)]);
            return NO;
        }
        done += (size_t)n;
    }
    return YES;
}
@end

// ---------------------------------------------------------------------------
// Internal I/O command record
// ---------------------------------------------------------------------------

@implementation RDNA4IOCommand
@end

@class RDNA4IOCommandQueue;
@class RDNA4IOCommandBuffer;



// ---------------------------------------------------------------------------
// RDNA4IOCommandQueue (MTLIOCommandQueue)
// ---------------------------------------------------------------------------
@implementation RDNA4IOCommandQueue
+ (void)initialize {
    if (self == [RDNA4IOCommandQueue class]) {
        static dispatch_once_t once;
        dispatch_once(&once, ^{
            const char *n[] = { "MTLIOCommandQueue" };
            rdna4_add_protocols(self, n, 1, "RDNA4IOCommandQueue");
        });
    }
}
RDNA4_DNR(RDNA4IOCommandQueue)

- (instancetype)initWithDevice:(id)dev descriptor:(id)desc error:(NSError **)err {
    self = [super init];
    if (!self) return nil;
    _dev = dev;
    NSInteger type = rdna4_io_desc_int(desc, "type", RDNA4IO_QUEUE_CONCURRENT);
    NSInteger prio = rdna4_io_desc_int(desc, "priority", RDNA4IO_PRIO_NORMAL);
    NSUInteger maxInFlight = rdna4_io_desc_uint(desc, "maxCommandsInFlight", 0);
    uint32_t conc = 1;
    const char *why = NULL;
    if (rdna4io_check_queue_desc((long)type, (long)prio, (uint64_t)maxInFlight, &conc, &why) != RDNA4IO_OK) {
        if (err) *err = rdna4_io_err(RDNA4IO_ERR_INTERNAL,
            [NSString stringWithUTF8String:why ? why : "invalid MTLIOCommandQueueDescriptor"]);
        return nil;
    }
    if (!rdna4_radv_open(err)) return nil;
    _scratchAllocator = rdna4_io_desc_obj(desc, "scratchBufferAllocator");
    pthread_mutex_init(&_qmu, NULL);
    rdna4io_qsched_init(&_sched, type == RDNA4IO_QUEUE_SERIAL, conc);
    _enqueuedWeak = [NSMapTable strongToWeakObjectsMapTable];
    _committedStrong = [NSMutableDictionary dictionary];
    _stagingPool = [NSMutableArray array];
    dispatch_qos_class_t qos = (prio == RDNA4IO_PRIO_HIGH) ? QOS_CLASS_USER_INTERACTIVE
                             : (prio == RDNA4IO_PRIO_LOW)  ? QOS_CLASS_UTILITY
                                                            : QOS_CLASS_USER_INITIATED;
    dispatch_queue_attr_t wattr = dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_CONCURRENT, qos, 0);
    _workQ = dispatch_queue_create("rdna4.io.worker", wattr);
    _compQ = dispatch_queue_create("rdna4.io.completion", DISPATCH_QUEUE_SERIAL);
    return self;
}

- (void)dealloc {
    pthread_mutex_lock(&_qmu);
    rdna4io_qsched_destroy(&_sched);
    [_stagingPool removeAllObjects];
    [_committedStrong removeAllObjects];
    [_enqueuedWeak removeAllObjects];
    pthread_mutex_unlock(&_qmu);
    pthread_mutex_destroy(&_qmu);
}

- (id)device { return _dev; }
- (NSString *)label { return _lbl; }
- (void)setLabel:(NSString *)l { _lbl = [l copy]; }
- (dispatch_queue_t)rdna4IOCompletionQueue { return _compQ; }

- (id)commandBuffer {
    return [[RDNA4IOCommandBuffer alloc] initWithQueue:self retainedReferences:YES];
}

- (id)commandBufferWithUnretainedReferences {
    return [[RDNA4IOCommandBuffer alloc] initWithQueue:self retainedReferences:NO];
}

- (void)rdna4IODispatchLocked {
    for (;;) {
        uint64_t slot = rdna4io_qsched_pick(&_sched);
        if (!slot) break;
        RDNA4IOCommandBuffer *cb = _committedStrong[@(slot)];
        if (!cb) {
            rdna4io_qsched_complete(&_sched, slot);
            continue;
        }
        dispatch_async(_workQ, ^{
            @autoreleasepool {
                [cb rdna4IOExecuteOnWorkerWithSlot:slot];
            }
        });
    }
}

- (void)enqueueBarrier {
    pthread_mutex_lock(&_qmu);
    rdna4io_qsched_barrier(&_sched);
    pthread_mutex_unlock(&_qmu);
}

- (uint64_t)rdna4IOReserveSlotForBuffer:(RDNA4IOCommandBuffer *)cb {
    pthread_mutex_lock(&_qmu);
    uint64_t slot = rdna4io_qsched_reserve(&_sched, 0);
    if (slot) [_enqueuedWeak setObject:cb forKey:@(slot)];
    pthread_mutex_unlock(&_qmu);
    return slot;
}

- (BOOL)rdna4IOCommitBuffer:(RDNA4IOCommandBuffer *)cb slot:(uint64_t)slot {
    pthread_mutex_lock(&_qmu);
    BOOL ok = rdna4io_qsched_mark_committed(&_sched, slot) != 0;
    if (ok) {
        [_enqueuedWeak removeObjectForKey:@(slot)];
        _committedStrong[@(slot)] = cb;
        [self rdna4IODispatchLocked];
    }
    pthread_mutex_unlock(&_qmu);
    return ok;
}

- (BOOL)rdna4IOCancelSlotIfPending:(uint64_t)slot {
    if (!slot) return NO;
    pthread_mutex_lock(&_qmu);
    BOOL cancelled = rdna4io_qsched_cancel_pending(&_sched, slot) != 0;
    if (cancelled) {
        [_enqueuedWeak removeObjectForKey:@(slot)];
        [_committedStrong removeObjectForKey:@(slot)];
        [self rdna4IODispatchLocked];
    }
    pthread_mutex_unlock(&_qmu);
    return cancelled;
}

- (void)rdna4IOAbandonUncommittedSlot:(uint64_t)slot {
    if (!slot) return;
    pthread_mutex_lock(&_qmu);
    if (rdna4io_qsched_cancel_pending(&_sched, slot)) {
        [_enqueuedWeak removeObjectForKey:@(slot)];
        [self rdna4IODispatchLocked];
    }
    pthread_mutex_unlock(&_qmu);
}

- (void)rdna4IOSlotDone:(uint64_t)slot {
    if (!slot) return;
    pthread_mutex_lock(&_qmu);
    rdna4io_qsched_complete(&_sched, slot);
    [_committedStrong removeObjectForKey:@(slot)];
    [self rdna4IODispatchLocked];
    pthread_mutex_unlock(&_qmu);
}

- (RDNA4Buffer *)rdna4IOAcquireStaging:(NSUInteger)bytes customScratch:(id __strong *)customOut error:(NSError **)err {
    if (customOut) *customOut = nil;
    if (!bytes) bytes = 4;
    if (_scratchAllocator) {
        SEL newSel = sel_registerName("newScratchBufferWithMinimumSize:");
        if ([_scratchAllocator respondsToSelector:newSel]) {
            id sb = (__bridge_transfer id)((void *(*)(id, SEL, NSUInteger))objc_msgSend)(
                _scratchAllocator, newSel, bytes);
            id rawBuf = sb ? rdna4_io_desc_obj(sb, "buffer") : nil;
            if ([rawBuf isKindOfClass:[RDNA4Buffer class]]) {
                RDNA4Buffer *b = (RDNA4Buffer *)rawBuf;
                if ([b device] == _dev && [b contents] && ![b rdna4HostReadOnly] && [b length] >= bytes) {
                    if (customOut) *customOut = sb;
                    return b;
                }
            }
            if (sb && [sb respondsToSelector:sel_registerName("returnToCache")])
                ((void (*)(id, SEL))objc_msgSend)(sb, sel_registerName("returnToCache"));
        }
    }
    if (bytes <= RDNA4_IO_STAGING_CHUNK) {
        pthread_mutex_lock(&_qmu);
        RDNA4Buffer *best = nil;
        NSUInteger bestIdx = NSNotFound;
        for (NSUInteger i = 0; i < _stagingPool.count; i++) {
            RDNA4Buffer *cand = _stagingPool[i];
            if ([cand length] >= bytes && (!best || [cand length] < [best length])) {
                best = cand;
                bestIdx = i;
            }
        }
        if (bestIdx != NSNotFound) [_stagingPool removeObjectAtIndex:bestIdx];
        pthread_mutex_unlock(&_qmu);
        if (best) return best;
    }
    NSUInteger allocSize = bytes < 65536 ? 65536 : bytes;
    if (bytes <= RDNA4_IO_STAGING_CHUNK && allocSize > RDNA4_IO_STAGING_CHUNK) allocSize = RDNA4_IO_STAGING_CHUNK;
    NSError *bErr = nil;
    RDNA4Buffer *nb = [[RDNA4Buffer alloc] initWithDevice:_dev
                                                   length:allocSize
                                                  options:MTLResourceStorageModeShared | RDNA4_OPT_NOZERO
                                                    error:&bErr];
    if (!nb || ![nb contents]) {
        if (err) *err = rdna4_io_err(RDNA4IO_ERR_INTERNAL,
            [NSString stringWithFormat:@"failed to allocate %lu-byte I/O staging buffer: %@",
             (unsigned long)allocSize, bErr.localizedDescription ?: @"allocation failed"]);
        return nil;
    }
    return nb;
}

- (void)rdna4IOReleaseStaging:(RDNA4Buffer *)buf customScratch:(id)custom {
    if (custom) {
        if ([custom respondsToSelector:sel_registerName("returnToCache")])
            ((void (*)(id, SEL))objc_msgSend)(custom, sel_registerName("returnToCache"));
        return;
    }
    if (!buf || [buf length] > RDNA4_IO_STAGING_CHUNK) return;
    pthread_mutex_lock(&_qmu);
    if (_stagingPool.count < 4) [_stagingPool addObject:buf];
    pthread_mutex_unlock(&_qmu);
}
@end

// ---------------------------------------------------------------------------
// RDNA4IOCommandBuffer (MTLIOCommandBuffer)
// ---------------------------------------------------------------------------
@implementation RDNA4IOCommandBuffer
+ (void)initialize {
    if (self == [RDNA4IOCommandBuffer class]) {
        static dispatch_once_t once;
        dispatch_once(&once, ^{
            const char *n[] = { "MTLIOCommandBuffer" };
            rdna4_add_protocols(self, n, 1, "RDNA4IOCommandBuffer");
        });
    }
}
RDNA4_DNR(RDNA4IOCommandBuffer)

- (instancetype)initWithQueue:(RDNA4IOCommandQueue *)queue retainedReferences:(BOOL)retained {
    self = [super init];
    if (!self) return nil;
    _q = queue;
    _dev = [queue device];
    _retained = retained;
    pthread_mutex_init(&_mu, NULL);
    pthread_cond_init(&_cond, NULL);
    rdna4io_cb_init(&_state);
    _cmds = [NSMutableArray array];
    _handlers = [NSMutableArray array];
    return self;
}

- (void)dealloc {
    uint64_t abandonSlot = 0;
    pthread_mutex_lock(&_mu);
    if (_slotId && _state.phase < RDNA4IO_PHASE_COMMITTED) abandonSlot = _slotId;
    pthread_mutex_unlock(&_mu);
    if (abandonSlot && _q) [_q rdna4IOAbandonUncommittedSlot:abandonSlot];
    pthread_cond_destroy(&_cond);
    pthread_mutex_destroy(&_mu);
}

- (NSString *)label { return _lbl; }
- (void)setLabel:(NSString *)l { _lbl = [l copy]; }
- (BOOL)retainedReferences { return _retained; }

- (NSInteger)status {
    pthread_mutex_lock(&_mu);
    NSInteger s = (NSInteger)_state.status;
    pthread_mutex_unlock(&_mu);
    return s;
}

- (NSError *)error {
    pthread_mutex_lock(&_mu);
    NSError *e = _err;
    pthread_mutex_unlock(&_mu);
    return e;
}

- (void)rdna4IOLatchLocked:(rdna4io_reason_t)reason mtlErr:(NSInteger)code detail:(NSString *)detail {
    rdna4io_cb_latch_error(&_state, (int)code, reason);
    if (!_err) _err = rdna4_io_err(code, detail ?: [NSString stringWithUTF8String:rdna4io_reason_text(reason)]);
}

- (BOOL)rdna4IORecordCommandLocked:(RDNA4IOCommand *)cmd {
    rdna4io_reason_t r = rdna4io_cb_add_command(&_state);
    if (r != RDNA4IO_OK) {
        if (r == RDNA4IO_R_COMMAND_LIMIT)
            [self rdna4IOLatchLocked:r mtlErr:RDNA4IO_ERR_INTERNAL detail:nil];
        return NO;
    }
    if (cmd) [_cmds addObject:cmd];
    return YES;
}

- (void)addCompletedHandler:(void (^)(id))block {
    if (!block) return;
    void (^copyBlock)(id) = [block copy];
    BOOL alreadyDone = NO;
    pthread_mutex_lock(&_mu);
    if (_state.phase == RDNA4IO_PHASE_TERMINAL) alreadyDone = YES;
    else [_handlers addObject:copyBlock];
    pthread_mutex_unlock(&_mu);
    if (alreadyDone) {
        dispatch_async([_q rdna4IOCompletionQueue], ^{ copyBlock(self); });
    }
}

- (void)loadBytes:(void *)pointer size:(NSUInteger)size
     sourceHandle:(id)sourceHandle sourceHandleOffset:(NSUInteger)sourceHandleOffset {
    pthread_mutex_lock(&_mu);
    if (![self rdna4IORecordCommandLocked:nil]) { pthread_mutex_unlock(&_mu); return; }
    if (![sourceHandle isKindOfClass:[RDNA4IOFileHandle class]]) {
        [self rdna4IOLatchLocked:RDNA4IO_R_INVALID_ARG mtlErr:RDNA4IO_ERR_INTERNAL
                          detail:@"loadBytes: sourceHandle is not a valid RDNA4IOFileHandle"];
        pthread_mutex_unlock(&_mu); return;
    }
    RDNA4IOFileHandle *fh = (RDNA4IOFileHandle *)sourceHandle;
    if ([fh rdna4Device] != _dev) {
        [self rdna4IOLatchLocked:RDNA4IO_R_DEVICE_MISMATCH mtlErr:RDNA4IO_ERR_INTERNAL detail:nil];
        pthread_mutex_unlock(&_mu); return;
    }
    rdna4io_reason_t r = rdna4io_check_load_bytes([fh rdna4FileSize], (uint64_t)sourceHandleOffset,
                                                  (uint64_t)size, pointer != NULL);
    if (r != RDNA4IO_OK) {
        [self rdna4IOLatchLocked:r mtlErr:RDNA4IO_ERR_INTERNAL detail:nil];
        pthread_mutex_unlock(&_mu); return;
    }
    RDNA4IOCommand *cmd = [RDNA4IOCommand new];
    cmd->kind = RDNA4IOCMD_LOAD_BYTES;
    cmd->handle = fh;
    cmd->dstPtr = pointer;
    cmd->size = (uint64_t)size;
    cmd->srcOffset = (uint64_t)sourceHandleOffset;
    [_cmds addObject:cmd];
    pthread_mutex_unlock(&_mu);
}

- (void)loadBuffer:(id)buffer offset:(NSUInteger)offset size:(NSUInteger)size
      sourceHandle:(id)sourceHandle sourceHandleOffset:(NSUInteger)sourceHandleOffset {
    pthread_mutex_lock(&_mu);
    if (![self rdna4IORecordCommandLocked:nil]) { pthread_mutex_unlock(&_mu); return; }
    if (![sourceHandle isKindOfClass:[RDNA4IOFileHandle class]] ||
        ![buffer isKindOfClass:[RDNA4Buffer class]]) {
        [self rdna4IOLatchLocked:RDNA4IO_R_INVALID_ARG mtlErr:RDNA4IO_ERR_INTERNAL
                          detail:@"loadBuffer: invalid buffer or source handle"];
        pthread_mutex_unlock(&_mu); return;
    }
    RDNA4IOFileHandle *fh = (RDNA4IOFileHandle *)sourceHandle;
    RDNA4Buffer *dst = (RDNA4Buffer *)buffer;
    if ([fh rdna4Device] != _dev || [dst device] != _dev) {
        [self rdna4IOLatchLocked:RDNA4IO_R_DEVICE_MISMATCH mtlErr:RDNA4IO_ERR_INTERNAL detail:nil];
        pthread_mutex_unlock(&_mu); return;
    }
    if ([dst rdna4HostReadOnly]) {
        [self rdna4IOLatchLocked:RDNA4IO_R_INVALID_ARG mtlErr:RDNA4IO_ERR_INTERNAL
                          detail:@"loadBuffer: destination buffer is read-only"];
        pthread_mutex_unlock(&_mu); return;
    }
    rdna4io_reason_t r = rdna4io_check_load_buffer([fh rdna4FileSize], (uint64_t)sourceHandleOffset,
                                                   (uint64_t)[dst length], (uint64_t)offset, (uint64_t)size);
    if (r != RDNA4IO_OK) {
        [self rdna4IOLatchLocked:r mtlErr:RDNA4IO_ERR_INTERNAL detail:nil];
        pthread_mutex_unlock(&_mu); return;
    }
    RDNA4IOCommand *cmd = [RDNA4IOCommand new];
    cmd->kind = RDNA4IOCMD_LOAD_BUFFER;
    cmd->handle = fh;
    cmd->dstBuffer = dst;
    cmd->dstOffset = (uint64_t)offset;
    cmd->size = (uint64_t)size;
    cmd->srcOffset = (uint64_t)sourceHandleOffset;
    [_cmds addObject:cmd];
    pthread_mutex_unlock(&_mu);
}

- (void)loadTexture:(id)texture slice:(NSUInteger)slice level:(NSUInteger)level size:(MTLSize)size
  sourceBytesPerRow:(NSUInteger)sourceBytesPerRow sourceBytesPerImage:(NSUInteger)sourceBytesPerImage
  destinationOrigin:(MTLOrigin)destinationOrigin sourceHandle:(id)sourceHandle
 sourceHandleOffset:(NSUInteger)sourceHandleOffset {
    pthread_mutex_lock(&_mu);
    if (![self rdna4IORecordCommandLocked:nil]) { pthread_mutex_unlock(&_mu); return; }
    if (![sourceHandle isKindOfClass:[RDNA4IOFileHandle class]] ||
        ![texture isKindOfClass:[RDNA4Texture class]]) {
        [self rdna4IOLatchLocked:RDNA4IO_R_INVALID_ARG mtlErr:RDNA4IO_ERR_INTERNAL
                          detail:@"loadTexture: invalid texture or source handle"];
        pthread_mutex_unlock(&_mu); return;
    }
    RDNA4IOFileHandle *fh = (RDNA4IOFileHandle *)sourceHandle;
    RDNA4Texture *tex = (RDNA4Texture *)texture;
    if ([fh rdna4Device] != _dev || [tex device] != _dev) {
        [self rdna4IOLatchLocked:RDNA4IO_R_DEVICE_MISMATCH mtlErr:RDNA4IO_ERR_INTERNAL detail:nil];
        pthread_mutex_unlock(&_mu); return;
    }
    if ([tex rdna4HostReadOnly] || [tex rdna4Samples] > 1 ||
        (![tex rdna4IsTexel] && ![tex rdna4CopyAspect])) {
        [self rdna4IOLatchLocked:RDNA4IO_R_BAD_LAYOUT mtlErr:RDNA4IO_ERR_INTERNAL
                          detail:@"loadTexture: texture is read-only, multisample, or combined depth/stencil"];
        pthread_mutex_unlock(&_mu); return;
    }
    MTLTextureType tt = [tex textureType];
    BOOL is1D = (tt == MTLTextureType1D || tt == MTLTextureType1DArray || tt == MTLTextureTypeTextureBuffer);
    BOOL is3D = (tt == MTLTextureType3D);
    rdna4io_tex_plan_t plan = {0};
    rdna4io_reason_t r = rdna4io_plan_texture_load(
        [fh rdna4FileSize], (uint64_t)sourceHandleOffset,
        (uint64_t)sourceBytesPerRow, (uint64_t)sourceBytesPerImage,
        (uint64_t)[tex width], (uint64_t)[tex height], (uint64_t)[tex depth],
        (uint64_t)[tex mipmapLevelCount], (uint64_t)[tex rdna4Layers],
        is1D ? 1 : 0, is3D ? 1 : 0, [tex isCompressed] ? 1 : 0, [tex bytesPerPixel],
        (uint64_t)slice, (uint64_t)level,
        (uint64_t)destinationOrigin.x, (uint64_t)destinationOrigin.y, (uint64_t)destinationOrigin.z,
        (uint64_t)size.width, (uint64_t)size.height, (uint64_t)size.depth,
        &plan);
    if (r != RDNA4IO_OK || !rdna4_region_ok(tex, (MTLRegion){ destinationOrigin, size }, level, slice, "loadTexture")) {
        [self rdna4IOLatchLocked:(r != RDNA4IO_OK ? r : RDNA4IO_R_DST_OOB) mtlErr:RDNA4IO_ERR_INTERNAL detail:nil];
        pthread_mutex_unlock(&_mu); return;
    }
    RDNA4IOCommand *cmd = [RDNA4IOCommand new];
    cmd->kind = RDNA4IOCMD_LOAD_TEXTURE;
    cmd->handle = fh;
    cmd->dstTexture = tex;
    cmd->srcOffset = (uint64_t)sourceHandleOffset;
    cmd->slice = slice;
    cmd->level = level;
    cmd->origin = destinationOrigin;
    cmd->texSize = size;
    cmd->texPlan = plan;
    [_cmds addObject:cmd];
    pthread_mutex_unlock(&_mu);
}

- (void)copyStatusToBuffer:(id)buffer offset:(NSUInteger)offset {
    pthread_mutex_lock(&_mu);
    if (![self rdna4IORecordCommandLocked:nil]) { pthread_mutex_unlock(&_mu); return; }
    if (![buffer isKindOfClass:[RDNA4Buffer class]] || [(RDNA4Buffer *)buffer device] != _dev ||
        [(RDNA4Buffer *)buffer rdna4HostReadOnly] ||
        !rdna4io_range_ok((uint64_t)[(RDNA4Buffer *)buffer length], (uint64_t)offset, sizeof(int64_t))) {
        [self rdna4IOLatchLocked:RDNA4IO_R_DST_OOB mtlErr:RDNA4IO_ERR_INTERNAL
                          detail:@"copyStatusToBuffer: invalid buffer, device, or offset"];
        pthread_mutex_unlock(&_mu); return;
    }
    RDNA4IOCommand *cmd = [RDNA4IOCommand new];
    cmd->kind = RDNA4IOCMD_COPY_STATUS;
    cmd->dstBuffer = (RDNA4Buffer *)buffer;
    cmd->dstOffset = (uint64_t)offset;
    [_cmds addObject:cmd];
    pthread_mutex_unlock(&_mu);
}

- (void)addBarrier {
    pthread_mutex_lock(&_mu);
    RDNA4IOCommand *cmd = [RDNA4IOCommand new];
    cmd->kind = RDNA4IOCMD_BARRIER;
    (void)[self rdna4IORecordCommandLocked:cmd];
    pthread_mutex_unlock(&_mu);
}

- (void)waitForEvent:(id)event value:(uint64_t)value {
    pthread_mutex_lock(&_mu);
    if (![self rdna4IORecordCommandLocked:nil]) { pthread_mutex_unlock(&_mu); return; }
    if (!event || !rdna4_event_is_shared(event)) {
        [self rdna4IOLatchLocked:RDNA4IO_R_INVALID_ARG mtlErr:RDNA4IO_ERR_INTERNAL
                          detail:@"waitForEvent: requires an MTLSharedEvent"];
        pthread_mutex_unlock(&_mu); return;
    }
    RDNA4IOCommand *cmd = [RDNA4IOCommand new];
    cmd->kind = RDNA4IOCMD_WAIT_EVENT;
    cmd->event = event;
    cmd->eventValue = value;
    [_cmds addObject:cmd];
    pthread_mutex_unlock(&_mu);
}

- (void)signalEvent:(id)event value:(uint64_t)value {
    pthread_mutex_lock(&_mu);
    if (![self rdna4IORecordCommandLocked:nil]) { pthread_mutex_unlock(&_mu); return; }
    if (!event || !rdna4_event_is_shared(event)) {
        [self rdna4IOLatchLocked:RDNA4IO_R_INVALID_ARG mtlErr:RDNA4IO_ERR_INTERNAL
                          detail:@"signalEvent: requires an MTLSharedEvent"];
        pthread_mutex_unlock(&_mu); return;
    }
    RDNA4IOCommand *cmd = [RDNA4IOCommand new];
    cmd->kind = RDNA4IOCMD_SIGNAL_EVENT;
    cmd->event = event;
    cmd->eventValue = value;
    [_cmds addObject:cmd];
    pthread_mutex_unlock(&_mu);
}

- (void)pushDebugGroup:(NSString *)string { (void)string; }
- (void)popDebugGroup {}

- (void)enqueue {
    pthread_mutex_lock(&_mu);
    BOOL needReserve = (!_slotId && !_finishing && rdna4io_cb_enqueue(&_state) != 0);
    pthread_mutex_unlock(&_mu);
    if (!needReserve) return;
    uint64_t slot = [_q rdna4IOReserveSlotForBuffer:self];
    BOOL abandon = NO;
    pthread_mutex_lock(&_mu);
    if (!slot) {
        [self rdna4IOLatchLocked:RDNA4IO_R_INVALID_ARG mtlErr:RDNA4IO_ERR_INTERNAL
                          detail:@"cannot reserve I/O queue slot"];
    } else if (_slotId || _finishing || _state.phase >= RDNA4IO_PHASE_COMMITTED) {
        abandon = YES;
    } else {
        _slotId = slot;
    }
    pthread_mutex_unlock(&_mu);
    if (abandon) [_q rdna4IOAbandonUncommittedSlot:slot];
}

- (BOOL)rdna4IOWriteStatus:(int64_t)statusVal toBuffer:(RDNA4Buffer *)dst offset:(uint64_t)off {
    if ([dst contents]) {
        memcpy((uint8_t *)[dst contents] + off, &statusVal, sizeof statusVal);
        return YES;
    }
    id custom = nil;
    NSError *err = nil;
    RDNA4Buffer *stg = [_q rdna4IOAcquireStaging:sizeof statusVal customScratch:&custom error:&err];
    if (!stg) return NO;
    memcpy([stg contents], &statusVal, sizeof statusVal);
    BOOL unresolved = NO;
    pthread_mutex_lock(&rdna4_io_gpu_mu);
    BOOL ok = rdna4_oneshot(&err, ^(VkCommandBuffer cmd) {
        (void)custom;
        VkBufferCopy bc = { [stg rdna4VkOffset], [dst rdna4VkOffset] + (VkDeviceSize)off, sizeof statusVal };
        vkCmdCopyBuffer(cmd, [stg vkBuffer], [dst vkBuffer], 1, &bc);
        rdna4_full_barrier(cmd);
    }, &unresolved);
    pthread_mutex_unlock(&rdna4_io_gpu_mu);
    if (!unresolved) [_q rdna4IOReleaseStaging:stg customScratch:custom];
    return ok;
}

- (void)rdna4IOFinishWithReason:(rdna4io_reason_t)reason mtlErr:(NSInteger)mtlErr
                         detail:(NSString *)detail startIdx:(NSUInteger)startIdx
                    releaseSlot:(uint64_t)releaseSlot {
    NSArray<void (^)(id)> *handlers = nil;
    NSArray<RDNA4IOCommand *> *cmds = nil;
    int finalStatus = RDNA4IO_STATUS_ERROR;

    // Claim single-finalizer ownership without publishing TERMINAL yet, so
    // waitUntilCompleted / status / addCompletedHandler: never observe completion
    // before copyStatusToBuffer:, unsignaled event failure, and slot release finish.
    pthread_mutex_lock(&_mu);
    if (_finishing || _state.phase == RDNA4IO_PHASE_TERMINAL) {
        pthread_mutex_unlock(&_mu);
        return;
    }
    _finishing = YES;
    if (reason != RDNA4IO_OK) [self rdna4IOLatchLocked:reason mtlErr:mtlErr detail:detail];
    if (reason == RDNA4IO_R_CANCELLED || (_state.cancel_requested && reason == RDNA4IO_OK))
        finalStatus = RDNA4IO_STATUS_CANCELLED;
    else if (reason != RDNA4IO_OK || _state.sticky_reason != RDNA4IO_OK)
        finalStatus = RDNA4IO_STATUS_ERROR;
    else
        finalStatus = RDNA4IO_STATUS_COMPLETE;
    cmds = [_cmds copy];
    [_cmds removeAllObjects];
    _slotId = 0;
    pthread_mutex_unlock(&_mu);

    // Flush any copyStatusToBuffer targets with the final status before publishing TERMINAL.
    for (RDNA4IOCommand *c in cmds) {
        if (c->kind == RDNA4IOCMD_COPY_STATUS && c->dstBuffer)
            (void)[self rdna4IOWriteStatus:(int64_t)finalStatus toBuffer:c->dstBuffer offset:c->dstOffset];
    }

    // Fail any unsignaled events so waiting GPU command buffers do not deadlock.
    if (finalStatus != RDNA4IO_STATUS_COMPLETE && startIdx < cmds.count) {
        NSMutableArray *unsignaled = [NSMutableArray array];
        for (NSUInteger i = startIdx; i < cmds.count; i++) {
            RDNA4IOCommand *c = cmds[i];
            if (c->kind == RDNA4IOCMD_SIGNAL_EVENT && c->event)
                [unsignaled addObject:@[c->event, @(c->eventValue)]];
        }
        if (unsignaled.count) {
            rdna4_event_fail_signals(unsignaled);
            rdna4_schedule_kick();
        }
    }

    if (releaseSlot) [_q rdna4IOSlotDone:releaseSlot];

    // Now atomically transition to TERMINAL, snapshot all handlers, and wake waiters.
    pthread_mutex_lock(&_mu);
    (void)rdna4io_cb_finish(&_state, reason, (int)mtlErr);
    if (_state.status == RDNA4IO_STATUS_CANCELLED && !_err)
        _err = rdna4_io_err(RDNA4IO_ERR_INTERNAL, @"I/O command buffer was cancelled");
    else if (_state.status == RDNA4IO_STATUS_ERROR && !_err)
        _err = rdna4_io_err(_state.sticky_mtl_err ? _state.sticky_mtl_err : RDNA4IO_ERR_INTERNAL,
                            [NSString stringWithUTF8String:rdna4io_reason_text(_state.sticky_reason)]);
    handlers = [_handlers copy];
    [_handlers removeAllObjects];
    pthread_cond_broadcast(&_cond);
    pthread_mutex_unlock(&_mu);

    if (handlers.count) {
        dispatch_async([_q rdna4IOCompletionQueue], ^{
            for (void (^h)(id) in handlers) h(self);
        });
    }
}

- (void)commit {
    pthread_mutex_lock(&_mu);
    if (!rdna4io_cb_commit(&_state)) { pthread_mutex_unlock(&_mu); return; }
    BOOL cancelled = _state.cancel_requested != 0;
    uint64_t slot = _slotId;
    pthread_mutex_unlock(&_mu);

    if (cancelled) {
        if (slot) (void)[_q rdna4IOCancelSlotIfPending:slot];
        [self rdna4IOFinishWithReason:RDNA4IO_R_CANCELLED mtlErr:RDNA4IO_ERR_INTERNAL
                               detail:@"I/O command buffer was cancelled before commit"
                             startIdx:0 releaseSlot:0];
        return;
    }

    if (!slot) {
        uint64_t newSlot = [_q rdna4IOReserveSlotForBuffer:self];
        uint64_t abandonSlot = 0;
        pthread_mutex_lock(&_mu);
        if (_slotId) { slot = _slotId; abandonSlot = newSlot; }
        else { _slotId = newSlot; slot = newSlot; }
        cancelled = _state.cancel_requested != 0;
        pthread_mutex_unlock(&_mu);
        if (abandonSlot) [_q rdna4IOAbandonUncommittedSlot:abandonSlot];
        if (!slot) {
            [self rdna4IOFinishWithReason:RDNA4IO_R_INVALID_ARG mtlErr:RDNA4IO_ERR_INTERNAL
                                   detail:@"cannot reserve I/O queue slot on commit"
                                 startIdx:0 releaseSlot:0];
            return;
        }
        if (cancelled && [_q rdna4IOCancelSlotIfPending:slot]) {
            [self rdna4IOFinishWithReason:RDNA4IO_R_CANCELLED mtlErr:RDNA4IO_ERR_INTERNAL
                                   detail:@"I/O command buffer was cancelled before execution"
                                 startIdx:0 releaseSlot:0];
            return;
        }
    }

    if (![_q rdna4IOCommitBuffer:self slot:slot]) {
        [self rdna4IOFinishWithReason:RDNA4IO_R_CANCELLED mtlErr:RDNA4IO_ERR_INTERNAL
                               detail:@"I/O command buffer queue slot was cancelled or unavailable"
                             startIdx:0 releaseSlot:0];
    }
}

- (void)tryCancel {
    BOOL cancelPendingSlot = NO;
    uint64_t slot = 0;
    dispatch_semaphore_t wakeSem = nil;
    pthread_mutex_lock(&_mu);
    if (_finishing || !rdna4io_cb_request_cancel(&_state)) {
        pthread_mutex_unlock(&_mu);
        return;
    }
    if (!_err) _err = rdna4_io_err(RDNA4IO_ERR_INTERNAL, @"I/O command buffer was cancelled");
    if (_state.phase == RDNA4IO_PHASE_COMMITTED && _slotId) {
        cancelPendingSlot = YES;
        slot = _slotId;
    }
    wakeSem = _waitEventSem;
    pthread_mutex_unlock(&_mu);

    if (wakeSem) dispatch_semaphore_signal(wakeSem);
    if (cancelPendingSlot && [_q rdna4IOCancelSlotIfPending:slot]) {
        [self rdna4IOFinishWithReason:RDNA4IO_R_CANCELLED mtlErr:RDNA4IO_ERR_INTERNAL
                               detail:@"I/O command buffer was cancelled before execution"
                             startIdx:0 releaseSlot:0];
    }
}

- (void)waitUntilCompleted {
    pthread_mutex_lock(&_mu);
    while (_state.phase != RDNA4IO_PHASE_TERMINAL) {
        // Avoid hanging forever if someone calls waitUntilCompleted on an uncommitted cancelled buffer.
        if (_state.phase < RDNA4IO_PHASE_COMMITTED && _state.cancel_requested && !_finishing) break;
        pthread_cond_wait(&_cond, &_mu);
    }
    pthread_mutex_unlock(&_mu);
}

- (BOOL)rdna4IOCancelledNow {
    pthread_mutex_lock(&_mu);
    BOOL stop = rdna4io_cb_should_stop(&_state) != 0;
    pthread_mutex_unlock(&_mu);
    return stop;
}

- (BOOL)rdna4IOExecLoadBuffer:(RDNA4IOCommand *)c error:(NSError **)err cancelled:(BOOL *)cancelled {
    if (!c->size) return YES;
    RDNA4Buffer *dst = c->dstBuffer;
    if ([dst contents]) {
        return [c->handle rdna4ReadInto:(uint8_t *)[dst contents] + c->dstOffset
                                   size:(size_t)c->size offset:c->srcOffset error:err];
    }
    uint64_t done = 0;
    while (done < c->size) {
        if ([self rdna4IOCancelledNow]) { if (cancelled) *cancelled = YES; return NO; }
        uint64_t rem = c->size - done;
        NSUInteger chunk = rem > RDNA4_IO_STAGING_CHUNK ? RDNA4_IO_STAGING_CHUNK : (NSUInteger)rem;
        id custom = nil;
        RDNA4Buffer *stg = [_q rdna4IOAcquireStaging:chunk customScratch:&custom error:err];
        if (!stg) return NO;
        BOOL readOK = [c->handle rdna4ReadInto:[stg contents] size:chunk offset:c->srcOffset + done error:err];
        if (!readOK) {
            [_q rdna4IOReleaseStaging:stg customScratch:custom];
            return NO;
        }
        if ([self rdna4IOCancelledNow]) {
            [_q rdna4IOReleaseStaging:stg customScratch:custom];
            if (cancelled) *cancelled = YES;
            return NO;
        }
        VkDeviceSize dstOff = [dst rdna4VkOffset] + (VkDeviceSize)(c->dstOffset + done);
        NSError *gpuErr = nil;
        BOOL unresolved = NO;
        pthread_mutex_lock(&rdna4_io_gpu_mu);
        BOOL gpuOK = rdna4_oneshot(&gpuErr, ^(VkCommandBuffer cmd) {
            (void)custom;
            VkBufferCopy bc = { [stg rdna4VkOffset], dstOff, (VkDeviceSize)chunk };
            vkCmdCopyBuffer(cmd, [stg vkBuffer], [dst vkBuffer], 1, &bc);
            rdna4_full_barrier(cmd);
        }, &unresolved);
        pthread_mutex_unlock(&rdna4_io_gpu_mu);
        if (!unresolved) [_q rdna4IOReleaseStaging:stg customScratch:custom];
        if (!gpuOK) {
            if (err) *err = rdna4_io_err(RDNA4IO_ERR_INTERNAL,
                gpuErr.localizedDescription ?: @"GPU buffer staging upload failed");
            return NO;
        }
        done += chunk;
    }
    return YES;
}

- (BOOL)rdna4IOExecLoadTexture:(RDNA4IOCommand *)c error:(NSError **)err cancelled:(BOOL *)cancelled {
    RDNA4Texture *tex = c->dstTexture;
    const rdna4io_tex_plan_t *p = &c->texPlan;
    if (!p->packed_bytes) return YES;

    if ([tex rdna4IsTexel]) {
        RDNA4IOCommand *tmp = [RDNA4IOCommand new];
        tmp->kind = RDNA4IOCMD_LOAD_BUFFER;
        tmp->handle = c->handle;
        tmp->dstBuffer = [tex rdna4TexelBuffer];
        tmp->dstOffset = (uint64_t)[tex bufferOffset] + (uint64_t)c->origin.x * (uint64_t)[tex bytesPerPixel];
        tmp->srcOffset = c->srcOffset;
        tmp->size = p->row_bytes;
        return [self rdna4IOExecLoadBuffer:tmp error:err cancelled:cancelled];
    }

    if ([tex rdna4IsIOS]) {
        uint8_t *base = (uint8_t *)[tex rdna4IBase];
        size_t stride = [tex rdna4IOSBytesPerRow];
        uint32_t bpp = [tex bytesPerPixel];
        for (uint64_t y = 0; y < p->bh; y++) {
            if ([self rdna4IOCancelledNow]) { if (cancelled) *cancelled = YES; return NO; }
            if (![c->handle rdna4ReadInto:base + (c->origin.y + y) * stride + c->origin.x * bpp
                                     size:(size_t)p->row_bytes
                                   offset:c->srcOffset + y * p->row_stride error:err])
                return NO;
        }
        return YES;
    }

    if (p->packed_bytes > SIZE_MAX) {
        if (err) *err = rdna4_io_err(RDNA4IO_ERR_INTERNAL, @"texture upload exceeds addressable memory");
        return NO;
    }
    id custom = nil;
    RDNA4Buffer *stg = [_q rdna4IOAcquireStaging:(NSUInteger)p->packed_bytes customScratch:&custom error:err];
    if (!stg) return NO;

    uint8_t *stgPtr = (uint8_t *)[stg contents];
    BOOL readOK = YES;
    if (p->row_stride == p->row_bytes && (p->depth == 1 || p->img_stride == p->row_bytes * p->bh)) {
        readOK = [c->handle rdna4ReadInto:stgPtr size:(size_t)p->packed_bytes offset:c->srcOffset error:err];
    } else {
        for (uint64_t z = 0; z < p->depth && readOK; z++) {
            for (uint64_t y = 0; y < p->bh && readOK; y++) {
                if ((y & 63u) == 0 && [self rdna4IOCancelledNow]) {
                    if (cancelled) *cancelled = YES;
                    readOK = NO;
                    break;
                }
                uint64_t fileOff = c->srcOffset + z * p->img_stride + y * p->row_stride;
                uint64_t dstOff = (z * p->bh + y) * p->row_bytes;
                readOK = [c->handle rdna4ReadInto:stgPtr + dstOff size:(size_t)p->row_bytes offset:fileOff error:err];
            }
        }
    }
    if (!readOK) {
        [_q rdna4IOReleaseStaging:stg customScratch:custom];
        return NO;
    }
    if ([self rdna4IOCancelledNow]) {
        [_q rdna4IOReleaseStaging:stg customScratch:custom];
        if (cancelled) *cancelled = YES;
        return NO;
    }

    BOOL is3D = ([tex textureType] == MTLTextureType3D);
    VkBufferImageCopy bic = {
        .bufferOffset = [stg rdna4VkOffset],
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = { [tex rdna4CopyAspect], [tex rdna4MipLevel:c->level], [tex rdna4Slice:c->slice], 1 },
        .imageOffset = { (int32_t)c->origin.x, (int32_t)c->origin.y, is3D ? (int32_t)c->origin.z : 0 },
        .imageExtent = { (uint32_t)c->texSize.width, (uint32_t)c->texSize.height, is3D ? (uint32_t)c->texSize.depth : 1 },
    };
    NSError *gpuErr = nil;
    BOOL unresolved = NO;
    pthread_mutex_lock(&rdna4_io_gpu_mu);
    BOOL gpuOK = rdna4_oneshot(&gpuErr, ^(VkCommandBuffer cmd) {
        (void)custom;
        rdna4_tex_to(cmd, tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdCopyBufferToImage(cmd, [stg vkBuffer], [tex vkImage],
                               rdna4_use_layout(tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL), 1, &bic);
        rdna4_tex_to(cmd, tex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        rdna4_full_barrier(cmd);
    }, &unresolved);
    pthread_mutex_unlock(&rdna4_io_gpu_mu);
    if (!unresolved) [_q rdna4IOReleaseStaging:stg customScratch:custom];
    if (!gpuOK) {
        if (err) *err = rdna4_io_err(RDNA4IO_ERR_INTERNAL,
            gpuErr.localizedDescription ?: @"GPU texture staging upload failed");
        return NO;
    }
    return YES;
}

- (BOOL)rdna4IOExecWaitEvent:(id)event value:(uint64_t)value error:(NSError **)err cancelled:(BOOL *)cancelled {
    if (rdna4_event_value(event) >= value) return YES;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    pthread_mutex_lock(&_mu);
    _waitEventSem = sem;
    pthread_mutex_unlock(&_mu);

    if ([event respondsToSelector:@selector(notifyListener:atValue:block:)]) {
        MTLSharedEventListener *listener = [[MTLSharedEventListener alloc]
            initWithDispatchQueue:rdna4_io_event_listener_queue()];
        [(id<MTLSharedEvent>)event notifyListener:listener atValue:value
            block:^(id<MTLSharedEvent> e, uint64_t v) {
                (void)e; (void)v;
                dispatch_semaphore_signal(sem);
            }];
    }

    uint64_t start = rdna4_now();
    BOOL ok = NO;
    for (;;) {
        if (rdna4_event_value(event) >= value) { ok = YES; break; }
        if ([self rdna4IOCancelledNow]) { if (cancelled) *cancelled = YES; break; }
        if (rdna4_event_failed(event, value)) {
            if (err) *err = rdna4_io_err(RDNA4IO_ERR_INTERNAL,
                [NSString stringWithFormat:@"waitForEvent: producer for value %llu failed",
                 (unsigned long long)value]);
            break;
        }
        if (rdna4_now() - start >= 60ULL * NSEC_PER_SEC) {
            if (err) *err = rdna4_io_err(RDNA4IO_ERR_INTERNAL,
                [NSString stringWithFormat:@"waitForEvent: timed out waiting for value %llu",
                 (unsigned long long)value]);
            break;
        }
        dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_MSEC));
    }

    pthread_mutex_lock(&_mu);
    _waitEventSem = nil;
    pthread_mutex_unlock(&_mu);
    return ok;
}

- (void)rdna4IOExecuteOnWorkerWithSlot:(uint64_t)slot {
    NSArray<RDNA4IOCommand *> *cmds = nil;
    pthread_mutex_lock(&_mu);
    int run = rdna4io_cb_begin_run(&_state);
    if (!run) {
        int st = _state.status;
        rdna4io_reason_t reason = _state.sticky_reason;
        int mcode = _state.sticky_mtl_err;
        // Keep COMMITTED/PENDING until rdna4IOFinishWithReason performs the single terminal transition
        // (status-buffer writes, unsignaled event failure, condvar broadcast, and handlers).
        _state.phase = RDNA4IO_PHASE_COMMITTED;
        _state.status = RDNA4IO_STATUS_PENDING;
        pthread_mutex_unlock(&_mu);
        [self rdna4IOFinishWithReason:(st == RDNA4IO_STATUS_CANCELLED ? RDNA4IO_R_CANCELLED : reason)
                               mtlErr:(mcode ? mcode : RDNA4IO_ERR_INTERNAL)
                               detail:nil startIdx:0 releaseSlot:slot];
        return;
    }
    cmds = [_cmds copy];
    pthread_mutex_unlock(&_mu);

    NSUInteger i = 0;
    for (; i < cmds.count; i++) {
        if ([self rdna4IOCancelledNow]) {
            [self rdna4IOFinishWithReason:RDNA4IO_R_CANCELLED mtlErr:RDNA4IO_ERR_INTERNAL
                                   detail:@"I/O command buffer was cancelled"
                                 startIdx:i releaseSlot:slot];
            return;
        }
        RDNA4IOCommand *c = cmds[i];
        NSError *opErr = nil;
        BOOL cancelled = NO;
        BOOL ok = YES;
        switch (c->kind) {
        case RDNA4IOCMD_LOAD_BYTES:
            ok = [c->handle rdna4ReadInto:c->dstPtr size:(size_t)c->size offset:c->srcOffset error:&opErr];
            break;
        case RDNA4IOCMD_LOAD_BUFFER:
            ok = [self rdna4IOExecLoadBuffer:c error:&opErr cancelled:&cancelled];
            break;
        case RDNA4IOCMD_LOAD_TEXTURE:
            ok = [self rdna4IOExecLoadTexture:c error:&opErr cancelled:&cancelled];
            break;
        case RDNA4IOCMD_BARRIER:
            // All prior load commands wait on their own GPU fence before returning,
            // so ordering and visibility are already complete at this barrier.
            break;
        case RDNA4IOCMD_WAIT_EVENT:
            ok = [self rdna4IOExecWaitEvent:c->event value:c->eventValue error:&opErr cancelled:&cancelled];
            break;
        case RDNA4IOCMD_SIGNAL_EVENT:
            rdna4_event_set_value(c->event, c->eventValue);
            rdna4_schedule_kick();
            break;
        case RDNA4IOCMD_COPY_STATUS:
            // Written in rdna4IOFinishWithReason with the terminal status.
            break;
        }
        if (!ok) {
            [self rdna4IOFinishWithReason:(cancelled ? RDNA4IO_R_CANCELLED : RDNA4IO_R_GPU_FAILED)
                                   mtlErr:RDNA4IO_ERR_INTERNAL
                                   detail:opErr.localizedDescription
                                 startIdx:i releaseSlot:slot];
            return;
        }
    }

    [self rdna4IOFinishWithReason:RDNA4IO_OK mtlErr:RDNA4IO_ERR_NONE detail:nil startIdx:cmds.count releaseSlot:slot];
}
@end
// ---------------------------------------------------------------------------------------------------------------
// Metal Sparse Textures & MTLResourceStateCommandEncoder (Task 5.3) over Vulkan vkQueueBindSparse / GFX12 PRT.
//
// CRITICAL: NEVER enable MTLGPUFamilyApple6 (1006). Sparse textures work on Mac2/Metal3 via explicit
// sparseTileSize* queries, sparse heaps, and MTLResourceStateCommandEncoder gated strictly on
// rdna4dc_metal3_sparse_texture_ok() (which itself requires kernel ABI 1.13 RDNA4N_CAP_VM_PRT + RADV 0028).
//
// Wave 3 integration notes (do NOT edit existing files in Wave 2B):
//   1. GarnetDevice.m:
//      a) Place `#include "rdna4_sparse_impl.m"` after `#include "rdna4_gl_drawable_impl.m"`
//         (after RDNA4Buffer, RDNA4Texture, RDNA4Heap, RDNA4Fence, RDNA4CommandBuffer, rdna4_oneshot,
//         and rdna4fw_wait_bounded are defined).
//      b) Register MTLResourceStateCommandEncoder (and optional census entry) in +[GarnetDevice load]:
//           { const char *n[] = { "MTLResourceStateCommandEncoder", "MTLCommandEncoder" };
//             rdna4_add_protocols([RDNA4ResourceStateEncoder class], n, 2, "RDNA4ResourceStateEncoder"); }
//      c) Wire MTLDevice sparse selectors in @implementation GarnetDevice (keep supportsFamily:Apple6 == NO!):
//           - (NSUInteger)sparseTileSizeInBytes { return rdna4_device_sparse_tile_size_in_bytes(); }
//           - (NSUInteger)sparseTileSizeInBytesForSparsePageSize:(NSInteger)sparsePageSize {
//               return rdna4_device_sparse_tile_size_for_page_size(sparsePageSize);
//           }
//           - (MTLSize)sparseTileSizeWithTextureType:(MTLTextureType)textureType
//                                        pixelFormat:(MTLPixelFormat)pixelFormat
//                                        sampleCount:(NSUInteger)sampleCount {
//               return rdna4_device_sparse_tile_size(textureType, pixelFormat, sampleCount);
//           }
//           - (MTLSize)sparseTileSizeWithTextureType:(MTLTextureType)textureType
//                                        pixelFormat:(MTLPixelFormat)pixelFormat
//                                        sampleCount:(NSUInteger)sampleCount
//                                     sparsePageSize:(NSInteger)sparsePageSize {
//               return rdna4_device_sparse_tile_size_with_page_size(textureType, pixelFormat, sampleCount, sparsePageSize);
//           }
//           - (void)convertSparsePixelRegions:(const MTLRegion *)pixelRegions
//                               toTileRegions:(MTLRegion *)tileRegions
//                                withTileSize:(MTLSize)tileSize
//                               alignmentMode:(MTLSparseTextureRegionAlignmentMode)mode
//                                  numRegions:(NSUInteger)numRegions {
//               rdna4_device_convert_sparse_pixel_regions(pixelRegions, tileRegions, tileSize, mode, numRegions);
//           }
//           - (void)convertSparseTileRegions:(const MTLRegion *)tileRegions
//                             toPixelRegions:(MTLRegion *)pixelRegions
//                               withTileSize:(MTLSize)tileSize
//                                 numRegions:(NSUInteger)numRegions {
//               rdna4_device_convert_sparse_tile_regions(tileRegions, pixelRegions, tileSize, numRegions);
//           }
//      d) Route MTLHeapTypeSparse (2) in -[GarnetDevice newHeapWithDescriptor:]:
//           if ((NSInteger)d.type == RDNA4SP_HEAP_SPARSE) return rdna4_sparse_heap_new(self, d, NULL);
//   2. RDNA4CommandBuffer (Encoders) / rdna4_command_buffer_impl.inc:
//      Add resource state encoder factories:
//           - (id)resourceStateCommandEncoder { return rdna4_cb_new_resource_state_encoder(self, nil); }
//           - (id)resourceStateCommandEncoderWithDescriptor:(id)d {
//               return rdna4_cb_new_resource_state_encoder(self, d);
//           }
//      Note: RDNA4ResourceStateEncoder uses rdna4EncodeOrderedHostAction: + binary VkSemaphore handoff
//      between vkQueueBindSparse and vkQueueSubmit, so no scheduler rewrite is required. Optional future
//      optimization: attach waitSemaphores directly onto RDNA4CommandSegment for zero-bubble pipelining.
//   3. rdna4_texture_impl.inc & rdna4_heap_impl.inc:
//      RDNA4SparseTexture subclasses RDNA4Texture (isSparse == YES, firstMipmapInTail, tailSizeInBytes),
//      so all existing render/compute/blit encoders accept it via isKindOfClass:[RDNA4Texture class].
//      RDNA4Heap (RDNA4SparseBacking) attaches a lazy 64 KiB backing pool for Private Automatic/Placement
//      heaps; RDNA4SparseHeap natively backs MTLHeapTypeSparse. For residency sets, relax the temporary
//      `isSparse` check in rdna4_residency_set_impl.inc once Wave 3 lands.
// ---------------------------------------------------------------------------------------------------------------
#ifndef RDNA4_SPARSE_IMPL_INC
#define RDNA4_SPARSE_IMPL_INC


// Lock pure-C constants and wire structs to the real Metal and Vulkan headers.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunguarded-availability-new"
_Static_assert((int)MTLSparseTextureMappingModeMap == RDNA4SP_MODE_MAP &&
               (int)MTLSparseTextureMappingModeUnmap == RDNA4SP_MODE_UNMAP,
               "MTLSparseTextureMappingMode values");
_Static_assert((int)MTLSparseTextureRegionAlignmentModeOutward == RDNA4SP_ALIGN_OUTWARD &&
               (int)MTLSparseTextureRegionAlignmentModeInward == RDNA4SP_ALIGN_INWARD,
               "MTLSparseTextureRegionAlignmentMode values");
_Static_assert((int)MTLHeapTypeAutomatic == RDNA4SP_HEAP_AUTOMATIC &&
               (int)MTLHeapTypePlacement == RDNA4SP_HEAP_PLACEMENT,
               "MTLHeapType automatic/placement values");
#if defined(__MAC_11_0)
_Static_assert((int)MTLHeapTypeSparse == RDNA4SP_HEAP_SPARSE, "MTLHeapTypeSparse value");
_Static_assert(sizeof(MTLMapIndirectArguments) == sizeof(rdna4sp_map_indirect_args) &&
               offsetof(MTLMapIndirectArguments, regionOriginX) == offsetof(rdna4sp_map_indirect_args, region_origin_x) &&
               offsetof(MTLMapIndirectArguments, regionOriginY) == offsetof(rdna4sp_map_indirect_args, region_origin_y) &&
               offsetof(MTLMapIndirectArguments, regionOriginZ) == offsetof(rdna4sp_map_indirect_args, region_origin_z) &&
               offsetof(MTLMapIndirectArguments, regionSizeWidth) == offsetof(rdna4sp_map_indirect_args, region_size_width) &&
               offsetof(MTLMapIndirectArguments, regionSizeHeight) == offsetof(rdna4sp_map_indirect_args, region_size_height) &&
               offsetof(MTLMapIndirectArguments, regionSizeDepth) == offsetof(rdna4sp_map_indirect_args, region_size_depth) &&
               offsetof(MTLMapIndirectArguments, mipMapLevel) == offsetof(rdna4sp_map_indirect_args, mip_map_level) &&
               offsetof(MTLMapIndirectArguments, sliceId) == offsetof(rdna4sp_map_indirect_args, slice_id),
               "MTLMapIndirectArguments layout");
#endif
#if defined(__MAC_13_0)
_Static_assert((int)MTLSparsePageSize16 == RDNA4SP_PAGE_SIZE_16 &&
               (int)MTLSparsePageSize64 == RDNA4SP_PAGE_SIZE_64 &&
               (int)MTLSparsePageSize256 == RDNA4SP_PAGE_SIZE_256,
               "MTLSparsePageSize values");
#endif
#pragma clang diagnostic pop
_Static_assert((unsigned)VK_IMAGE_ASPECT_COLOR_BIT == RDNA4SP_ASP_COLOR &&
               (unsigned)VK_IMAGE_ASPECT_DEPTH_BIT == RDNA4SP_ASP_DEPTH &&
               (unsigned)VK_IMAGE_ASPECT_STENCIL_BIT == RDNA4SP_ASP_STENCIL,
               "VkImageAspectFlagBits sparse values");
_Static_assert((unsigned)MTLPixelFormatA8Unorm == RDNA4SP_PF_A8Unorm &&
               (unsigned)MTLPixelFormatR8Unorm == RDNA4SP_PF_R8Unorm &&
               (unsigned)MTLPixelFormatR8Snorm == RDNA4SP_PF_R8Snorm &&
               (unsigned)MTLPixelFormatR8Uint == RDNA4SP_PF_R8Uint &&
               (unsigned)MTLPixelFormatR8Sint == RDNA4SP_PF_R8Sint &&
               (unsigned)MTLPixelFormatR16Unorm == RDNA4SP_PF_R16Unorm &&
               (unsigned)MTLPixelFormatR16Snorm == RDNA4SP_PF_R16Snorm &&
               (unsigned)MTLPixelFormatR16Uint == RDNA4SP_PF_R16Uint &&
               (unsigned)MTLPixelFormatR16Sint == RDNA4SP_PF_R16Sint &&
               (unsigned)MTLPixelFormatR16Float == RDNA4SP_PF_R16Float &&
               (unsigned)MTLPixelFormatRG8Unorm == RDNA4SP_PF_RG8Unorm &&
               (unsigned)MTLPixelFormatRG8Snorm == RDNA4SP_PF_RG8Snorm &&
               (unsigned)MTLPixelFormatRG8Uint == RDNA4SP_PF_RG8Uint &&
               (unsigned)MTLPixelFormatRG8Sint == RDNA4SP_PF_RG8Sint &&
               (unsigned)MTLPixelFormatR32Uint == RDNA4SP_PF_R32Uint &&
               (unsigned)MTLPixelFormatR32Sint == RDNA4SP_PF_R32Sint &&
               (unsigned)MTLPixelFormatR32Float == RDNA4SP_PF_R32Float &&
               (unsigned)MTLPixelFormatRG16Unorm == RDNA4SP_PF_RG16Unorm &&
               (unsigned)MTLPixelFormatRG16Snorm == RDNA4SP_PF_RG16Snorm &&
               (unsigned)MTLPixelFormatRG16Uint == RDNA4SP_PF_RG16Uint &&
               (unsigned)MTLPixelFormatRG16Sint == RDNA4SP_PF_RG16Sint &&
               (unsigned)MTLPixelFormatRG16Float == RDNA4SP_PF_RG16Float &&
               (unsigned)MTLPixelFormatRGBA8Unorm == RDNA4SP_PF_RGBA8Unorm &&
               (unsigned)MTLPixelFormatRGBA8Unorm_sRGB == RDNA4SP_PF_RGBA8Unorm_sRGB &&
               (unsigned)MTLPixelFormatRGBA8Snorm == RDNA4SP_PF_RGBA8Snorm &&
               (unsigned)MTLPixelFormatRGBA8Uint == RDNA4SP_PF_RGBA8Uint &&
               (unsigned)MTLPixelFormatRGBA8Sint == RDNA4SP_PF_RGBA8Sint &&
               (unsigned)MTLPixelFormatBGRA8Unorm == RDNA4SP_PF_BGRA8Unorm &&
               (unsigned)MTLPixelFormatBGRA8Unorm_sRGB == RDNA4SP_PF_BGRA8Unorm_sRGB &&
               (unsigned)MTLPixelFormatRGB10A2Unorm == RDNA4SP_PF_RGB10A2Unorm &&
               (unsigned)MTLPixelFormatRGB10A2Uint == RDNA4SP_PF_RGB10A2Uint &&
               (unsigned)MTLPixelFormatRG11B10Float == RDNA4SP_PF_RG11B10Float &&
               (unsigned)MTLPixelFormatRGB9E5Float == RDNA4SP_PF_RGB9E5Float &&
               (unsigned)MTLPixelFormatBGR10A2Unorm == RDNA4SP_PF_BGR10A2Unorm &&
               (unsigned)MTLPixelFormatRG32Uint == RDNA4SP_PF_RG32Uint &&
               (unsigned)MTLPixelFormatRG32Sint == RDNA4SP_PF_RG32Sint &&
               (unsigned)MTLPixelFormatRG32Float == RDNA4SP_PF_RG32Float &&
               (unsigned)MTLPixelFormatRGBA16Unorm == RDNA4SP_PF_RGBA16Unorm &&
               (unsigned)MTLPixelFormatRGBA16Snorm == RDNA4SP_PF_RGBA16Snorm &&
               (unsigned)MTLPixelFormatRGBA16Uint == RDNA4SP_PF_RGBA16Uint &&
               (unsigned)MTLPixelFormatRGBA16Sint == RDNA4SP_PF_RGBA16Sint &&
               (unsigned)MTLPixelFormatRGBA16Float == RDNA4SP_PF_RGBA16Float &&
               (unsigned)MTLPixelFormatRGBA32Uint == RDNA4SP_PF_RGBA32Uint &&
               (unsigned)MTLPixelFormatRGBA32Sint == RDNA4SP_PF_RGBA32Sint &&
               (unsigned)MTLPixelFormatRGBA32Float == RDNA4SP_PF_RGBA32Float &&
               (unsigned)MTLPixelFormatBC1_RGBA == RDNA4SP_PF_BC1_RGBA &&
               (unsigned)MTLPixelFormatBC1_RGBA_sRGB == RDNA4SP_PF_BC1_RGBA_sRGB &&
               (unsigned)MTLPixelFormatBC2_RGBA == RDNA4SP_PF_BC2_RGBA &&
               (unsigned)MTLPixelFormatBC2_RGBA_sRGB == RDNA4SP_PF_BC2_RGBA_sRGB &&
               (unsigned)MTLPixelFormatBC3_RGBA == RDNA4SP_PF_BC3_RGBA &&
               (unsigned)MTLPixelFormatBC3_RGBA_sRGB == RDNA4SP_PF_BC3_RGBA_sRGB &&
               (unsigned)MTLPixelFormatBC4_RUnorm == RDNA4SP_PF_BC4_RUnorm &&
               (unsigned)MTLPixelFormatBC4_RSnorm == RDNA4SP_PF_BC4_RSnorm &&
               (unsigned)MTLPixelFormatBC5_RGUnorm == RDNA4SP_PF_BC5_RGUnorm &&
               (unsigned)MTLPixelFormatBC5_RGSnorm == RDNA4SP_PF_BC5_RGSnorm &&
               (unsigned)MTLPixelFormatBC6H_RGBFloat == RDNA4SP_PF_BC6H_RGBFloat &&
               (unsigned)MTLPixelFormatBC6H_RGBUfloat == RDNA4SP_PF_BC6H_RGBUfloat &&
               (unsigned)MTLPixelFormatBC7_RGBAUnorm == RDNA4SP_PF_BC7_RGBAUnorm &&
               (unsigned)MTLPixelFormatBC7_RGBAUnorm_sRGB == RDNA4SP_PF_BC7_RGBAUnorm_sRGB &&
               (unsigned)MTLPixelFormatDepth16Unorm == RDNA4SP_PF_Depth16Unorm &&
               (unsigned)MTLPixelFormatDepth32Float == RDNA4SP_PF_Depth32Float &&
               (unsigned)MTLPixelFormatStencil8 == RDNA4SP_PF_Stencil8,
               "MTLPixelFormat sparse table values");

// Lazy Vulkan sparse entry point resolution (mirrors rdna4_counters_impl.inc).
typedef struct {
    PFN_vkQueueBindSparse queueBindSparse;
    PFN_vkGetImageSparseMemoryRequirements getImageSparseMemReqs;
    PFN_vkGetPhysicalDeviceSparseImageFormatProperties getPhysSparseImageFormatProps;
} rdna4_sparse_vk_fn_t;

static const rdna4_sparse_vk_fn_t *rdna4_sparse_vk_fns(void) {
    if (!RDNA4R.ok || !RDNA4R.dev) return NULL;
    static rdna4_sparse_vk_fn_t fns;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        if (vkGetDeviceProcAddr) {
            fns.queueBindSparse = (PFN_vkQueueBindSparse)vkGetDeviceProcAddr(RDNA4R.dev, "vkQueueBindSparse");
            fns.getImageSparseMemReqs = (PFN_vkGetImageSparseMemoryRequirements)vkGetDeviceProcAddr(RDNA4R.dev, "vkGetImageSparseMemoryRequirements");
        }
        if (rdna4_gipa && RDNA4R.inst) {
            if (!fns.queueBindSparse)
                fns.queueBindSparse = (PFN_vkQueueBindSparse)rdna4_gipa(RDNA4R.inst, "vkQueueBindSparse");
            if (!fns.getImageSparseMemReqs)
                fns.getImageSparseMemReqs = (PFN_vkGetImageSparseMemoryRequirements)rdna4_gipa(RDNA4R.inst, "vkGetImageSparseMemoryRequirements");
            fns.getPhysSparseImageFormatProps = (PFN_vkGetPhysicalDeviceSparseImageFormatProperties)
                rdna4_gipa(RDNA4R.inst, "vkGetPhysicalDeviceSparseImageFormatProperties");
        }
    });
    return &fns;
}

// Execute vkQueueBindSparse on RDNA4R.q with direct VkFence completion + bounded wait.
// Never holds RDNA4R.qlock while waiting. Sets *unresolvedOut = YES on timeout/device-lost without destroying in-flight handles.
static BOOL rdna4_sparse_submit_bind(const VkBindSparseInfo *baseInfo, NSError **err, BOOL *unresolvedOut) {
    if (unresolvedOut) *unresolvedOut = NO;
    if (!baseInfo) { if (err) *err = rdna4_err(140, @"NULL VkBindSparseInfo"); return NO; }
    if (!RDNA4R.ok || !RDNA4R.dev || !RDNA4R.q) {
        if (err) *err = rdna4_err(140, @"RADV device is not open for sparse bind");
        return NO;
    }
    const rdna4_sparse_vk_fn_t *vk = rdna4_sparse_vk_fns();
    if (!vk || !vk->queueBindSparse || !vkCreateFence || !vkDestroyFence || !vkWaitForFences) {
        if (err) *err = rdna4_err(140, @"Vulkan sparse bind or fence entry points unavailable");
        return NO;
    }
    if (atomic_load(&rdna4_submit_fault) != VK_SUCCESS) {
        if (err) *err = rdna4_err(53, @"earlier GPU submission is unresolved; sparse bind disabled");
        return NO;
    }

    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence = VK_NULL_HANDLE;
    VkResult vr = vkCreateFence(RDNA4R.dev, &fci, NULL, &fence);
    if (vr != VK_SUCCESS || fence == VK_NULL_HANDLE) {
        if (err) *err = rdna4_err(141, [NSString stringWithFormat:@"vkCreateFence(sparse) = %d", vr]);
        return NO;
    }

    VkBindSparseInfo bi = *baseInfo;
    bi.sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO;
    bi.waitSemaphoreCount = 0u;
    bi.pWaitSemaphores = NULL;
    bi.signalSemaphoreCount = 0u;
    bi.pSignalSemaphores = NULL;

    BOOL bindSubmitted = NO, deviceLost = NO;
    rdna4_qlock();
    vr = (VkResult)atomic_load(&rdna4_submit_fault);
    if (vr == VK_SUCCESS) {
        vr = vk->queueBindSparse(RDNA4R.q, 1u, &bi, fence);
        if (vr == VK_SUCCESS) {
            bindSubmitted = YES;
        } else if (vr == VK_ERROR_DEVICE_LOST) {
            deviceLost = YES;
            atomic_store(&rdna4_submit_fault, (int)vr);
        }
    }
    pthread_mutex_unlock(&RDNA4R.qlock);

    if (!bindSubmitted) {
        if (deviceLost) {
            // Unresolved GPU state on VK_ERROR_DEVICE_LOST: do not destroy fence or release backing memory.
            if (unresolvedOut) *unresolvedOut = YES;
            if (err) *err = rdna4_err(142, [NSString stringWithFormat:@"vkQueueBindSparse = %d", vr]);
        } else {
            vkDestroyFence(RDNA4R.dev, fence, NULL);
            if (err) *err = rdna4_err(142, [NSString stringWithFormat:@"vkQueueBindSparse = %d", vr]);
        }
        return NO;
    }

    rdna4_fence_wait_ctx waitCtx = { .fence = fence, .cbid = 0u, .t1 = rdna4_now() };
    int slices = 0;
    vr = (VkResult)rdna4fw_wait_bounded(rdna4_fence_wait_slice, &waitCtx, 0,
                                        RDNA4FW_MAX_SLICES, RDNA4FW_SLICE_NS,
                                        rdna4_fence_wait_pending, &slices);
    if (vr != VK_SUCCESS) {
        atomic_store(&rdna4_submit_fault, (int)vr);
        if (unresolvedOut) *unresolvedOut = YES;
        if (err) *err = rdna4_err(54, [NSString stringWithFormat:@"sparse bind fence wait = %d (%d slice(s))", vr, slices]);
        return NO;
    }

    vkDestroyFence(RDNA4R.dev, fence, NULL);
    return YES;
}

// ---------------------------------------------------------------------------------------------------------------
// RDNA4SparseBackingPool: manages 64 KiB physical tile chunks (<= 64 MiB non-composite VkDeviceMemory blocks)
// shared by both MTLHeapTypeSparse (RDNA4SparseHeap) and Private MTLHeapTypeAutomatic/Placement heaps.
// ---------------------------------------------------------------------------------------------------------------

@implementation RDNA4SparseBackingPool
- (instancetype)initWithCapacityBytes:(uint64_t)capacityBytes ownsAccounting:(BOOL)ownsAcct error:(NSError **)err {
    self = [super init];
    if (!self) return nil;
    if (!rdna4sp_pool_init(&_pool, capacityBytes)) {
        if (err) *err = rdna4_err(143, [NSString stringWithFormat:@"invalid sparse tile pool capacity %llu", (unsigned long long)capacityBytes]);
        return nil;
    }
    pthread_mutex_init(&_mu, NULL);
    _muInit = YES;
    _capacityBytes = capacityBytes;
    _ownsAccounting = ownsAcct;
    return self;
}

- (void)dealloc {
    if (_muInit) pthread_mutex_lock(&_mu);
    if (RDNA4R.ok && RDNA4R.dev && vkFreeMemory) {
        for (size_t i = 0; i < _pool.n_chunks; i++) {
            VkDeviceMemory mem = (VkDeviceMemory)(uintptr_t)_pool.chunks[i].mem_handle;
            if (mem != VK_NULL_HANDLE) vkFreeMemory(RDNA4R.dev, mem, NULL);
        }
    }
    rdna4sp_pool_destroy(&_pool);
    if (_ownsAccounting && _capacityBytes) {
        atomic_fetch_sub(&rdna4_heap_reserved, _capacityBytes);
        atomic_fetch_sub(&rdna4_alloc_total, _capacityBytes);
    }
    if (_muInit) {
        pthread_mutex_unlock(&_mu);
        pthread_mutex_destroy(&_mu);
    }
}

- (rdna4sp_tile_pool *)pool { return &_pool; }
- (pthread_mutex_t *)mutex { return &_mu; }

- (NSUInteger)capacityBytes { return (NSUInteger)_capacityBytes; }
- (NSUInteger)usedBytes {
    pthread_mutex_lock(&_mu);
    uint64_t u = (uint64_t)_pool.used_tiles * RDNA4SP_TILE_BYTES;
    pthread_mutex_unlock(&_mu);
    return (NSUInteger)u;
}
- (NSUInteger)availableBytes {
    pthread_mutex_lock(&_mu);
    uint64_t a = (uint64_t)rdna4sp_pool_total_available(&_pool) * RDNA4SP_TILE_BYTES;
    pthread_mutex_unlock(&_mu);
    return (NSUInteger)a;
}

// Caller MUST hold _mu. Lazily allocates <= 64 MiB single non-composite DEVICE_LOCAL chunks up to capacity.
- (BOOL)ensureFreeTilesLocked:(uint64_t)need memoryTypeBits:(uint32_t)mtBits error:(NSError **)err {
    if (rdna4sp_pool_free_backed(&_pool) >= need) return YES;
    if (rdna4sp_pool_total_available(&_pool) < need) {
        if (err) *err = rdna4_err(144, [NSString stringWithFormat:@"sparse heap out of tiles (need %llu, available %u)",
                                        (unsigned long long)need, rdna4sp_pool_total_available(&_pool)]);
        return NO;
    }
    if (!RDNA4R.ok || !RDNA4R.dev || !vkAllocateMemory || !vkFreeMemory) {
        if (err) *err = rdna4_err(144, @"RADV unavailable for sparse tile backing allocation");
        return NO;
    }
    const int mt = rdna4_find_mem(mtBits ? mtBits : UINT32_MAX, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt < 0) {
        if (err) *err = rdna4_err(144, @"no device-local memory type for sparse tile backing");
        return NO;
    }
    while (rdna4sp_pool_free_backed(&_pool) < need) {
        const uint32_t unbacked = rdna4sp_pool_unbacked(&_pool);
        if (!unbacked) {
            if (err) *err = rdna4_err(144, @"sparse heap backing capacity exhausted");
            return NO;
        }
        uint64_t missing = need - rdna4sp_pool_free_backed(&_pool);
        // Grow in >= 4 MiB (64-tile) steps when remaining capacity allows, capped at 64 MiB per BO chunk.
        uint64_t chunkTiles = missing < 64u ? 64u : missing;
        if (chunkTiles > unbacked) chunkTiles = unbacked;
        const uint64_t maxChunkTiles = RDNA4SP_MAX_CHUNK_BYTES / RDNA4SP_TILE_BYTES;
        if (chunkTiles > maxChunkTiles) chunkTiles = maxChunkTiles;
        const uint64_t chunkBytes = chunkTiles * RDNA4SP_TILE_BYTES;
        VkMemoryAllocateInfo ma = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = chunkBytes,
            .memoryTypeIndex = (uint32_t)mt,
        };
        VkDeviceMemory mem = VK_NULL_HANDLE;
        VkResult vr = vkAllocateMemory(RDNA4R.dev, &ma, NULL, &mem);
        if (vr != VK_SUCCESS || mem == VK_NULL_HANDLE) {
            if (err) *err = rdna4_err(144, [NSString stringWithFormat:@"vkAllocateMemory(sparse chunk %llu B) = %d",
                                            (unsigned long long)chunkBytes, vr]);
            return NO;
        }
        if (!rdna4sp_pool_add_chunk(&_pool, (uint64_t)(uintptr_t)mem, chunkBytes)) {
            vkFreeMemory(RDNA4R.dev, mem, NULL);
            if (err) *err = rdna4_err(144, @"sparse tile pool rejected new chunk");
            return NO;
        }
    }
    return YES;
}

- (BOOL)ensureFreeTiles:(uint64_t)need memoryTypeBits:(uint32_t)mtBits error:(NSError **)err {
    pthread_mutex_lock(&_mu);
    BOOL ok = [self ensureFreeTilesLocked:need memoryTypeBits:mtBits error:err];
    pthread_mutex_unlock(&_mu);
    return ok;
}
@end

@class RDNA4SparseTexture;
static RDNA4SparseBackingPool *rdna4_heap_sparse_pool(id heap, NSError **err);
static RDNA4SparseTexture *rdna4_sparse_texture_new(id dev, MTLTextureDescriptor *d, id heap, NSError **err);

// ---------------------------------------------------------------------------------------------------------------
// RDNA4SparseHeap: MTLHeap implementation for MTLHeapTypeSparse (64 KiB GFX12 PRT tile pool, Private only).
// ---------------------------------------------------------------------------------------------------------------

@implementation RDNA4SparseHeap
+ (void)initialize {
    if (self == [RDNA4SparseHeap class]) {
        static dispatch_once_t once;
        dispatch_once(&once, ^{
            const char *n[] = { "MTLHeap" };
            rdna4_add_protocols(self, n, 1, "RDNA4SparseHeap");
        });
    }
}
RDNA4_DNR(RDNA4SparseHeap)

- (instancetype)initWithDevice:(id)dev descriptor:(MTLHeapDescriptor *)d error:(NSError **)err {
    self = [super init];
    if (!self) return nil;
    if (!dev || !d) {
        if (err) *err = rdna4_err(145, @"sparse heap requires a valid device and descriptor");
        return nil;
    }
    if ((NSInteger)d.type != RDNA4SP_HEAP_SPARSE) {
        if (err) *err = rdna4_err(145, @"RDNA4SparseHeap requires MTLHeapTypeSparse");
        return nil;
    }
    if (d.storageMode != MTLStorageModePrivate) {
        if (err) *err = rdna4_err(145, @"sparse heaps require MTLStorageModePrivate");
        return nil;
    }
    if (!d.size) {
        if (err) *err = rdna4_err(145, @"sparse heap size must be non-zero");
        return nil;
    }
    if ((unsigned)d.hazardTrackingMode > 2u) {
        if (err) *err = rdna4_err(145, @"invalid hazardTrackingMode on sparse heap");
        return nil;
    }
    SEL spSel = sel_registerName("sparsePageSize");
    if ([d respondsToSelector:spSel]) {
        NSInteger psize = ((NSInteger (*)(id, SEL))objc_msgSend)(d, spSel);
        if (psize != RDNA4SP_PAGE_SIZE_DEFAULT && psize != RDNA4SP_PAGE_SIZE_16 &&
            psize != RDNA4SP_PAGE_SIZE_64) {
            if (err) *err = rdna4_err(145, [NSString stringWithFormat:@"unsupported MTLSparsePageSize %ld (GFX12 PRT uses 64 KiB tiles)", (long)psize]);
            return nil;
        }
        if (psize == RDNA4SP_PAGE_SIZE_16)
            RDNA4_ONCE("sparse heap: MTLSparsePageSize16 default descriptor mapped to GFX12 64 KiB physical tiles");
    }
    if (!rdna4_radv_open(err)) return nil;
    const rdna4dc_metal3_caps m3 = rdna4_metal3_caps();
    if (!rdna4dc_metal3_sparse_texture_ok(&m3)) {
        if (err) *err = rdna4_err(145, @"sparse textures are unavailable on this RADV/kernel configuration");
        return nil;
    }
    uint64_t req = (uint64_t)d.size;
    if (req > UINT64_MAX - (RDNA4SP_TILE_BYTES - 1u)) {
        if (err) *err = rdna4_err(145, @"sparse heap size overflow");
        return nil;
    }
    uint64_t aligned = (req + RDNA4SP_TILE_BYTES - 1u) & ~(RDNA4SP_TILE_BYTES - 1u);
    rdna4dc_caps caps = [(GarnetDevice *)dev rdna4MemoryCaps];
    if (aligned > caps.allocation_budget || !rdna4_heap_reserve_capacity(aligned, caps.allocation_budget)) {
        if (err) *err = rdna4_err(145, [NSString stringWithFormat:@"sparse heap size %llu exceeds VRAM budget %llu",
                                        (unsigned long long)aligned, (unsigned long long)caps.allocation_budget]);
        return nil;
    }
    atomic_fetch_add(&rdna4_alloc_total, aligned);
    RDNA4SparseBackingPool *pool = [[RDNA4SparseBackingPool alloc] initWithCapacityBytes:aligned ownsAccounting:YES error:err];
    if (!pool) {
        atomic_fetch_sub(&rdna4_heap_reserved, aligned);
        atomic_fetch_sub(&rdna4_alloc_total, aligned);
        return nil;
    }
    _dev = dev;
    _size = (NSUInteger)aligned;
    _cc = (unsigned)d.cpuCacheMode;
    _hz = d.hazardTrackingMode ? (unsigned)d.hazardTrackingMode : 1u;
    _purge = 2u;
    _pool = pool;
    RDNA4LOG("RDNA4SparseHeap %p: %lu bytes (%llu x 64 KiB tiles)", (__bridge void *)self,
             (unsigned long)_size, (unsigned long long)(aligned / RDNA4SP_TILE_BYTES));
    return self;
}

- (RDNA4SparseBackingPool *)rdna4SparseBackingPool { return _pool; }
- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)td {
    NSError *e = nil;
    RDNA4SparseTexture *t = rdna4_sparse_texture_new(_dev, td, self, &e);
    if (!t) RDNA4LOG("sparseHeap newTextureWithDescriptor NIL: %s", e.localizedDescription.UTF8String);
    return t;
}
- (id)newBufferWithLength:(NSUInteger)len options:(MTLResourceOptions)o {
    (void)len; (void)o;
    RDNA4_ONCE("sparseHeap newBufferWithLength:options: sparse heaps back sparse textures only; nil");
    return nil;
}
- (id)newBufferWithLength:(NSUInteger)l options:(MTLResourceOptions)o offset:(NSUInteger)off {
    (void)l; (void)o; (void)off; return nil;
}
- (id)newBufferWithLength:(NSUInteger)l options:(MTLResourceOptions)o atOffset:(NSUInteger)off {
    (void)l; (void)o; (void)off; return nil;
}
- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)off { (void)d; (void)off; return nil; }
- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)d atOffset:(NSUInteger)off { (void)d; (void)off; return nil; }
- (NSUInteger)maxAvailableSizeWithAlignment:(NSUInteger)al { (void)al; return [_pool availableBytes]; }
- (NSUInteger)setPurgeableState:(NSUInteger)st { NSUInteger p = _purge; if (st >= 2 && st <= 4) _purge = st; return p; }
- (id)device { return _dev; }
- (NSString *)label { return _lbl; }
- (void)setLabel:(NSString *)l { _lbl = [l copy]; }
- (NSUInteger)size { return _size; }
- (NSUInteger)usedSize { return [_pool usedBytes]; }
- (NSUInteger)currentAllocatedSize { return _size; }
- (NSUInteger)allocatedSize { return _size; }
- (MTLStorageMode)storageMode { return MTLStorageModePrivate; }
- (MTLCPUCacheMode)cpuCacheMode { return (MTLCPUCacheMode)_cc; }
- (MTLHazardTrackingMode)hazardTrackingMode { return (MTLHazardTrackingMode)_hz; }
- (NSUInteger)resourceOptions { return ((NSUInteger)MTLStorageModePrivate << 4) | _cc | ((NSUInteger)_hz << 8); }
- (NSUInteger)unfilteredResourceOptions { return [self resourceOptions]; }
- (MTLHeapType)type { return (MTLHeapType)RDNA4SP_HEAP_SPARSE; }
- (NSUInteger)protectionOptions { return 0; }
- (uint64_t)gpuAddress { return 0; }
- (uint64_t)memoryPoolId { return 0; }
- (NSString *)formattedDescription:(NSUInteger)indent {
    (void)indent;
    return [NSString stringWithFormat:@"<RDNA4SparseHeap: %p> size %lu used %lu label %@",
            (__bridge void *)self, (unsigned long)_size, (unsigned long)[self usedSize], _lbl];
}
@end

// Backing-pool helper for Private MTLHeapTypeAutomatic / MTLHeapTypePlacement heaps used for sparse tiles.
static const void *const rdna4_heap_sparse_pool_key = &rdna4_heap_sparse_pool_key;


@implementation RDNA4Heap (RDNA4SparseBacking)
- (RDNA4SparseBackingPool *)rdna4SparseBackingPoolWithError:(NSError **)err {
    if ([self storageMode] != MTLStorageModePrivate) {
        if (err) *err = rdna4_err(145, @"sparse tile backing requires a Private MTLHeap");
        return nil;
    }
    @synchronized (self) {
        RDNA4SparseBackingPool *existing = objc_getAssociatedObject(self, rdna4_heap_sparse_pool_key);
        if (existing) return existing;
        uint64_t cap = (uint64_t)[self size] & ~(RDNA4SP_TILE_BYTES - 1u);
        if (!cap) {
            if (err) *err = rdna4_err(145, @"heap size is smaller than one 64 KiB sparse tile");
            return nil;
        }
        // RDNA4Heap already accounted for _size in rdna4_heap_reserved / rdna4_alloc_total.
        RDNA4SparseBackingPool *created = [[RDNA4SparseBackingPool alloc] initWithCapacityBytes:cap ownsAccounting:NO error:err];
        if (created)
            objc_setAssociatedObject(self, rdna4_heap_sparse_pool_key, created, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        return created;
    }
}
@end

static RDNA4SparseBackingPool *rdna4_heap_sparse_pool(id heap, NSError **err) {
    if (!heap) {
        if (err) *err = rdna4_err(145, @"sparse texture requires a backing MTLHeap");
        return nil;
    }
    if ([heap isKindOfClass:[RDNA4SparseHeap class]])
        return [(RDNA4SparseHeap *)heap rdna4SparseBackingPool];
    if ([heap isKindOfClass:[RDNA4Heap class]])
        return [(RDNA4Heap *)heap rdna4SparseBackingPoolWithError:err];
    if (err) *err = rdna4_err(145, @"unsupported heap class for sparse tile backing");
    return nil;
}

GARNET_INTERNAL __attribute__((unused)) id rdna4_sparse_heap_new(id dev, MTLHeapDescriptor *d, NSError **err) {
    NSError *localErr = nil;
    RDNA4SparseHeap *h = [[RDNA4SparseHeap alloc] initWithDevice:dev descriptor:d error:&localErr];
    if (!h) {
        RDNA4LOG("newHeapWithDescriptor(Sparse) NIL: %s", localErr.localizedDescription.UTF8String);
        if (err) *err = localErr;
    }
    return h;
}

// ---------------------------------------------------------------------------------------------------------------
// RDNA4SparseTexture: sparse subclass of RDNA4Texture (VK_IMAGE_CREATE_SPARSE_BINDING_BIT | SPARSE_RESIDENCY_BIT).
// ---------------------------------------------------------------------------------------------------------------

@implementation RDNA4SparseTexture
- (instancetype)initSparseWithDevice:(id)dev descriptor:(MTLTextureDescriptor *)d heap:(id)heap error:(NSError **)err {
    self = [super init];
    if (!self) return nil;
    if (!dev || !d || !heap) {
        if (err) *err = rdna4_err(146, @"sparse texture requires device, descriptor, and backing heap");
        return nil;
    }
    if (d.storageMode != MTLStorageModePrivate) {
        if (err) *err = rdna4_err(146, @"sparse textures require MTLStorageModePrivate");
        return nil;
    }
    if (!rdna4_radv_open(err)) return nil;
    const rdna4dc_metal3_caps m3 = rdna4_metal3_caps();
    const NSUInteger sc = d.sampleCount ? d.sampleCount : 1u;
    if (!rdna4sp_caps_support_texture(&m3, (uint64_t)d.textureType, (uint64_t)sc)) {
        if (err) *err = rdna4_err(146, [NSString stringWithFormat:@"sparse texture type %lu samples %lu not supported by RADV/kernel",
                                        (unsigned long)d.textureType, (unsigned long)sc]);
        return nil;
    }
    if (!rdna4sp_fmt_lookup((uint64_t)d.pixelFormat)) {
        if (err) *err = rdna4_err(146, [NSString stringWithFormat:@"pixel format %lu is not supported for sparse textures",
                                        (unsigned long)d.pixelFormat]);
        return nil;
    }
    RDNA4SparseBackingPool *pool = rdna4_heap_sparse_pool(heap, err);
    if (!pool) return nil;

    const rdna4_sparse_vk_fn_t *vk = rdna4_sparse_vk_fns();
    if (!vk || !vk->queueBindSparse || !vk->getImageSparseMemReqs) {
        if (err) *err = rdna4_err(146, @"Vulkan sparse image entry points are unavailable");
        return nil;
    }

    rdna4_heap_image_plan plan;
    if (!rdna4_heap_image_plan_make(d, NO, &plan, err)) return nil;

    VkImageCreateInfo ic = plan.create;
    ic.flags |= VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT;
    if (m3.sparse_residency_aliased) ic.flags |= VK_IMAGE_CREATE_SPARSE_ALIASED_BIT;

    if (vkGetPhysicalDeviceImageFormatProperties) {
        VkImageFormatProperties ifp = {0};
        VkResult qr = vkGetPhysicalDeviceImageFormatProperties(RDNA4R.pd, ic.format, ic.imageType,
                                                               ic.tiling, ic.usage, ic.flags, &ifp);
        if (qr != VK_SUCCESS && plan.canRT && !(d.usage & MTLTextureUsageRenderTarget)) {
            ic.usage &= ~(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
                          VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
            qr = vkGetPhysicalDeviceImageFormatProperties(RDNA4R.pd, ic.format, ic.imageType,
                                                          ic.tiling, ic.usage, ic.flags, &ifp);
        }
        if (qr != VK_SUCCESS) {
            if (err) *err = rdna4_err(146, [NSString stringWithFormat:@"RADV refuses sparse image format %d type %d usage 0x%x (vk %d)",
                                            ic.format, ic.imageType, ic.usage, qr]);
            return nil;
        }
    }

    _dev = dev;
    _w = d.width;
    _h = d.height;
    _pf = d.pixelFormat;
    _fmt = plan.fmt;
    _usage = d.usage;
    _layout = VK_IMAGE_LAYOUT_UNDEFINED;
    _swz = d.swizzle;
    _opts = ((NSUInteger)d.storageMode << 4) | (NSUInteger)d.cpuCacheMode | ((NSUInteger)d.hazardTrackingMode << 8);
    _is1D = plan.is1D;
    _arr1D = plan.arr1D;
    _layers = plan.td.layersIvar;
    _tdk = plan.td.kind;
    _is3D = plan.is3D;
    _levels = plan.levels;
    _depth = plan.depth;
    _samples = plan.samples;
    _aspects = plan.aspects;
    _imageFlags = ic.flags;

    VkResult vr = vkCreateImage(RDNA4R.dev, &ic, NULL, &_img);
    if (vr != VK_SUCCESS || _img == VK_NULL_HANDLE) {
        _img = VK_NULL_HANDLE;
        if (err) *err = rdna4_err(147, [NSString stringWithFormat:@"vkCreateImage(sparse) = %d", vr]);
        return nil;
    }

    VkMemoryRequirements mr = {0};
    vkGetImageMemoryRequirements(RDNA4R.dev, _img, &mr);
    _spMemTypeBits = mr.memoryTypeBits;

    uint32_t reqCount = 0u;
    vk->getImageSparseMemReqs(RDNA4R.dev, _img, &reqCount, NULL);
    if (reqCount == 0u || reqCount > 8u) {
        vkDestroyImage(RDNA4R.dev, _img, NULL);
        _img = VK_NULL_HANDLE;
        if (err) *err = rdna4_err(147, [NSString stringWithFormat:@"vkGetImageSparseMemoryRequirements returned count %u", reqCount]);
        return nil;
    }
    VkSparseImageMemoryRequirements reqs[8];
    memset(reqs, 0, sizeof reqs);
    vk->getImageSparseMemReqs(RDNA4R.dev, _img, &reqCount, reqs);

    const uint32_t wantAspect = plan.isDS ? rdna4dp_view_aspect((unsigned)plan.aspects) : VK_IMAGE_ASPECT_COLOR_BIT;
    const VkSparseImageMemoryRequirements *chosen = NULL;
    for (uint32_t i = 0; i < reqCount; i++) {
        if (reqs[i].formatProperties.aspectMask & wantAspect) { chosen = &reqs[i]; break; }
    }
    if (!chosen) {
        vkDestroyImage(RDNA4R.dev, _img, NULL);
        _img = VK_NULL_HANDLE;
        if (err) *err = rdna4_err(147, @"no matching aspect in VkSparseImageMemoryRequirements");
        return nil;
    }

    rdna4sp_image_layout spLay;
    if (!rdna4sp_compute_layout((uint64_t)d.textureType, (uint64_t)d.pixelFormat,
                                (uint32_t)d.width, (uint32_t)d.height, (uint32_t)plan.depth,
                                (uint32_t)plan.levels, plan.td.layers, plan.td.samples,
                                (chosen->formatProperties.flags & VK_SPARSE_IMAGE_FORMAT_ALIGNED_MIP_SIZE_BIT) != 0,
                                (chosen->formatProperties.flags & VK_SPARSE_IMAGE_FORMAT_SINGLE_MIPTAIL_BIT) != 0,
                                &spLay) ||
        !rdna4sp_layout_adopt_vk_reqs(&spLay,
                                      chosen->formatProperties.imageGranularity.width,
                                      chosen->formatProperties.imageGranularity.height,
                                      chosen->formatProperties.imageGranularity.depth,
                                      wantAspect,
                                      chosen->imageMipTailFirstLod,
                                      (uint64_t)chosen->imageMipTailSize,
                                      (uint64_t)chosen->imageMipTailOffset,
                                      (uint64_t)chosen->imageMipTailStride,
                                      (chosen->formatProperties.flags & VK_SPARSE_IMAGE_FORMAT_SINGLE_MIPTAIL_BIT) != 0) ||
        !rdna4sp_tex_init(&_spState, &spLay)) {
        vkDestroyImage(RDNA4R.dev, _img, NULL);
        _img = VK_NULL_HANDLE;
        if (err) *err = rdna4_err(147, @"failed to initialize sparse texture residency table");
        return nil;
    }

    VkComponentMapping cm = rdna4_view_map(_fmt, _swz);
    _viewSwz = !rdna4_is_identity_map(&cm);
    VkImageViewCreateInfo vc = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = _img,
        .viewType = (VkImageViewType)plan.td.viewType,
        .format = _fmt->vk,
        .components = cm,
        .subresourceRange = { wantAspect, 0u, (uint32_t)_levels, 0u, plan.td.layers },
    };
    vr = vkCreateImageView(RDNA4R.dev, &vc, NULL, &_view);
    if (vr != VK_SUCCESS || _view == VK_NULL_HANDLE) {
        _view = VK_NULL_HANDLE;
        rdna4sp_tex_destroy(&_spState);
        vkDestroyImage(RDNA4R.dev, _img, NULL);
        _img = VK_NULL_HANDLE;
        if (err) *err = rdna4_err(148, [NSString stringWithFormat:@"vkCreateImageView(sparse) = %d", vr]);
        return nil;
    }

    pthread_mutex_init(&_spMu, NULL);
    _spMuInit = YES;
    _spHeap = heap;
    _spPool = pool;
    RDNA4LOGR("RDNA4SparseTexture %p: %lux%lux%lu pf %lu mips %lu layers %u tile %llux%llux%llu firstTail %u tailBytes %llu",
              (__bridge void *)self, (unsigned long)_w, (unsigned long)_h, (unsigned long)_depth,
              (unsigned long)_pf, (unsigned long)_levels, plan.td.layers,
              (unsigned long long)_spState.layout.granularity.width,
              (unsigned long long)_spState.layout.granularity.height,
              (unsigned long long)_spState.layout.granularity.depth,
              _spState.layout.mip_tail_first_lod, (unsigned long long)_spState.layout.mip_tail_size);
    return self;
}

- (void)dealloc {
    // Destroy image views & sparse VkImage FIRST so RADV tears down the PRT VA range
    // before physical 64 KiB slots are returned to _spPool for reuse.
    if (RDNA4R.ok && RDNA4R.dev) {
        for (NSNumber *lvv in _lvViews.allValues)
            vkDestroyImageView(RDNA4R.dev, (VkImageView)(uintptr_t)lvv.unsignedLongLongValue, NULL);
        [_lvViews removeAllObjects];
        if (_view != VK_NULL_HANDLE) { vkDestroyImageView(RDNA4R.dev, _view, NULL); _view = VK_NULL_HANDLE; }
        if (_img != VK_NULL_HANDLE) { vkDestroyImage(RDNA4R.dev, _img, NULL); _img = VK_NULL_HANDLE; }
    }
    if (_spMuInit) pthread_mutex_lock(&_spMu);
    if (_spPool) {
        pthread_mutex_t *pmu = [_spPool mutex];
        pthread_mutex_lock(pmu);
        rdna4sp_tex_release_all_slots(&_spState, [_spPool pool]);
        pthread_mutex_unlock(pmu);
    } else {
        rdna4sp_tex_release_all_slots(&_spState, NULL);
    }
    rdna4sp_tex_destroy(&_spState);
    if (_spMuInit) {
        pthread_mutex_unlock(&_spMu);
        pthread_mutex_destroy(&_spMu);
    }
}

- (BOOL)isSparse { return YES; }
- (NSUInteger)firstMipmapInTail { return (NSUInteger)_spState.layout.mip_tail_first_lod; }
- (NSUInteger)tailSizeInBytes { return (NSUInteger)_spState.layout.mip_tail_size; }
- (NSUInteger)allocatedSize {
    if (!_spMuInit) return 0;
    pthread_mutex_lock(&_spMu);
    uint64_t bytes = _spState.mapped_tile_count * RDNA4SP_TILE_BYTES;
    pthread_mutex_unlock(&_spMu);
    return (NSUInteger)bytes;
}
- (id)heap { return _spHeap; }
- (NSUInteger)heapOffset { return 0; }
- (BOOL)isAliasable { return NO; }
- (void)makeAliasable {}
- (const rdna4sp_texture_state *)rdna4SparseState { return &_spState; }

- (void)replaceRegion:(MTLRegion)r mipmapLevel:(NSUInteger)lvl slice:(NSUInteger)slice
            withBytes:(const void *)bytes bytesPerRow:(NSUInteger)bpr bytesPerImage:(NSUInteger)bpi {
    (void)r; (void)lvl; (void)slice; (void)bytes; (void)bpr; (void)bpi;
    RDNA4LOG("replaceRegion: refused on Private sparse texture %p", (__bridge void *)self);
}
- (void)getBytes:(void *)dst bytesPerRow:(NSUInteger)bpr bytesPerImage:(NSUInteger)bpi
      fromRegion:(MTLRegion)r mipmapLevel:(NSUInteger)lvl slice:(NSUInteger)slice {
    (void)dst; (void)bpr; (void)bpi; (void)r; (void)lvl; (void)slice;
    RDNA4LOG("getBytes: refused on Private sparse texture %p", (__bridge void *)self);
}

- (BOOL)rdna4ValidateRegions:(const rdna4sp_region *)regions mipLevels:(const uint64_t *)mips
                      slices:(const uint64_t *)slices count:(size_t)count
                        mode:(MTLSparseTextureMappingMode)mode error:(NSError **)err {
    const char *why = NULL;
    uint64_t needed = 0u;
    pthread_mutex_lock(&_spMu);
    int rc = rdna4sp_count_needed_tiles(&_spState, (uint32_t)mode, regions, mips, slices, count, &needed, &why);
    pthread_mutex_unlock(&_spMu);
    if (rc != RDNA4SP_OK) {
        if (err) *err = rdna4_err(149, [NSString stringWithUTF8String:why ?: "invalid sparse mapping region"]);
        return NO;
    }
    return YES;
}

- (BOOL)rdna4ExecuteSparseMode:(MTLSparseTextureMappingMode)mode
                       regions:(const rdna4sp_region *)regions
                     mipLevels:(const uint64_t *)mips
                        slices:(const uint64_t *)slices
                         count:(size_t)count
                         error:(NSError **)err {
    if (count == 0u) return YES;
    if (!_spPool || _img == VK_NULL_HANDLE) {
        if (err) *err = rdna4_err(149, @"sparse texture has no live Vulkan image or backing pool");
        return NO;
    }

    rdna4sp_bind_plan plan;
    rdna4sp_plan_init(&plan);
    const char *why = NULL;
    pthread_mutex_t *pmu = [_spPool mutex];

    pthread_mutex_lock(&_spMu);
    uint64_t need = 0u;
    int rc = rdna4sp_count_needed_tiles(&_spState, (uint32_t)mode, regions, mips, slices, count, &need, &why);
    if (rc != RDNA4SP_OK) {
        pthread_mutex_unlock(&_spMu);
        if (err) *err = rdna4_err(149, [NSString stringWithUTF8String:why ?: "invalid sparse region"]);
        return NO;
    }
    pthread_mutex_lock(pmu);
    if (mode == MTLSparseTextureMappingModeMap && need > 0u &&
        ![_spPool ensureFreeTilesLocked:need memoryTypeBits:_spMemTypeBits error:err]) {
        pthread_mutex_unlock(pmu);
        pthread_mutex_unlock(&_spMu);
        return NO;
    }
    rc = rdna4sp_plan_prepare(&plan, &_spState, [_spPool pool], (uint32_t)mode,
                              regions, mips, slices, count, &why);
    if (rc != RDNA4SP_OK) {
        pthread_mutex_unlock(pmu);
        pthread_mutex_unlock(&_spMu);
        rdna4sp_plan_destroy(&plan);
        if (err) *err = rdna4_err(149, [NSString stringWithUTF8String:why ?: "sparse bind planning failed"]);
        return NO;
    }
    if (plan.n_image_ops == 0u && plan.n_opaque_ops == 0u) {
        rdna4sp_plan_commit(&plan, [_spPool pool]);
        pthread_mutex_unlock(pmu);
        pthread_mutex_unlock(&_spMu);
        rdna4sp_plan_destroy(&plan);
        return YES;
    }
    // Release pool mutex before submitting/waiting on GPU; _spMu stays held to serialize per-texture binds.
    pthread_mutex_unlock(pmu);

    VkSparseImageMemoryBind *vkImgBinds = NULL;
    VkSparseMemoryBind *vkOpaqueBinds = NULL;
    BOOL ok = YES;
    if (plan.n_image_ops > 0u) {
        vkImgBinds = (VkSparseImageMemoryBind *)malloc(plan.n_image_ops * sizeof(*vkImgBinds));
        if (!vkImgBinds) ok = NO;
        else {
            for (size_t i = 0; i < plan.n_image_ops; i++)
                vkImgBinds[i] = rdna4sp_to_vk_image_bind(&plan.image_ops[i]);
        }
    }
    if (ok && plan.n_opaque_ops > 0u) {
        vkOpaqueBinds = (VkSparseMemoryBind *)malloc(plan.n_opaque_ops * sizeof(*vkOpaqueBinds));
        if (!vkOpaqueBinds) ok = NO;
        else {
            for (size_t i = 0; i < plan.n_opaque_ops; i++)
                vkOpaqueBinds[i] = rdna4sp_to_vk_opaque_bind(&plan.opaque_ops[i]);
        }
    }

    BOOL unresolved = NO;
    if (!ok) {
        if (err) *err = rdna4_err(149, @"out of host memory building VkSparseImageMemoryBind arrays");
    } else {
        VkSparseImageMemoryBindInfo imgBindInfo = {
            .image = _img,
            .bindCount = (uint32_t)plan.n_image_ops,
            .pBinds = vkImgBinds,
        };
        VkSparseImageOpaqueMemoryBindInfo opaqueBindInfo = {
            .image = _img,
            .bindCount = (uint32_t)plan.n_opaque_ops,
            .pBinds = vkOpaqueBinds,
        };
        VkBindSparseInfo bi = rdna4sp_make_bind_sparse_info(
            plan.n_image_ops ? &imgBindInfo : NULL,
            plan.n_opaque_ops ? &opaqueBindInfo : NULL,
            0u, NULL, 0u, NULL);
        ok = rdna4_sparse_submit_bind(&bi, err, &unresolved);
    }

    free(vkImgBinds);
    free(vkOpaqueBinds);

    pthread_mutex_lock(pmu);
    if (ok) {
        rdna4sp_plan_commit(&plan, [_spPool pool]);
    } else if (!unresolved) {
        rdna4sp_plan_rollback(&plan, &_spState, [_spPool pool]);
    } else {
        // Keep texture & pool retained on unresolved GPU fault so backing memory cannot be freed under flight.
        (void)CFBridgingRetain(self);
        (void)CFBridgingRetain(_spPool);
        plan.active = 0;
    }
    pthread_mutex_unlock(pmu);
    pthread_mutex_unlock(&_spMu);

    rdna4sp_plan_destroy(&plan);
    return ok;
}
@end

static RDNA4SparseTexture *rdna4_sparse_texture_new(id dev, MTLTextureDescriptor *d, id heap, NSError **err) {
    return [[RDNA4SparseTexture alloc] initSparseWithDevice:dev descriptor:d heap:heap error:err];
}

// ---------------------------------------------------------------------------------------------------------------
// RDNA4ResourceStateEncoder: MTLResourceStateCommandEncoder implementation backed by ordered vkQueueBindSparse.
// ---------------------------------------------------------------------------------------------------------------

@implementation RDNA4SparsePendingOp
- (void)dealloc {
    free(regions);
    free(mips);
    free(slices);
}
@end


@implementation RDNA4ResourceStateEncoder
+ (void)initialize {
    if (self == [RDNA4ResourceStateEncoder class]) {
        static dispatch_once_t once;
        dispatch_once(&once, ^{
            const char *n[] = { "MTLResourceStateCommandEncoder", "MTLCommandEncoder" };
            rdna4_add_protocols(self, n, 2, "RDNA4ResourceStateEncoder");
        });
    }
}
RDNA4_DNR(RDNA4ResourceStateEncoder)

- (instancetype)initWithCommandBuffer:(id)cb descriptor:(id)desc {
    (void)desc;
    self = [super initWithCommandBuffer:cb];
    if (!self) return nil;
    _cb = cb;
    _pendingOps = [NSMutableArray array];
    RDNA4LOGR("RDNA4ResourceStateEncoder %p", (__bridge void *)self);
    return self;
}

- (id)device { return [_cb device]; }
- (NSString *)label { return _lbl; }
- (void)setLabel:(NSString *)l { _lbl = [l copy]; }
- (NSUInteger)getType { return 4; }
- (void)insertDebugSignpost:(NSString *)s { (void)s; }
- (void)pushDebugGroup:(NSString *)s { (void)s; }
- (void)popDebugGroup {}

// Flush queued sparse mapping ops as one ordered command-buffer host action so preceding GPU
// segments complete before vkQueueBindSparse and following segments observe the new PTEs.
- (void)rdna4FlushPendingOps {
    if (!_pendingOps.count || [_cb rdna4Error]) {
        [_pendingOps removeAllObjects];
        return;
    }
    NSArray<RDNA4SparsePendingOp *> *batch = [_pendingOps copy];
    [_pendingOps removeAllObjects];
    NSError *queueErr = nil;
    if (![_cb rdna4EncodeOrderedHostAction:^BOOL(NSError **actionErr) {
        for (RDNA4SparsePendingOp *op in batch) {
            if (!op->indirect) {
                if (![op->tex rdna4ExecuteSparseMode:op->mode regions:op->regions
                                           mipLevels:op->mips slices:op->slices
                                               count:op->count error:actionErr])
                    return NO;
                continue;
            }
            rdna4sp_map_indirect_args rawArgs;
            memset(&rawArgs, 0, sizeof rawArgs);
            if ([op->indirectBuf contents]) {
                memcpy(&rawArgs, (const uint8_t *)[op->indirectBuf contents] + op->indirectOffset, sizeof rawArgs);
            } else {
                VkDeviceSize srcOff = 0;
                if (__builtin_add_overflow([op->indirectBuf rdna4VkOffset], (VkDeviceSize)op->indirectOffset, &srcOff)) {
                    if (actionErr) *actionErr = rdna4_err(150, @"sparse indirect buffer Vulkan offset overflow");
                    return NO;
                }
                RDNA4Buffer *stg = [[RDNA4Buffer alloc] initWithDevice:[op->indirectBuf device]
                                                                length:sizeof(rawArgs)
                                                               options:MTLResourceStorageModeShared
                                                                 error:actionErr];
                if (!stg || ![stg contents]) {
                    if (actionErr && !*actionErr)
                        *actionErr = rdna4_err(150, @"sparse indirect readback staging buffer allocation failed");
                    return NO;
                }
                BOOL unresolved = NO;
                BOOL copyOk = rdna4_oneshot(actionErr, ^(VkCommandBuffer cmd) {
                    VkBufferCopy bc = {
                        .srcOffset = srcOff,
                        .dstOffset = [stg rdna4VkOffset],
                        .size = sizeof(rawArgs),
                    };
                    vkCmdCopyBuffer(cmd, [op->indirectBuf vkBuffer], [stg vkBuffer], 1u, &bc);
                    VkMemoryBarrier mb = {
                        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
                    };
                    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                                         0u, 1u, &mb, 0u, NULL, 0u, NULL);
                }, &unresolved);
                if (!copyOk) {
                    if (unresolved) {
                        (void)CFBridgingRetain(stg);
                        (void)CFBridgingRetain(op->indirectBuf);
                    }
                    return NO;
                }
                memcpy(&rawArgs, [stg contents], sizeof rawArgs);
            }
            rdna4sp_region reg;
            uint64_t mip = 0u, slice = 0u;
            const char *why = NULL;
            int rc = rdna4sp_validate_indirect_args([op->tex rdna4SparseState], (uint32_t)op->mode,
                                                    &rawArgs, &reg, &mip, &slice, &why);
            if (rc != RDNA4SP_OK) {
                if (actionErr) *actionErr = rdna4_err(150, [NSString stringWithUTF8String:why ?: "invalid indirect sparse mapping arguments"]);
                return NO;
            }
            if (![op->tex rdna4ExecuteSparseMode:op->mode regions:&reg mipLevels:&mip slices:&slice count:1u error:actionErr])
                return NO;
        }
        return YES;
    } error:&queueErr]) {
        if (![_cb rdna4Error])
            [_cb rdna4Fail:queueErr.localizedDescription ?: @"failed to enqueue sparse bind action"];
    }
}

- (void)updateTextureMappings:(id<MTLTexture>)texture
                         mode:(MTLSparseTextureMappingMode)mode
                      regions:(const MTLRegion *)regions
                    mipLevels:(const NSUInteger *)mipLevels
                       slices:(const NSUInteger *)slices
                   numRegions:(NSUInteger)numRegions {
    if (_ended) { [_cb rdna4Fail:@"updateTextureMappings after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    if (![texture isKindOfClass:[RDNA4SparseTexture class]] || [(RDNA4Texture *)texture device] != [_cb device] || ![texture isSparse]) {
        [_cb rdna4Fail:@"updateTextureMappings requires a sparse texture from this device"];
        return;
    }
    if (mode != MTLSparseTextureMappingModeMap && mode != MTLSparseTextureMappingModeUnmap) {
        [_cb rdna4Fail:@"updateTextureMappings: invalid MTLSparseTextureMappingMode"];
        return;
    }
    if (numRegions == 0u) return;
    if (numRegions > RDNA4SP_MAX_BATCH_REGIONS) {
        [_cb rdna4Fail:@"updateTextureMappings: numRegions exceeds maximum batch limit"];
        return;
    }
    if (!regions || !mipLevels || !slices) {
        [_cb rdna4Fail:@"updateTextureMappings: NULL regions, mipLevels, or slices pointer"];
        return;
    }

    RDNA4SparseTexture *stex = (RDNA4SparseTexture *)texture;
    RDNA4SparsePendingOp *op = [RDNA4SparsePendingOp new];
    op->tex = stex;
    op->mode = mode;
    op->count = (size_t)numRegions;
    op->regions = (rdna4sp_region *)malloc(op->count * sizeof(*op->regions));
    op->mips = (uint64_t *)malloc(op->count * sizeof(*op->mips));
    op->slices = (uint64_t *)malloc(op->count * sizeof(*op->slices));
    if (!op->regions || !op->mips || !op->slices) {
        [_cb rdna4Fail:@"updateTextureMappings: out of host memory"];
        return;
    }
    for (size_t i = 0; i < op->count; i++) {
        op->regions[i] = (rdna4sp_region){
            .origin = { (uint64_t)regions[i].origin.x, (uint64_t)regions[i].origin.y, (uint64_t)regions[i].origin.z },
            .size   = { (uint64_t)regions[i].size.width, (uint64_t)regions[i].size.height, (uint64_t)regions[i].size.depth },
        };
        op->mips[i] = (uint64_t)mipLevels[i];
        op->slices[i] = (uint64_t)slices[i];
    }

    NSError *valErr = nil;
    if (![stex rdna4ValidateRegions:op->regions mipLevels:op->mips slices:op->slices count:op->count mode:mode error:&valErr]) {
        [_cb rdna4Fail:valErr.localizedDescription ?: @"updateTextureMappings: region validation failed"];
        return;
    }
    [_cb rdna4RetainEncoded:stex];
    if ([stex heap]) [_cb rdna4RetainEncoded:[stex heap]];
    [_pendingOps addObject:op];
}

- (void)updateTextureMapping:(id<MTLTexture>)texture
                        mode:(MTLSparseTextureMappingMode)mode
                      region:(MTLRegion)region
                    mipLevel:(NSUInteger)mipLevel
                       slice:(NSUInteger)slice {
    [self updateTextureMappings:texture mode:mode regions:&region mipLevels:&mipLevel slices:&slice numRegions:1u];
}

- (void)updateTextureMapping:(id<MTLTexture>)texture
                        mode:(MTLSparseTextureMappingMode)mode
              indirectBuffer:(id<MTLBuffer>)indirectBuffer
        indirectBufferOffset:(NSUInteger)indirectBufferOffset {
    if (_ended) { [_cb rdna4Fail:@"updateTextureMapping(indirect) after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    if (![texture isKindOfClass:[RDNA4SparseTexture class]] || [(RDNA4Texture *)texture device] != [_cb device] || ![texture isSparse]) {
        [_cb rdna4Fail:@"updateTextureMapping(indirect) requires a sparse texture from this device"];
        return;
    }
    if (mode != MTLSparseTextureMappingModeMap && mode != MTLSparseTextureMappingModeUnmap) {
        [_cb rdna4Fail:@"updateTextureMapping(indirect): invalid MTLSparseTextureMappingMode"];
        return;
    }
    if (![indirectBuffer isKindOfClass:[RDNA4Buffer class]] ||
        [(RDNA4Buffer *)indirectBuffer device] != [_cb device] ||
        [(RDNA4Buffer *)indirectBuffer isAliasable]) {
        [_cb rdna4Fail:@"updateTextureMapping(indirect) requires a non-aliasable RDNA4Buffer from this device"];
        return;
    }
    RDNA4Buffer *buf = (RDNA4Buffer *)indirectBuffer;
    if ((indirectBufferOffset & 3u) != 0u ||
        !rdna4_buffer_range([buf length], indirectBufferOffset, sizeof(rdna4sp_map_indirect_args))) {
        [_cb rdna4Fail:@"updateTextureMapping(indirect): indirectBufferOffset misaligned or out of bounds"];
        return;
    }
    RDNA4SparseTexture *stex = (RDNA4SparseTexture *)texture;
    RDNA4SparsePendingOp *op = [RDNA4SparsePendingOp new];
    op->tex = stex;
    op->mode = mode;
    op->indirect = YES;
    op->indirectBuf = buf;
    op->indirectOffset = indirectBufferOffset;
    [_cb rdna4RetainEncoded:stex];
    if ([stex heap]) [_cb rdna4RetainEncoded:[stex heap]];
    [_cb rdna4RetainEncoded:buf];
    [_pendingOps addObject:op];
}

- (void)updateFence:(id<MTLFence>)fence {
    if (_ended) { [_cb rdna4Fail:@"resourceState updateFence after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    [self rdna4FlushPendingOps];
    if ([_cb rdna4Error]) return;
    [_cb rdna4UpdateFence:fence];
}

- (void)waitForFence:(id<MTLFence>)fence {
    if (_ended) { [_cb rdna4Fail:@"resourceState waitForFence after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    [self rdna4FlushPendingOps];
    if ([_cb rdna4Error]) return;
    [_cb rdna4WaitForFence:fence];
}

- (void)endEncoding {
    if (_ended) return;
    _ended = YES;
    [self rdna4FlushPendingOps];
    RDNA4LOGR("RDNA4ResourceStateEncoder endEncoding");
    [super endEncoding];
}
@end

GARNET_INTERNAL __attribute__((unused)) id rdna4_cb_new_resource_state_encoder(RDNA4CommandBuffer *cb, id desc) {
    if (!cb) return nil;
    RDNA4LOGR("cb resourceStateCommandEncoder");
    return [[RDNA4ResourceStateEncoder alloc] initWithCommandBuffer:cb descriptor:desc];
}

// ---------------------------------------------------------------------------------------------------------------
// MTLDevice sparse query & coordinate conversion helpers (called by GarnetDevice in Wave 3).
// Never opens RADV unexpectedly and NEVER claims MTLGPUFamilyApple6.
// ---------------------------------------------------------------------------------------------------------------
GARNET_INTERNAL __attribute__((unused)) NSUInteger rdna4_device_sparse_tile_size_in_bytes(void) {
    const rdna4dc_metal3_caps m3 = rdna4_metal3_caps();
    return (NSUInteger)rdna4sp_tile_size_in_bytes(&m3);
}

GARNET_INTERNAL __attribute__((unused)) NSUInteger rdna4_device_sparse_tile_size_for_page_size(NSInteger sparsePageSize) {
    const rdna4dc_metal3_caps m3 = rdna4_metal3_caps();
    return (NSUInteger)rdna4sp_tile_size_in_bytes_for_page_size(&m3, (int64_t)sparsePageSize);
}

GARNET_INTERNAL __attribute__((unused)) MTLSize rdna4_device_sparse_tile_size(MTLTextureType textureType,
                                                                     MTLPixelFormat pixelFormat,
                                                                     NSUInteger sampleCount) {
    const rdna4dc_metal3_caps m3 = rdna4_metal3_caps();
    rdna4sp_size sz = { 0u, 0u, 0u };
    if (!rdna4sp_tile_size_gated(&m3, (uint64_t)textureType, (uint64_t)pixelFormat,
                                 (uint64_t)sampleCount, &sz))
        return MTLSizeMake(0, 0, 0);

    const rdna4sp_fmt_info *fi = rdna4sp_fmt_lookup((uint64_t)pixelFormat);
    const RDNA4Fmt *fmt = rdna4_fmt(pixelFormat);
    if (!fmt) fmt = rdna4_fmt_ds(pixelFormat);
    if (!fmt || !fi) return MTLSizeMake(0, 0, 0);

    // Cross-check against RADV's authoritative sparse image granularity when RADV is open.
    // Never fall back to the static table if RADV reports zero matching sparse format properties.
    const rdna4_sparse_vk_fn_t *vk = rdna4_sparse_vk_fns();
    if (RDNA4R.ok && RDNA4R.pd && vk && vk->getPhysSparseImageFormatProps) {
        VkImageType vit = textureType == MTLTextureType3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
        VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        uint32_t propCount = 0u;
        vk->getPhysSparseImageFormatProps(RDNA4R.pd, fmt->vk, vit, (VkSampleCountFlagBits)sampleCount,
                                          usage, VK_IMAGE_TILING_OPTIMAL, &propCount, NULL);
        if (propCount == 0u) return MTLSizeMake(0, 0, 0);
        uint32_t cap = propCount > 8u ? 8u : propCount;
        VkSparseImageFormatProperties props[8];
        memset(props, 0, sizeof props);
        vk->getPhysSparseImageFormatProps(RDNA4R.pd, fmt->vk, vit, (VkSampleCountFlagBits)sampleCount,
                                          usage, VK_IMAGE_TILING_OPTIMAL, &cap, props);
        for (uint32_t i = 0u; i < cap; i++) {
            if ((props[i].aspectMask & fi->aspect_mask) == fi->aspect_mask &&
                props[i].imageGranularity.width && props[i].imageGranularity.height &&
                props[i].imageGranularity.depth) {
                return MTLSizeMake(props[i].imageGranularity.width,
                                   props[i].imageGranularity.height,
                                   props[i].imageGranularity.depth);
            }
        }
        return MTLSizeMake(0, 0, 0);
    }
    return MTLSizeMake((NSUInteger)sz.width, (NSUInteger)sz.height, (NSUInteger)sz.depth);
}

GARNET_INTERNAL __attribute__((unused)) MTLSize rdna4_device_sparse_tile_size_with_page_size(MTLTextureType textureType,
                                                                                    MTLPixelFormat pixelFormat,
                                                                                    NSUInteger sampleCount,
                                                                                    NSInteger sparsePageSize) {
    if (sparsePageSize != RDNA4SP_PAGE_SIZE_64) return MTLSizeMake(0, 0, 0);
    return rdna4_device_sparse_tile_size(textureType, pixelFormat, sampleCount);
}

GARNET_INTERNAL __attribute__((unused)) void rdna4_device_convert_sparse_pixel_regions(const MTLRegion *pixelRegions,
                                                                              MTLRegion *tileRegions,
                                                                              MTLSize tileSize,
                                                                              MTLSparseTextureRegionAlignmentMode mode,
                                                                              NSUInteger numRegions) {
    if (!numRegions || !pixelRegions || !tileRegions) return;
    rdna4sp_size ts = { (uint64_t)tileSize.width, (uint64_t)tileSize.height, (uint64_t)tileSize.depth };
    for (NSUInteger i = 0; i < numRegions; i++) {
        rdna4sp_region inR = {
            .origin = { (uint64_t)pixelRegions[i].origin.x, (uint64_t)pixelRegions[i].origin.y, (uint64_t)pixelRegions[i].origin.z },
            .size   = { (uint64_t)pixelRegions[i].size.width, (uint64_t)pixelRegions[i].size.height, (uint64_t)pixelRegions[i].size.depth },
        };
        rdna4sp_region outR = { {0, 0, 0}, {0, 0, 0} };
        if (rdna4sp_convert_pixel_regions_to_tiles(&inR, &outR, ts, (uint32_t)mode, 1u)) {
            tileRegions[i] = MTLRegionMake3D((NSUInteger)outR.origin.x, (NSUInteger)outR.origin.y, (NSUInteger)outR.origin.z,
                                             (NSUInteger)outR.size.width, (NSUInteger)outR.size.height, (NSUInteger)outR.size.depth);
        } else {
            tileRegions[i] = MTLRegionMake3D(0, 0, 0, 0, 0, 0);
        }
    }
}

GARNET_INTERNAL __attribute__((unused)) void rdna4_device_convert_sparse_tile_regions(const MTLRegion *tileRegions,
                                                                             MTLRegion *pixelRegions,
                                                                             MTLSize tileSize,
                                                                             NSUInteger numRegions) {
    if (!numRegions || !tileRegions || !pixelRegions) return;
    rdna4sp_size ts = { (uint64_t)tileSize.width, (uint64_t)tileSize.height, (uint64_t)tileSize.depth };
    for (NSUInteger i = 0; i < numRegions; i++) {
        rdna4sp_region inR = {
            .origin = { (uint64_t)tileRegions[i].origin.x, (uint64_t)tileRegions[i].origin.y, (uint64_t)tileRegions[i].origin.z },
            .size   = { (uint64_t)tileRegions[i].size.width, (uint64_t)tileRegions[i].size.height, (uint64_t)tileRegions[i].size.depth },
        };
        rdna4sp_region outR = { {0, 0, 0}, {0, 0, 0} };
        if (rdna4sp_convert_tile_regions_to_pixels(&inR, &outR, ts, 1u)) {
            pixelRegions[i] = MTLRegionMake3D((NSUInteger)outR.origin.x, (NSUInteger)outR.origin.y, (NSUInteger)outR.origin.z,
                                              (NSUInteger)outR.size.width, (NSUInteger)outR.size.height, (NSUInteger)outR.size.depth);
        } else {
            pixelRegions[i] = MTLRegionMake3D(0, 0, 0, 0, 0, 0);
        }
    }
}

#endif /* RDNA4_SPARSE_IMPL_INC */
