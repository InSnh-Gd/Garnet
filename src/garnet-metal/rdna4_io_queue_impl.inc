// ---------------------------------------------------------------------------------------------------------------
// Metal 3 Fast Resource Loading (Task 4.1): RDNA4IOFileHandle, RDNA4IOCommandQueue, RDNA4IOCommandBuffer.
//
// Wave 2B GarnetDevice.m integration:
//   1. Include order in GarnetDevice.m: place after `#include "rdna4_gl_drawable_impl.inc"`
//      (after RDNA4Buffer, RDNA4Texture, RDNA4CommandQueue, rdna4_full_barrier, rdna4_oneshot,
//      and rdna4_event_* / rdna4_schedule_kick are defined):
//        #include "rdna4_residency_set_impl.inc"
//        #include "rdna4_io_queue_impl.inc"
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
#include "rdna4_io_queue.h"

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
@interface RDNA4IOFileHandle : NSObject {
    id _dev;
    int _fd;
    uint64_t _fileSize;
    NSString *_lbl;
}
- (instancetype)initWithDevice:(id)dev url:(NSURL *)url
             compressionMethod:(NSInteger)method compressed:(BOOL)compressed
                         error:(NSError **)err;
- (id)rdna4Device;
- (uint64_t)rdna4FileSize;
- (BOOL)rdna4ReadInto:(void *)dst size:(size_t)size offset:(uint64_t)offset error:(NSError **)err;
@end

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
typedef enum {
    RDNA4IOCMD_LOAD_BYTES = 1,
    RDNA4IOCMD_LOAD_BUFFER,
    RDNA4IOCMD_LOAD_TEXTURE,
    RDNA4IOCMD_BARRIER,
    RDNA4IOCMD_WAIT_EVENT,
    RDNA4IOCMD_SIGNAL_EVENT,
    RDNA4IOCMD_COPY_STATUS,
} RDNA4IOCommandKind;

@interface RDNA4IOCommand : NSObject {
@public
    RDNA4IOCommandKind kind;
    RDNA4IOFileHandle *handle;
    RDNA4Buffer *dstBuffer;
    RDNA4Texture *dstTexture;
    id event;
    void *dstPtr;
    uint64_t dstOffset;
    uint64_t size;
    uint64_t srcOffset;
    uint64_t eventValue;
    NSUInteger slice, level;
    MTLOrigin origin;
    MTLSize texSize;
    rdna4io_tex_plan_t texPlan;
}
@end
@implementation RDNA4IOCommand
@end

@class RDNA4IOCommandQueue;
@class RDNA4IOCommandBuffer;

@interface RDNA4IOCommandQueue : NSObject {
    id _dev;
    NSString *_lbl;
    id _scratchAllocator;
    dispatch_queue_t _workQ;
    dispatch_queue_t _compQ;
    pthread_mutex_t _qmu;
    rdna4io_qsched_t _sched;
    NSMapTable<NSNumber *, RDNA4IOCommandBuffer *> *_enqueuedWeak;
    NSMutableDictionary<NSNumber *, RDNA4IOCommandBuffer *> *_committedStrong;
    NSMutableArray<RDNA4Buffer *> *_stagingPool;
}
- (instancetype)initWithDevice:(id)dev descriptor:(id)desc error:(NSError **)err;
- (id)device;
- (dispatch_queue_t)rdna4IOCompletionQueue;
- (uint64_t)rdna4IOReserveSlotForBuffer:(RDNA4IOCommandBuffer *)cb;
- (BOOL)rdna4IOCommitBuffer:(RDNA4IOCommandBuffer *)cb slot:(uint64_t)slot;
- (BOOL)rdna4IOCancelSlotIfPending:(uint64_t)slot;
- (void)rdna4IOAbandonUncommittedSlot:(uint64_t)slot;
- (void)rdna4IOSlotDone:(uint64_t)slot;
- (RDNA4Buffer *)rdna4IOAcquireStaging:(NSUInteger)bytes customScratch:(id __strong *)customOut error:(NSError **)err;
- (void)rdna4IOReleaseStaging:(RDNA4Buffer *)buf customScratch:(id)custom;
@end

@interface RDNA4IOCommandBuffer : NSObject {
    RDNA4IOCommandQueue *_q;
    id _dev;
    BOOL _retained;
    NSString *_lbl;
    pthread_mutex_t _mu;
    pthread_cond_t _cond;
    rdna4io_cb_state_t _state;
    BOOL _finishing;
    uint64_t _slotId;
    NSError *_err;
    NSMutableArray<RDNA4IOCommand *> *_cmds;
    NSMutableArray<void (^)(id)> *_handlers;
    dispatch_semaphore_t _waitEventSem;
}
- (instancetype)initWithQueue:(RDNA4IOCommandQueue *)queue retainedReferences:(BOOL)retained;
- (void)rdna4IOExecuteOnWorkerWithSlot:(uint64_t)slot;
@end

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
