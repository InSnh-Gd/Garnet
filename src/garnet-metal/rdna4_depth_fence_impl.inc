// ---- gap census: depth/stencil state and fences ----
// MTLDepthStencilState: SkyLight and QuartzCore create one each at start-up (-newDepthStencilStateWithDescriptor:, census rdt1), and the compositor sends
// -setDepthStencilState: on its encoders. The render passes here have colour attachments only (no depth/stencil exist), for which Metal makes the
// state a no-op, so this object records the descriptor and nothing else.
// bundle 10: the state IS used now (a pipeline with a depth/stencil attachment reads it at every draw): compare, depth write, the two stencil faces (compare, ops, masks).
typedef struct { unsigned long cmp; int write; rdna4ds_face_in_t f, b; uint32_t fRead, fWrite, bRead, bWrite; } RDNA4DSInfo;
static RDNA4DSInfo rdna4_dsinfo_default(void) { return (RDNA4DSInfo){ RDNA4DS_CMP_ALWAYS, 0, { RDNA4DS_CMP_ALWAYS, 0, 0, 0 }, { RDNA4DS_CMP_ALWAYS, 0, 0, 0 }, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu }; }
@interface RDNA4DepthStencilState : NSObject { id _dev; NSString *_lbl; MTLDepthStencilDescriptor *_d; RDNA4DSInfo _info; }
- (instancetype)initWithDevice:(id)dev descriptor:(MTLDepthStencilDescriptor *)d;
- (MTLDepthStencilDescriptor *)rdna4Descriptor;
- (RDNA4DSInfo)rdna4Info;
@end
@implementation RDNA4DepthStencilState
- (instancetype)initWithDevice:(id)dev descriptor:(MTLDepthStencilDescriptor *)d {
    self = [super init]; if (!self) return nil;
    _dev = dev; _d = [d copy]; _lbl = [d.label copy];
    _info = rdna4_dsinfo_default();
    _info.cmp = (unsigned long)d.depthCompareFunction; _info.write = d.depthWriteEnabled ? 1 : 0;   // bundle 10
    MTLStencilDescriptor *fs = d.frontFaceStencil, *bs = d.backFaceStencil;   // nil = Always / Keep / masks all ones
    if (fs) { _info.f = (rdna4ds_face_in_t){ (unsigned long)fs.stencilCompareFunction, (unsigned long)fs.stencilFailureOperation, (unsigned long)fs.depthFailureOperation, (unsigned long)fs.depthStencilPassOperation }; _info.fRead = fs.readMask; _info.fWrite = fs.writeMask; }
    if (bs) { _info.b = (rdna4ds_face_in_t){ (unsigned long)bs.stencilCompareFunction, (unsigned long)bs.stencilFailureOperation, (unsigned long)bs.depthFailureOperation, (unsigned long)bs.depthStencilPassOperation }; _info.bRead = bs.readMask; _info.bWrite = bs.writeMask; }
    RDNA4LOG("RDNA4DepthStencilState %p: depth compare %lu write %d; stencil front cmp %lu ops %lu/%lu/%lu masks %x/%x, back cmp %lu ops %lu/%lu/%lu masks %x/%x (used by pipelines with a depth/stencil attachment)", (__bridge void *)self,
           _info.cmp, _info.write, _info.f.cmp, _info.f.fail, _info.f.dfail, _info.f.pass, _info.fRead, _info.fWrite, _info.b.cmp, _info.b.fail, _info.b.dfail, _info.b.pass, _info.bRead, _info.bWrite);
    return self;
}
RDNA4_DNR(RDNA4DepthStencilState)
- (MTLDepthStencilDescriptor *)rdna4Descriptor { return _d; }
- (RDNA4DSInfo)rdna4Info { return _info; }
- (id)device { return _dev; }
- (NSString *)label { return _lbl; }
- (void)setLabel:(NSString *)l { _lbl = [l copy]; }
@end

// A fence update is a checkpoint at an encoded command-buffer position. The
// producer segment's Vulkan fence completes the checkpoint; a consumer segment
// that waits on it is not submitted until then. This is deliberately stronger
// than a scoped stage dependency: the backend keeps a conservative full barrier.
typedef NS_ENUM(int, RDNA4FenceCheckpointState) {
    RDNA4FenceCheckpointPending = 0,
    RDNA4FenceCheckpointReady = 1,
    RDNA4FenceCheckpointFailed = 2,
};

@interface RDNA4FenceCheckpoint : NSObject {
@public
    uint64_t generation;
    uint64_t producerCBId;
    _Atomic int state;
    _Atomic int fault;
}
- (void)rdna4Ready;
- (void)rdna4Fail:(VkResult)fault;
@end

@implementation RDNA4FenceCheckpoint
- (void)rdna4Ready {
    int want = RDNA4FenceCheckpointPending;
    (void)atomic_compare_exchange_strong_explicit(&state, &want, RDNA4FenceCheckpointReady,
                                                  memory_order_release, memory_order_relaxed);
}
- (void)rdna4Fail:(VkResult)result {
    atomic_store_explicit(&fault, (int)result, memory_order_relaxed);
    int current = atomic_load_explicit(&state, memory_order_relaxed);
    while (current == RDNA4FenceCheckpointPending) {
        int want = RDNA4FenceCheckpointPending;
        if (atomic_compare_exchange_weak_explicit(&state, &want, RDNA4FenceCheckpointFailed,
                                                  memory_order_release, memory_order_relaxed)) break;
        current = want;
    }
}
@end

// MTLFence identity plus generation state. A wait captures the latest update
// encoded at that point. A new fence starts at a ready generation-zero
// checkpoint, so waiting before any update is a completed no-op dependency; a
// later update creates a new pending checkpoint and never retargets that wait.
@interface RDNA4Fence : NSObject {
    id _dev;
    NSString *_lbl;
    pthread_mutex_t _mu;
    uint64_t _latestGeneration;
    RDNA4FenceCheckpoint *_latestCheckpoint;
}
- (instancetype)initWithDevice:(id)dev;
- (RDNA4FenceCheckpoint *)rdna4UpdateCheckpoint;
- (RDNA4FenceCheckpoint *)rdna4UpdateCheckpointForCB:(uint64_t)cbid;
- (RDNA4FenceCheckpoint *)rdna4WaitCheckpoint;
@end
@implementation RDNA4Fence
- (instancetype)initWithDevice:(id)dev {
    self = [super init];
    if (!self) return nil;
    _dev = dev;
    pthread_mutex_init(&_mu, NULL);
    _latestCheckpoint = [RDNA4FenceCheckpoint new];
    _latestCheckpoint->generation = 0;
    [_latestCheckpoint rdna4Ready];
    return self;
}
- (void)dealloc { pthread_mutex_destroy(&_mu); }
RDNA4_DNR(RDNA4Fence)
- (RDNA4FenceCheckpoint *)rdna4UpdateCheckpointForCB:(uint64_t)cbid {
    pthread_mutex_lock(&_mu);
    if (_latestGeneration == UINT64_MAX) {
        pthread_mutex_unlock(&_mu);
        return nil;
    }
    uint64_t generation = _latestGeneration + 1;
    RDNA4FenceCheckpoint *checkpoint = [RDNA4FenceCheckpoint new];
    checkpoint->generation = generation;
    checkpoint->producerCBId = cbid;
    _latestGeneration = generation;
    _latestCheckpoint = checkpoint;
    pthread_mutex_unlock(&_mu);
    return checkpoint;
}
- (RDNA4FenceCheckpoint *)rdna4UpdateCheckpoint { return [self rdna4UpdateCheckpointForCB:0]; }
- (RDNA4FenceCheckpoint *)rdna4WaitCheckpoint {
    pthread_mutex_lock(&_mu);
    RDNA4FenceCheckpoint *checkpoint = _latestCheckpoint;
    pthread_mutex_unlock(&_mu);
    return checkpoint;
}
- (id)device { return _dev; }
- (NSString *)label { return _lbl; }
- (void)setLabel:(NSString *)l { _lbl = [l copy]; }
@end

// Indirect resources must survive command execution, even without a bound descriptor.
#define RDNA4_ENCODER_SHARED \
    - (void)useResource:(id)r usage:(NSUInteger)u { if (_ended) { [_cb rdna4Fail:@"useResource after endEncoding"]; return; } [_cb rdna4UseResource:r usage:u]; if (r && ![_cb rdna4Error]) { if (u & MTLResourceUsageWrite) { if (!_argumentWritable) _argumentWritable = [NSMutableSet set]; [_argumentWritable addObject:r]; } if (!_argumentResources) _argumentResources = [NSMutableSet set]; [_argumentResources addObject:r]; } } \
    - (void)useResource:(id)r usage:(NSUInteger)u stages:(NSUInteger)st { (void)st; [self useResource:r usage:u]; } \
    - (void)useResources:(const id __unsafe_unretained *)r count:(NSUInteger)n usage:(NSUInteger)u { if (n && !r) { [_cb rdna4Fail:@"useResources has a NULL resource array"]; return; } for (NSUInteger i = 0; i < n; ++i) [self useResource:r[i] usage:u]; } \
    - (void)useResources:(const id __unsafe_unretained *)r count:(NSUInteger)n usage:(NSUInteger)u stages:(NSUInteger)st { (void)st; [self useResources:r count:n usage:u]; } \
    - (void)useHeap:(id)h { if (_ended) { [_cb rdna4Fail:@"useHeap after endEncoding"]; return; } [_cb rdna4UseHeap:h]; if (h && ![_cb rdna4Error]) { if (!_argumentWritable) _argumentWritable = [NSMutableSet set]; [_argumentWritable addObject:h]; if (!_argumentResources) _argumentResources = [NSMutableSet set]; [_argumentResources addObject:h]; } } \
    - (void)useHeap:(id)h stages:(NSUInteger)st { (void)st; [self useHeap:h]; } \
    - (void)useHeaps:(const id __unsafe_unretained *)h count:(NSUInteger)n { if (n && !h) { [_cb rdna4Fail:@"useHeaps has a NULL heap array"]; return; } for (NSUInteger i = 0; i < n; ++i) [self useHeap:h[i]]; } \
    - (void)useHeaps:(const id __unsafe_unretained *)h count:(NSUInteger)n stages:(NSUInteger)st { (void)st; [self useHeaps:h count:n]; } \
    - (void)useResourceGroup:(id)g usage:(NSUInteger)u stages:(NSUInteger)st { (void)g; (void)u; (void)st; RDNA4_ONCE("useResourceGroup: ignored"); }

