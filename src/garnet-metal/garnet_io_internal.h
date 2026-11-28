// garnet_io_internal.h - Shared internal declarations for Metal 3 Fast Resource Loading.
#ifndef GARNET_IO_INTERNAL_H
#define GARNET_IO_INTERNAL_H

#include "garnet_io_queue.h"

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

@interface RDNA4IOCommandBuffer (GarnetInternalMethods)
+ (void)initialize;
- (NSString *)label;
- (void)setLabel:(NSString *)l;
- (BOOL)retainedReferences;
- (NSInteger)status;
- (NSError *)error;
- (void)rdna4IOLatchLocked:(rdna4io_reason_t)reason mtlErr:(NSInteger)code detail:(NSString *)detail;
- (BOOL)rdna4IORecordCommandLocked:(RDNA4IOCommand *)cmd;
- (void)addCompletedHandler:(void (^)(id))block;
- (void)loadBytes:(void *)pointer size:(NSUInteger)size sourceHandle:(id)sourceHandle sourceHandleOffset:(NSUInteger)sourceHandleOffset;
- (void)loadBuffer:(id)buffer offset:(NSUInteger)offset size:(NSUInteger)size sourceHandle:(id)sourceHandle sourceHandleOffset:(NSUInteger)sourceHandleOffset;
- (void)loadTexture:(id)texture slice:(NSUInteger)slice level:(NSUInteger)level size:(MTLSize)size sourceBytesPerRow:(NSUInteger)sourceBytesPerRow sourceBytesPerImage:(NSUInteger)sourceBytesPerImage destinationOrigin:(MTLOrigin)destinationOrigin sourceHandle:(id)sourceHandle sourceHandleOffset:(NSUInteger)sourceHandleOffset;
- (void)copyStatusToBuffer:(id)buffer offset:(NSUInteger)offset;
- (void)addBarrier;
- (void)waitForEvent:(id)event value:(uint64_t)value;
- (void)signalEvent:(id)event value:(uint64_t)value;
- (void)pushDebugGroup:(NSString *)string;
- (void)popDebugGroup;
- (void)enqueue;
- (BOOL)rdna4IOWriteStatus:(int64_t)statusVal toBuffer:(RDNA4Buffer *)dst offset:(uint64_t)off;
- (void)rdna4IOFinishWithReason:(rdna4io_reason_t)reason mtlErr:(NSInteger)mtlErr detail:(NSString *)detail startIdx:(NSUInteger)startIdx releaseSlot:(uint64_t)releaseSlot;
- (void)commit;
- (void)tryCancel;
- (void)waitUntilCompleted;
- (BOOL)rdna4IOCancelledNow;
- (BOOL)rdna4IOExecLoadBuffer:(RDNA4IOCommand *)c error:(NSError **)err cancelled:(BOOL *)cancelled;
- (BOOL)rdna4IOExecLoadTexture:(RDNA4IOCommand *)c error:(NSError **)err cancelled:(BOOL *)cancelled;
- (BOOL)rdna4IOExecWaitEvent:(id)event value:(uint64_t)value error:(NSError **)err cancelled:(BOOL *)cancelled;
@end

@interface RDNA4IOCommandQueue (GarnetInternalMethods)
+ (void)initialize;
- (NSString *)label;
- (void)setLabel:(NSString *)l;
- (id)commandBuffer;
- (id)commandBufferWithUnretainedReferences;
- (void)rdna4IODispatchLocked;
- (void)enqueueBarrier;
@end

@interface RDNA4IOFileHandle (GarnetInternalMethods)
+ (void)initialize;
- (instancetype)initWithDevice:(id)dev url:(NSURL *)url compressionMethod:(NSInteger)method compressed:(BOOL)compressed error:(NSError **)err;
- (NSString *)label;
- (void)setLabel:(NSString *)l;
@end

#endif /* GARNET_IO_INTERNAL_H */
