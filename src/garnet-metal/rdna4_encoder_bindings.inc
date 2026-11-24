#ifndef RDNA4_ENCODER_BINDINGS_INC
#define RDNA4_ENCODER_BINDINGS_INC

enum {
    RDNA4_RESOURCE_PIPELINE = 1,
    RDNA4_RESOURCE_DEPTH_STENCIL = 2,
    RDNA4_RESOURCE_ATTACHMENT = 3,
    RDNA4_RESOURCE_RESOLVE = 4,
    RDNA4_RESOURCE_BUFFER = 5,
    RDNA4_RESOURCE_TEXTURE = 6,
    RDNA4_RESOURCE_ACCEL = 7,
};

static NSNumber *rdna4_resource_slot(uint32_t kind, BOOL fragment,
                                     NSUInteger index) {
    uint64_t value = ((uint64_t)kind << 56) |
        ((uint64_t)(fragment ? 1u : 0u) << 48) | (uint64_t)index;
    return @(value);
}

static BOOL rdna4_encoder_take_segment_change(uint64_t *tracked,
                                              uint64_t current) {
    if (!tracked || *tracked == current) return NO;
    *tracked = current;
    return YES;
}

/*
 * Current encoder state is bounded by the descriptor bands. The dictionary
 * owns only objects still bound to a slot; it is not a command-buffer history.
 * Every draw/dispatch snapshots these objects into the current segment.
 */
@interface RDNA4ResourceBindings : NSObject {
    NSMutableDictionary<NSNumber *, id> *_objects;
}
- (void)setObject:(id)object slot:(NSNumber *)slot;
- (void)clear;
- (NSUInteger)count;
- (void)snapshotToCommandBuffer:(RDNA4CommandBuffer *)commandBuffer;
@end

@implementation RDNA4ResourceBindings
- (instancetype)init {
    self = [super init];
    if (!self) return nil;
    _objects = [NSMutableDictionary dictionary];
    return self;
}
- (void)setObject:(id)object slot:(NSNumber *)slot {
    if (!slot) return;
    if (object) _objects[slot] = object;
    else [_objects removeObjectForKey:slot];
}
- (void)clear {
    [_objects removeAllObjects];
}
- (NSUInteger)count {
    return _objects.count;
}
- (void)snapshotToCommandBuffer:(RDNA4CommandBuffer *)commandBuffer {
    if (!commandBuffer) return;
    @autoreleasepool {
        for (id object in _objects.allValues)
            [commandBuffer rdna4RetainEncoded:object];
    }
}
@end

#endif
