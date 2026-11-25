#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#define VK_DESCRIPTOR_TYPE_STORAGE_BUFFER 0
#define VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER 1

static void fail(NSString *message) __attribute__((noreturn));
static void fail(NSString *message) {
    fprintf(stderr, "ICB_TRANSACTION_FAIL %s\n", message.UTF8String);
    _exit(1);
}

typedef struct {
    uint64_t minUniformBufferOffsetAlignment;
    uint64_t minStorageBufferOffsetAlignment;
    uint64_t maxUniformBufferRange;
    uint64_t maxStorageBufferRange;
} RDNA4ICBLimits;

static struct {
    BOOL ok;
    RDNA4ICBLimits lim;
} RDNA4R = {
    YES,
    { 1, 1, UINT32_MAX, UINT32_MAX },
};

static _Atomic uint64_t rdna4_uid_ctr;
static uint64_t rdna4_resource_uid(_Atomic uint64_t *uid) {
    uint64_t current = atomic_load_explicit(uid, memory_order_relaxed);
    if (current) return current;
    uint64_t candidate = atomic_fetch_add_explicit(&rdna4_uid_ctr, 1, memory_order_relaxed) + 1;
    uint64_t expected = 0;
    return atomic_compare_exchange_strong_explicit(uid, &expected, candidate,
        memory_order_relaxed, memory_order_relaxed) ? candidate : expected;
}

static void rdna4_log(const char *format, ...) { (void)format; }
#define RDNA4LOG(...) rdna4_log(__VA_ARGS__)
#define RDNA4LOGR(...) rdna4_log(__VA_ARGS__)
#define RDNA4_ONCE(...) do { } while (0)

static NSError *rdna4_err(NSInteger code, NSString *message) {
    return [NSError errorWithDomain:@"RDNA4ICBTransactionTest"
                               code:code
                           userInfo:@{ NSLocalizedDescriptionKey: message ?: @"ICB transaction failure" }];
}

@class RDNA4Buffer;

@interface GarnetDevice : NSObject
@end

@implementation GarnetDevice
@end

@interface RDNA4Buffer : NSObject
- (NSUInteger)length;
- (id<MTLDevice>)device;
- (NSUInteger)rdna4VkOffset;
- (uint64_t)gpuAddress;
@end

@implementation RDNA4Buffer
- (NSUInteger)length { return 4096; }
- (id<MTLDevice>)device { return nil; }
- (NSUInteger)rdna4VkOffset { return 0; }
- (uint64_t)gpuAddress { return 0; }
@end

typedef struct {
    uint32_t binding;
    uint32_t type;
    uint32_t count;
    BOOL addressTable;
} RDNA4PB;

typedef struct {
    __unsafe_unretained RDNA4Buffer *b;
    NSUInteger off;
} RDNA4BufSlot;

typedef struct {
    RDNA4BufSlot buf[31];
} RDNA4StageState;

@interface RDNA4RenderPipelineState : NSObject
- (id)rdna4Real;
- (int)rdna4DSVk;
- (unsigned)rdna4SampleCount;
- (const uint32_t *)needBindings;
- (uint32_t)nNeed;
- (const RDNA4PB *)pbV;
- (uint32_t)npbV;
- (const RDNA4PB *)pbF;
- (uint32_t)npbF;
@end

@implementation RDNA4RenderPipelineState
- (id)rdna4Real { return nil; }
- (int)rdna4DSVk { return 0; }
- (unsigned)rdna4SampleCount { return 1; }
- (const uint32_t *)needBindings { return NULL; }
- (uint32_t)nNeed { return 0; }
- (const RDNA4PB *)pbV { return NULL; }
- (uint32_t)npbV { return 0; }
- (const RDNA4PB *)pbF { return NULL; }
- (uint32_t)npbF { return 0; }
@end

@interface RDNA4CommandBuffer : NSObject {
    GarnetDevice *_dev;
    BOOL (^_orderedHostAction)(NSError **);
    NSError *_failure;
    NSMutableArray *_retained;
}
- (instancetype)initWithDevice:(GarnetDevice *)device;
- (BOOL)rdna4EncodeOrderedHostAction:(BOOL (^)(NSError **))action
                               error:(NSError **)error;
- (BOOL)rdna4HasPendingActionForTest;
- (BOOL)rdna4RunPendingActionForTest:(BOOL *)ran error:(NSError **)error;
- (void)rdna4Retain:(id)object;
- (void)rdna4Fail:(NSString *)reason;
- (NSError *)rdna4Error;
@end

@implementation RDNA4CommandBuffer
- (instancetype)initWithDevice:(GarnetDevice *)device {
    self = [super init];
    if (!self) return nil;
    _dev = device;
    _retained = [NSMutableArray array];
    return self;
}
- (id<MTLDevice>)device { return (id<MTLDevice>)_dev; }
- (MTLCommandBufferStatus)status { return MTLCommandBufferStatusNotEnqueued; }
- (BOOL)rdna4EncodeOrderedHostAction:(BOOL (^)(NSError **))action
                               error:(NSError **)error {
    if (_failure) {
        if (error) *error = _failure;
        return NO;
    }
    if (!action) {
        if (error)
            *error = rdna4_err(200, @"ordered host action is nil");
        return NO;
    }
    if (_orderedHostAction) {
        if (error)
            *error = rdna4_err(201, @"ordered host action was encoded twice without execution");
        return NO;
    }
    _orderedHostAction = [action copy];
    return YES;
}
- (BOOL)rdna4HasPendingActionForTest { return _orderedHostAction != nil; }
- (BOOL)rdna4RunPendingActionForTest:(BOOL *)ran error:(NSError **)error {
    if (ran) *ran = NO;
    if (!_orderedHostAction) return YES;
    BOOL (^action)(NSError **) = _orderedHostAction;
    _orderedHostAction = nil;
    if (ran) *ran = YES;
    @try {
        return action(error);
    } @catch (NSException *exception) {
        if (error)
            *error = rdna4_err(202, [NSString stringWithFormat:@"ordered action raised: %@",
                                     exception.reason ?: @"unknown exception"]);
        return NO;
    }
}
- (void)rdna4Retain:(id)object {
    if (object) [_retained addObject:object];
}
- (void)rdna4Fail:(NSString *)reason {
    if (!_failure) _failure = rdna4_err(203, reason ?: @"command-buffer encoding failed");
}
- (NSError *)rdna4Error { return _failure; }
@end

@interface RDNA4RenderEncoder : NSObject {
@private
    RDNA4CommandBuffer *_cb;
    BOOL _ended;
    BOOL _hostWriteDropped;
    int _dsVkFmt;
    unsigned _encSamples;
    RDNA4RenderPipelineState *_pso;
    MTLRenderPipelineReflection *_roReflection;
    RDNA4StageState _sv;
    RDNA4StageState _sf;
}
- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)pipelineState;
@end

@implementation RDNA4RenderEncoder
- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)pipelineState {
    (void)pipelineState;
}
@end

@interface RDNA4BlitEncoder : NSObject {
@private
    RDNA4CommandBuffer *_cb;
    BOOL _ended;
}
- (instancetype)initWithCommandBuffer:(RDNA4CommandBuffer *)commandBuffer;
@end

@implementation RDNA4BlitEncoder
- (instancetype)initWithCommandBuffer:(RDNA4CommandBuffer *)commandBuffer {
    self = [super init];
    if (!self) return nil;
    _cb = commandBuffer;
    return self;
}
@end

static BOOL rdna4_buffer_view(uint64_t length, uint64_t offset, uint64_t vkOffset,
                              uint64_t alignment, uint64_t limit,
                              uint64_t *viewOffset, uint64_t *viewSize) {
    (void)vkOffset;
    if (!alignment || offset > length) return NO;
    uint64_t start = (offset / alignment) * alignment;
    uint64_t end = length;
    if (limit && end - start > limit) end = start + limit;
    if (end <= start) return NO;
    if (viewOffset) *viewOffset = start;
    if (viewSize) *viewSize = end - start;
    return YES;
}

typedef struct {
    uint64_t address[31];
    uint64_t length[31];
    uint64_t offset[31];
} rdna4_buffer_table;

static BOOL rdna4_buffer_table_set(rdna4_buffer_table *table, uint32_t slot,
                                   uint64_t address, uint64_t length,
                                   uint64_t offset, BOOL readOnly) {
    (void)readOnly;
    if (!table || slot >= 31 || offset > length) return NO;
    table->address[slot] = address;
    table->length[slot] = length;
    table->offset[slot] = offset;
    return YES;
}

#include "garnet_icb.m"

static _Atomic unsigned rdna4_tx_copy_calls;
static _Atomic unsigned rdna4_tx_fail_copy_at;

@interface RDNA4ICBRecord (TransactionFault)
- (RDNA4ICBRecord *)rdna4TestCopy;
@end

@implementation RDNA4ICBRecord (TransactionFault)
- (RDNA4ICBRecord *)rdna4TestCopy {
    unsigned call = atomic_fetch_add(&rdna4_tx_copy_calls, 1) + 1;
    if (atomic_load(&rdna4_tx_fail_copy_at) == call)
        [NSException raise:NSInternalInconsistencyException
                    format:@"injected record-copy failure at %u", call];
    return [self rdna4TestCopy];
}
@end

static void setCopyFault(BOOL enabled) {
    static BOOL installed;
    if (installed == enabled) return;
    Method original = class_getInstanceMethod([RDNA4ICBRecord class], @selector(rdna4Copy));
    Method replacement = class_getInstanceMethod([RDNA4ICBRecord class], @selector(rdna4TestCopy));
    if (!original || !replacement) fail(@"record copy selectors unavailable");
    method_exchangeImplementations(original, replacement);
    installed = enabled;
}

static RDNA4IndirectCommandBuffer *newICBWithTypes(GarnetDevice *device, NSUInteger count,
                                                   MTLIndirectCommandType commandTypes) {
    MTLIndirectCommandBufferDescriptor *descriptor = [MTLIndirectCommandBufferDescriptor new];
    descriptor.commandTypes = commandTypes;
    descriptor.inheritPipelineState = YES;
    descriptor.inheritBuffers = YES;
    descriptor.inheritDepthStencilState = YES;
    descriptor.inheritDepthBias = YES;
    descriptor.inheritDepthClipMode = YES;
    descriptor.inheritCullMode = YES;
    descriptor.inheritFrontFacingWinding = YES;
    descriptor.inheritTriangleFillMode = YES;
    NSError *error = nil;
    RDNA4IndirectCommandBuffer *buffer =
        [[RDNA4IndirectCommandBuffer alloc] initWithDevice:device
                                                descriptor:descriptor
                                          maxCommandCount:count
                                                   options:MTLResourceStorageModePrivate
                                                     error:&error];
    if (!buffer) fail([NSString stringWithFormat:@"ICB creation: %@", error]);
    return buffer;
}

static RDNA4IndirectCommandBuffer *newICB(GarnetDevice *device, NSUInteger count) {
    return newICBWithTypes(device, count, MTLIndirectCommandTypeDraw);
}

static void setDraw(RDNA4IndirectCommandBuffer *buffer, NSUInteger index,
                    NSUInteger vertexCount) {
    [buffer rdna4SetDrawAtIndex:index primitiveType:MTLPrimitiveTypeTriangle
                    vertexStart:0 vertexCount:vertexCount instanceCount:1 baseInstance:0];
    RDNA4ICBRecord *record = [buffer rdna4LockedRecordAtIndex:index];
    if (!record || record.error || record.kind != RDNA4ICBCommandDraw ||
        record.vertexCount != vertexCount)
        fail([NSString stringWithFormat:@"slot %lu did not record draw count %lu: %@",
              (unsigned long)index, (unsigned long)vertexCount, record.error]);
}

static NSUInteger drawCount(RDNA4IndirectCommandBuffer *buffer, NSUInteger index) {
    RDNA4ICBRecord *record = [buffer rdna4LockedRecordAtIndex:index];
    return record && record.kind == RDNA4ICBCommandDraw ? record.vertexCount : 0;
}

static void expectDraws(RDNA4IndirectCommandBuffer *buffer,
                        const NSUInteger *expected, NSUInteger count,
                        NSString *caseName) {
    for (NSUInteger i = 0; i < count; ++i) {
        NSUInteger actual = drawCount(buffer, i);
        if (actual != expected[i])
            fail([NSString stringWithFormat:@"%@ slot %lu is %lu, expected %lu",
                  caseName, (unsigned long)i, (unsigned long)actual,
                  (unsigned long)expected[i]]);
    }
}

static BOOL runAction(RDNA4CommandBuffer *command, NSString *caseName) {
    BOOL ran = NO;
    NSError *error = nil;
    BOOL ok = [command rdna4RunPendingActionForTest:&ran error:&error];
    if (!ran) fail([NSString stringWithFormat:@"%@ had no queued action", caseName]);
    if (!ok) fail([NSString stringWithFormat:@"%@ action failed: %@", caseName, error]);
    return YES;
}

static void expectNoAction(RDNA4CommandBuffer *command, NSString *caseName) {
    BOOL ran = NO;
    NSError *error = nil;
    if (![command rdna4RunPendingActionForTest:&ran error:&error] || ran)
        fail([NSString stringWithFormat:@"%@ unexpectedly had or failed an action: %@",
              caseName, error]);
}

static void testResetTransaction(GarnetDevice *device) {
    RDNA4IndirectCommandBuffer *buffer = newICB(device, 5);
    for (NSUInteger i = 0; i < 4; ++i) setDraw(buffer, i, 100 + i);
    RDNA4CommandBuffer *owner = [[RDNA4CommandBuffer alloc] initWithDevice:device];
    RDNA4CommandBuffer *other = [[RDNA4CommandBuffer alloc] initWithDevice:device];
    RDNA4BlitEncoder *blit = [[RDNA4BlitEncoder alloc] initWithCommandBuffer:owner];

    [blit resetCommandsInBuffer:(id<MTLIndirectCommandBuffer>)buffer
                      withRange:NSMakeRange(1, 2)];
    if ([owner rdna4Error]) fail([NSString stringWithFormat:@"reset encode: %@", [owner rdna4Error]]);
    NSUInteger before[] = { 100, 101, 102, 103, 0 };
    expectDraws(buffer, before, 5, @"reset encode-time");

    // A later CPU setter must not drain or apply the queued GPU-position transaction.
    setDraw(buffer, 4, 104);
    NSUInteger afterSetter[] = { 100, 101, 102, 103, 104 };
    expectDraws(buffer, afterSetter, 5, @"reset after unrelated setter");

    expectNoAction(other, @"reset on another command buffer");
    expectDraws(buffer, afterSetter, 5, @"reset before owner action");
    runAction(owner, @"reset action");
    NSUInteger applied[] = { 100, 0, 0, 103, 104 };
    expectDraws(buffer, applied, 5, @"reset after owner action");
    expectNoAction(owner, @"reset replay");
    expectDraws(buffer, applied, 5, @"reset after replay check");
    printf("ICB_TRANSACTION_CASE_PASS reset encode=unchanged execute=changed cross_cb=unchanged\n");
}

static void testDirectReset(GarnetDevice *device) {
    RDNA4IndirectCommandBuffer *buffer = newICB(device, 4);
    for (NSUInteger i = 0; i < 4; ++i) setDraw(buffer, i, 200 + i);
    [buffer resetWithRange:NSMakeRange(1, 2)];
    NSUInteger expected[] = { 200, 0, 0, 203 };
    expectDraws(buffer, expected, 4, @"direct CPU reset");
    printf("ICB_TRANSACTION_CASE_PASS direct_reset immediate\n");
}

static void testCopyTransaction(GarnetDevice *device) {
    RDNA4IndirectCommandBuffer *source = newICB(device, 4);
    RDNA4IndirectCommandBuffer *destination = newICB(device, 5);
    setDraw(source, 0, 201);
    setDraw(source, 2, 203);
    setDraw(source, 3, 204);
    for (NSUInteger i = 0; i < 5; ++i) setDraw(destination, i, 300 + i);
    RDNA4CommandBuffer *owner = [[RDNA4CommandBuffer alloc] initWithDevice:device];
    RDNA4CommandBuffer *other = [[RDNA4CommandBuffer alloc] initWithDevice:device];
    RDNA4BlitEncoder *blit = [[RDNA4BlitEncoder alloc] initWithCommandBuffer:owner];

    [blit copyIndirectCommandBuffer:(id<MTLIndirectCommandBuffer>)source
                        sourceRange:NSMakeRange(0, 2)
                        destination:(id<MTLIndirectCommandBuffer>)destination
                   destinationIndex:1];
    if ([owner rdna4Error]) fail([NSString stringWithFormat:@"copy encode: %@", [owner rdna4Error]]);
    NSUInteger before[] = { 300, 301, 302, 303, 304 };
    expectDraws(destination, before, 5, @"copy encode-time");

    // The snapshot must come from execution time, not from this encode call.
    [source rdna4ResetOneAtIndex:0];
    setDraw(source, 0, 777);
    expectNoAction(other, @"copy on another command buffer");
    expectDraws(destination, before, 5, @"copy before owner action");
    runAction(owner, @"copy action");
    NSUInteger applied[] = { 300, 777, 0, 303, 304 };
    expectDraws(destination, applied, 5, @"copy after owner action");

    // Published records are deep copies, not aliases of the live source slots.
    [source rdna4ResetOneAtIndex:0];
    setDraw(source, 0, 888);
    expectDraws(destination, applied, 5, @"copy after source mutation");
    expectNoAction(owner, @"copy replay");
    expectDraws(destination, applied, 5, @"copy after replay check");
    printf("ICB_TRANSACTION_CASE_PASS copy snapshot_at_execution encode=unchanged cross_cb=unchanged\n");
}

static void testOverlappingCopy(GarnetDevice *device) {
    RDNA4IndirectCommandBuffer *buffer = newICB(device, 5);
    for (NSUInteger i = 0; i < 5; ++i) setDraw(buffer, i, 400 + i);
    RDNA4CommandBuffer *owner = [[RDNA4CommandBuffer alloc] initWithDevice:device];
    RDNA4BlitEncoder *blit = [[RDNA4BlitEncoder alloc] initWithCommandBuffer:owner];
    [blit copyIndirectCommandBuffer:(id<MTLIndirectCommandBuffer>)buffer
                        sourceRange:NSMakeRange(0, 4)
                        destination:(id<MTLIndirectCommandBuffer>)buffer
                   destinationIndex:1];
    if ([owner rdna4Error]) fail([NSString stringWithFormat:@"overlap encode: %@", [owner rdna4Error]]);
    runAction(owner, @"overlap copy");
    NSUInteger expected[] = { 400, 400, 401, 402, 403 };
    expectDraws(buffer, expected, 5, @"overlapping same-buffer copy");
    printf("ICB_TRANSACTION_CASE_PASS overlapping_copy snapshot_first\n");
}

static void testNoPartialFailure(GarnetDevice *device) {
    RDNA4IndirectCommandBuffer *source = newICB(device, 4);
    RDNA4IndirectCommandBuffer *destination = newICB(device, 4);
    setDraw(source, 0, 501);
    setDraw(source, 1, 502);
    for (NSUInteger i = 0; i < 4; ++i) setDraw(destination, i, 600 + i);
    RDNA4CommandBuffer *owner = [[RDNA4CommandBuffer alloc] initWithDevice:device];
    RDNA4BlitEncoder *blit = [[RDNA4BlitEncoder alloc] initWithCommandBuffer:owner];
    [blit copyIndirectCommandBuffer:(id<MTLIndirectCommandBuffer>)source
                        sourceRange:NSMakeRange(0, 2)
                        destination:(id<MTLIndirectCommandBuffer>)destination
                   destinationIndex:1];
    NSUInteger before[] = { 600, 601, 602, 603 };
    expectDraws(destination, before, 4, @"fault copy before action");

    atomic_store(&rdna4_tx_copy_calls, 0);
    atomic_store(&rdna4_tx_fail_copy_at, 2);
    setCopyFault(YES);
    BOOL ran = NO;
    NSError *error = nil;
    BOOL ok = [owner rdna4RunPendingActionForTest:&ran error:&error];
    setCopyFault(NO);
    atomic_store(&rdna4_tx_fail_copy_at, 0);
    if (!ran || ok || !error)
        fail(@"injected copy failure did not return one terminal action error");
    expectDraws(destination, before, 4, @"fault copy after action");
    expectNoAction(owner, @"fault copy replay");
    expectDraws(destination, before, 4, @"fault copy after replay check");
    printf("ICB_TRANSACTION_CASE_PASS failure_no_partial copied=%u\n",
           atomic_load(&rdna4_tx_copy_calls));
}

static void testStaticValidation(GarnetDevice *device) {
    RDNA4IndirectCommandBuffer *source = newICB(device, 3);
    RDNA4IndirectCommandBuffer *destination = newICB(device, 3);
    for (NSUInteger i = 0; i < 3; ++i) {
        setDraw(source, i, 700 + i);
        setDraw(destination, i, 800 + i);
    }
    RDNA4CommandBuffer *owner = [[RDNA4CommandBuffer alloc] initWithDevice:device];
    RDNA4BlitEncoder *blit = [[RDNA4BlitEncoder alloc] initWithCommandBuffer:owner];
    [blit resetCommandsInBuffer:(id<MTLIndirectCommandBuffer>)destination
                      withRange:NSMakeRange(2, 2)];
    if (![owner rdna4Error]) fail(@"invalid reset range was accepted");
    if ([owner rdna4HasPendingActionForTest]) fail(@"invalid reset queued an action");
    NSUInteger unchanged[] = { 800, 801, 802 };
    expectDraws(destination, unchanged, 3, @"invalid reset");

    owner = [[RDNA4CommandBuffer alloc] initWithDevice:device];
    blit = [[RDNA4BlitEncoder alloc] initWithCommandBuffer:owner];
    [blit copyIndirectCommandBuffer:(id<MTLIndirectCommandBuffer>)source
                        sourceRange:NSMakeRange(1, 2)
                        destination:(id<MTLIndirectCommandBuffer>)destination
                   destinationIndex:2];
    if (![owner rdna4Error]) fail(@"oversized copy range was accepted");
    if ([owner rdna4HasPendingActionForTest]) fail(@"invalid copy queued an action");
    expectDraws(destination, unchanged, 3, @"invalid copy");

    GarnetDevice *otherDevice = [GarnetDevice new];
    owner = [[RDNA4CommandBuffer alloc] initWithDevice:otherDevice];
    blit = [[RDNA4BlitEncoder alloc] initWithCommandBuffer:owner];
    [blit resetCommandsInBuffer:(id<MTLIndirectCommandBuffer>)source
                      withRange:NSMakeRange(0, 1)];
    if (![owner rdna4Error]) fail(@"cross-device reset was accepted");
    if ([owner rdna4HasPendingActionForTest]) fail(@"cross-device reset queued an action");

    RDNA4IndirectCommandBuffer *different =
        newICBWithTypes(device, 3, MTLIndirectCommandTypeDraw | MTLIndirectCommandTypeDrawIndexed);
    for (NSUInteger i = 0; i < 3; ++i) setDraw(different, i, 850 + i);
    owner = [[RDNA4CommandBuffer alloc] initWithDevice:device];
    blit = [[RDNA4BlitEncoder alloc] initWithCommandBuffer:owner];
    [blit copyIndirectCommandBuffer:(id<MTLIndirectCommandBuffer>)source
                        sourceRange:NSMakeRange(0, 1)
                        destination:(id<MTLIndirectCommandBuffer>)different
                   destinationIndex:0];
    if (![owner rdna4Error]) fail(@"cross-descriptor copy was accepted");
    if ([owner rdna4HasPendingActionForTest]) fail(@"cross-descriptor copy queued an action");
    NSUInteger differentExpected[] = { 850, 851, 852 };
    expectDraws(different, differentExpected, 3, @"cross-descriptor copy");
    printf("ICB_TRANSACTION_CASE_PASS static_validation no_action\n");
}

static void testLifetime(GarnetDevice *device) {
    __weak RDNA4IndirectCommandBuffer *weakBuffer = nil;
    RDNA4CommandBuffer *owner = [[RDNA4CommandBuffer alloc] initWithDevice:device];
    @autoreleasepool {
        RDNA4IndirectCommandBuffer *buffer = newICB(device, 2);
        setDraw(buffer, 0, 901);
        weakBuffer = buffer;
        RDNA4BlitEncoder *blit = [[RDNA4BlitEncoder alloc] initWithCommandBuffer:owner];
        [blit resetCommandsInBuffer:(id<MTLIndirectCommandBuffer>)buffer
                          withRange:NSMakeRange(0, 1)];
        buffer = nil;
    }
    if (!weakBuffer) fail(@"queued action did not retain its indirect command buffer");
    runAction(owner, @"lifetime action");
    if (weakBuffer) fail(@"executed action did not release its indirect command buffer");

    __weak RDNA4CommandBuffer *weakOwner = nil;
    __weak RDNA4IndirectCommandBuffer *unexecuted = nil;
    @autoreleasepool {
        RDNA4CommandBuffer *temporary = [[RDNA4CommandBuffer alloc] initWithDevice:device];
        RDNA4IndirectCommandBuffer *buffer = newICB(device, 2);
        setDraw(buffer, 0, 902);
        weakOwner = temporary;
        unexecuted = buffer;
        RDNA4BlitEncoder *blit = [[RDNA4BlitEncoder alloc] initWithCommandBuffer:temporary];
        [blit resetCommandsInBuffer:(id<MTLIndirectCommandBuffer>)buffer
                          withRange:NSMakeRange(0, 1)];
        temporary = nil;
        blit = nil;
        buffer = nil;
    }
    if (weakOwner || unexecuted)
        fail(@"queued action retained its command buffer or leaked its ICB");
    printf("ICB_TRANSACTION_CASE_PASS lifetime captures_icb not_cb\n");
}

int main(void) {
    @autoreleasepool {
        GarnetDevice *device = [GarnetDevice new];
        testResetTransaction(device);
        testDirectReset(device);
        testCopyTransaction(device);
        testOverlappingCopy(device);
        testNoPartialFailure(device);
        testStaticValidation(device);
        testLifetime(device);
        puts("ICB_TRANSACTION_PASS no GPU submission; real ICB transaction methods");
        return 0;
    }
}
