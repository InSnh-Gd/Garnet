// ---------------------------------------------------------------------------------------------------------------
// Metal 3.2 Residency Sets (Task 4.2): RDNA4ResidencySnapshot & RDNA4ResidencySet.
//
// Wave 2B GarnetDevice.m integration:
//   1. Include order in GarnetDevice.m: place after `#include "rdna4_gl_drawable_impl.inc"`
//      (after RDNA4Buffer, RDNA4Texture, RDNA4Heap, and RDNA4Residency*Internal protocols in
//      rdna4_command_queue_impl.inc are defined):
//        #include "rdna4_residency_set_impl.inc"
//        #include "rdna4_io_queue_impl.inc"
//   2. Device factory selector on @implementation GarnetDevice:
//        - (id)newResidencySetWithDescriptor:(id)descriptor error:(NSError **)error {
//            return [[RDNA4ResidencySet alloc] initWithDevice:self descriptor:descriptor error:error];
//        }
//   3. Optional 2-line Tier 2 texture hook in `rdna4_argument_table_commands.inc` (`rdna4PrepareArguments:`):
//        NSSet *residency = [self rdna4ResidencyDeclaredResources];
//        if (residency.count) resources = [resources setByAddingObjectsFromSet:residency];
//      (Indirect BDA buffers are already merged directly in `rdna4_command_buffer_impl.inc` `rdna4FillSet:`.)
//   4. Protocol MTLResidencySet is attached automatically in +initialize via rdna4_add_protocols()
//      and may also be listed in GarnetDevice +load census.
// ---------------------------------------------------------------------------------------------------------------
#include "rdna4_residency_set.h"

static BOOL rdna4_residency_alloc_valid(id obj, id dev, uint64_t *sizeOut) {
    if (sizeOut) *sizeOut = 0;
    if (!obj || !dev) return NO;
    if (![obj respondsToSelector:@selector(device)] || [obj device] != dev) return NO;
    BOOL knownType = [obj isKindOfClass:[RDNA4Buffer class]] ||
                     [obj isKindOfClass:[RDNA4Texture class]] ||
                     [obj isKindOfClass:[RDNA4Heap class]];
    if (!knownType) {
        Class asCls = objc_getClass("GarnetAccelerationStructure");
        Class spCls = objc_getClass("RDNA4SparseHeap");
        if ((asCls && [obj isKindOfClass:asCls]) ||
            (spCls && [obj isKindOfClass:spCls]))
            knownType = YES;
    }
    if (!knownType) {
        Protocol *pAlloc = objc_getProtocol("MTLAllocation");
        Protocol *pRes = objc_getProtocol("MTLResource");
        Protocol *pHeap = objc_getProtocol("MTLHeap");
        Protocol *pAS = objc_getProtocol("MTLAccelerationStructure");
        if ((pAlloc && [obj conformsToProtocol:pAlloc]) ||
            (pRes && [obj conformsToProtocol:pRes]) ||
            (pHeap && [obj conformsToProtocol:pHeap]) ||
            (pAS && [obj conformsToProtocol:pAS]))
            knownType = YES;
    }
    if (!knownType) return NO;
    if (sizeOut) {
        if ([obj respondsToSelector:@selector(allocatedSize)])
            *sizeOut = (uint64_t)((NSUInteger (*)(id, SEL))objc_msgSend)(obj, @selector(allocatedSize));
        else if ([obj respondsToSelector:@selector(size)])
            *sizeOut = (uint64_t)((NSUInteger (*)(id, SEL))objc_msgSend)(obj, @selector(size));
    }
    return YES;
}

@interface RDNA4ResidencySnapshot : NSObject <RDNA4ResidencySnapshotInternal> {
    NSArray *_allocations;
    uint64_t _allocatedSize;
    uint64_t _generation;
}
- (instancetype)initWithAllocations:(NSArray *)allocations
                      allocatedSize:(uint64_t)allocatedSize
                         generation:(uint64_t)generation;
- (uint64_t)rdna4AllocatedSize;
- (uint64_t)rdna4Generation;
@end

@implementation RDNA4ResidencySnapshot
- (instancetype)initWithAllocations:(NSArray *)allocations
                      allocatedSize:(uint64_t)allocatedSize
                         generation:(uint64_t)generation {
    self = [super init];
    if (!self) return nil;
    _allocations = allocations ? [allocations copy] : @[];
    _allocatedSize = allocatedSize;
    _generation = generation;
    return self;
}

- (NSArray *)rdna4Allocations { return _allocations; }
- (uint64_t)rdna4AllocatedSize { return _allocatedSize; }
- (uint64_t)rdna4Generation { return _generation; }

- (void)rdna4AddBuffersToSet:(NSMutableSet *)buffers device:(id)device {
    if (!buffers || !device) return;
    for (id obj in _allocations) {
        if ([obj isKindOfClass:[RDNA4Buffer class]]) {
            RDNA4Buffer *b = (RDNA4Buffer *)obj;
            if ([b device] == device && ![b isAliasable]) [buffers addObject:b];
        } else if ([obj isKindOfClass:[RDNA4Heap class]]) {
            RDNA4Heap *h = (RDNA4Heap *)obj;
            if ([h device] != device) continue;
            for (id res in [h rdna4LiveResources]) {
                if ([res isKindOfClass:[RDNA4Buffer class]] && [(RDNA4Buffer *)res device] == device &&
                    ![res isAliasable])
                    [buffers addObject:res];
            }
        }
    }
}
@end

@interface RDNA4ResidencySet : NSObject <RDNA4ResidencySetInternal> {
    id _dev;
    NSString *_lbl;
    pthread_mutex_t _mu;
    rdna4rs_set_t _state;
    NSMutableDictionary<NSNumber *, id> *_stagedObjects;
    NSMutableDictionary<NSNumber *, id> *_committedObjects;
    RDNA4ResidencySnapshot *_snapshot;
}
- (instancetype)initWithDevice:(id)dev descriptor:(id)desc error:(NSError **)err;
- (BOOL)rdna4IsResidentRequested;
@end

@implementation RDNA4ResidencySet
+ (void)initialize {
    if (self == [RDNA4ResidencySet class]) {
        static dispatch_once_t once;
        dispatch_once(&once, ^{
            const char *n[] = { "MTLResidencySet" };
            rdna4_add_protocols(self, n, 1, "RDNA4ResidencySet");
        });
    }
}
RDNA4_DNR(RDNA4ResidencySet)

- (instancetype)initWithDevice:(id)dev descriptor:(id)desc error:(NSError **)err {
    self = [super init];
    if (!self) return nil;
    if (!dev) {
        if (err) *err = rdna4_err(130, @"residency set requires a valid device");
        return nil;
    }
    NSUInteger initialCap = 0;
    NSString *lbl = nil;
    if (desc) {
        SEL capSel = sel_registerName("initialCapacity");
        if ([desc respondsToSelector:capSel])
            initialCap = ((NSUInteger (*)(id, SEL))objc_msgSend)(desc, capSel);
        SEL lblSel = sel_registerName("label");
        if ([desc respondsToSelector:lblSel]) {
            id raw = ((id (*)(id, SEL))objc_msgSend)(desc, lblSel);
            if ([raw isKindOfClass:[NSString class]]) lbl = [(NSString *)raw copy];
        }
    }
    if (!rdna4rs_validate_capacity((uint64_t)initialCap) || !rdna4rs_init(&_state, (uint64_t)initialCap)) {
        if (err) *err = rdna4_err(130,
            [NSString stringWithFormat:@"residency set initialCapacity %lu is invalid or cannot be allocated",
             (unsigned long)initialCap]);
        return nil;
    }
    _dev = dev;
    _lbl = lbl;
    pthread_mutex_init(&_mu, NULL);
    _stagedObjects = [NSMutableDictionary dictionary];
    _committedObjects = [NSMutableDictionary dictionary];
    _snapshot = [[RDNA4ResidencySnapshot alloc] initWithAllocations:@[] allocatedSize:0 generation:0];
    return self;
}

- (void)dealloc {
    pthread_mutex_lock(&_mu);
    rdna4rs_destroy(&_state);
    [_stagedObjects removeAllObjects];
    [_committedObjects removeAllObjects];
    _snapshot = nil;
    pthread_mutex_unlock(&_mu);
    pthread_mutex_destroy(&_mu);
}

- (id)device { return _dev; }
- (NSString *)label { return _lbl; }

- (uint64_t)allocatedSize {
    pthread_mutex_lock(&_mu);
    uint64_t sz = [_snapshot rdna4AllocatedSize];
    pthread_mutex_unlock(&_mu);
    return sz;
}

- (NSUInteger)allocationCount {
    pthread_mutex_lock(&_mu);
    NSUInteger n = [_snapshot rdna4Allocations].count;
    pthread_mutex_unlock(&_mu);
    return n;
}

- (NSArray *)allAllocations {
    pthread_mutex_lock(&_mu);
    NSArray *arr = [_snapshot rdna4Allocations];
    pthread_mutex_unlock(&_mu);
    return arr;
}

- (BOOL)containsAllocation:(id)allocation {
    if (!allocation) return NO;
    uintptr_t key = (uintptr_t)(__bridge void *)allocation;
    pthread_mutex_lock(&_mu);
    BOOL present = rdna4rs_contains(&_state, key) != 0;
    pthread_mutex_unlock(&_mu);
    return present;
}

- (void)addAllocation:(id)allocation {
    uint64_t sz = 0;
    if (!rdna4_residency_alloc_valid(allocation, _dev, &sz)) return;
    uintptr_t key = (uintptr_t)(__bridge void *)allocation;
    pthread_mutex_lock(&_mu);
    if (rdna4rs_add(&_state, key, sz)) _stagedObjects[@(key)] = allocation;
    pthread_mutex_unlock(&_mu);
}

- (void)addAllocations:(const id __unsafe_unretained *)allocations count:(NSUInteger)count {
    if (!allocations || !count) return;
    for (NSUInteger i = 0; i < count; i++) [self addAllocation:allocations[i]];
}

- (void)removeAllocation:(id)allocation {
    if (!allocation) return;
    uintptr_t key = (uintptr_t)(__bridge void *)allocation;
    int removedPending = 0;
    pthread_mutex_lock(&_mu);
    if (rdna4rs_remove(&_state, key, &removedPending) && removedPending)
        [_stagedObjects removeObjectForKey:@(key)];
    pthread_mutex_unlock(&_mu);
}

- (void)removeAllocations:(const id __unsafe_unretained *)allocations count:(NSUInteger)count {
    if (!allocations || !count) return;
    for (NSUInteger i = 0; i < count; i++) [self removeAllocation:allocations[i]];
}

- (void)removeAllAllocations {
    pthread_mutex_lock(&_mu);
    rdna4rs_remove_all(&_state);
    [_stagedObjects removeAllObjects];
    pthread_mutex_unlock(&_mu);
}

- (void)commit {
    pthread_mutex_lock(&_mu);
    rdna4rs_diff_t diff = {0};
    if (!rdna4rs_commit(&_state, &diff)) {
        pthread_mutex_unlock(&_mu);
        RDNA4LOG("RDNA4ResidencySet %p: commit failed (out of memory); preserving prior snapshot", (__bridge void *)self);
        return;
    }
    for (size_t i = 0; i < diff.added_count; i++) {
        NSNumber *k = @(diff.added[i]);
        id obj = _stagedObjects[k];
        if (obj) _committedObjects[k] = obj;
    }
    for (size_t i = 0; i < diff.removed_count; i++) {
        [_committedObjects removeObjectForKey:@(diff.removed[i])];
    }
    [_stagedObjects removeAllObjects];

    NSMutableArray *arr = [NSMutableArray arrayWithCapacity:_state.committed_count];
    for (size_t i = 0; i < _state.committed_count; i++) {
        id obj = _committedObjects[@(_state.committed[i].key)];
        if (obj) [arr addObject:obj];
    }
    _snapshot = [[RDNA4ResidencySnapshot alloc] initWithAllocations:arr
                                                      allocatedSize:_state.committed_size
                                                         generation:_state.generation];
    rdna4rs_diff_destroy(&diff);
    pthread_mutex_unlock(&_mu);
}

- (void)requestResidency {
    pthread_mutex_lock(&_mu);
    rdna4rs_request_residency(&_state);
    pthread_mutex_unlock(&_mu);
}

- (void)endResidency {
    pthread_mutex_lock(&_mu);
    rdna4rs_end_residency(&_state);
    pthread_mutex_unlock(&_mu);
}

- (id<RDNA4ResidencySnapshotInternal>)rdna4CommittedSnapshot {
    pthread_mutex_lock(&_mu);
    RDNA4ResidencySnapshot *snap = _snapshot;
    pthread_mutex_unlock(&_mu);
    return snap;
}

- (BOOL)rdna4IsResidentRequested {
    pthread_mutex_lock(&_mu);
    BOOL active = _state.resident_requested != 0;
    pthread_mutex_unlock(&_mu);
    return active;
}
@end
