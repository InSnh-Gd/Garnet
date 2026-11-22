#ifndef RDNA4_ICB_INC
#define RDNA4_ICB_INC

// Mac2 requires executeCommandsInBuffer(_:range:) to contain at most
// 0x4000 (16,384) commands; Apple silicon families have no such limit.
// Source: Apple, executeCommandsInBuffer(_:range:) (accessed 2026-11-05):
// https://developer.apple.com/documentation/metal/mtlrendercommandencoder/executecommandsinbuffer(_:range:)
static const NSUInteger RDNA4ICB_MAX_EXECUTION_RANGE = 0x4000;
static const NSUInteger RDNA4ICB_MAX_BUFFER_BINDINGS = 31;
static const NSUInteger RDNA4ICB_MIN_TESS_FACTOR_BYTES = 16;
static const MTLIndirectCommandType RDNA4ICB_TYPE_DRAW_PATCHES = (MTLIndirectCommandType)(1u << 2);
static const MTLIndirectCommandType RDNA4ICB_TYPE_DRAW_INDEXED_PATCHES = (MTLIndirectCommandType)(1u << 3);

typedef NS_ENUM(NSUInteger, RDNA4ICBCommandKind) {
    RDNA4ICBCommandEmpty = 0,
    RDNA4ICBCommandDraw,
    RDNA4ICBCommandDrawIndexed,
    RDNA4ICBCommandDrawPatches,
    RDNA4ICBCommandDrawIndexedPatches,
};

@class RDNA4ICBBind;
@class RDNA4ICBRecord;
@class RDNA4IndirectCommandBuffer;
@class RDNA4IndirectRenderCommand;
@class RDNA4CommandBuffer;

@interface RDNA4CommandBuffer (RDNA4OrderedHostAction)
- (id<MTLDevice>)device;
- (BOOL)rdna4EncodeOrderedHostAction:(BOOL (^)(NSError **))action error:(NSError **)error;
@end

@interface RDNA4RenderEncoder (RDNA4IndirectCommands)
- (id)rdna4ICBDevice;
- (RDNA4CommandBuffer *)rdna4ICBCommandBuffer;
- (RDNA4RenderPipelineState *)rdna4ICBPipeline;
- (BOOL)rdna4ICBCompatiblePipeline:(id)pipeline;
- (BOOL)rdna4ICBInheritedBuffersSatisfyPipeline:(id)pipeline;
- (BOOL)rdna4ICBExplicitBuffersSatisfyRecord:(RDNA4ICBRecord *)record pipeline:(id)pipeline;
- (void)rdna4ICBClearExplicitBuffers;
- (void)rdna4ICBRetainResource:(id)resource;
@end

@interface RDNA4ICBBind : NSObject
@property(nonatomic, strong) RDNA4Buffer *buffer;
@property(nonatomic) NSUInteger offset;
- (instancetype)initWithBuffer:(RDNA4Buffer *)buffer offset:(NSUInteger)offset;
@end

@implementation RDNA4ICBBind
- (instancetype)initWithBuffer:(RDNA4Buffer *)buffer offset:(NSUInteger)offset {
    self = [super init];
    if (!self) return nil;
    _buffer = buffer;
    _offset = offset;
    return self;
}
@end

@interface RDNA4ICBRecord : NSObject
@property(nonatomic) RDNA4ICBCommandKind kind;
@property(nonatomic, copy) NSString *error;
@property(nonatomic, strong) RDNA4RenderPipelineState *pipeline;
@property(nonatomic, strong) NSMutableDictionary<NSNumber *, RDNA4ICBBind *> *vertexBinds;
@property(nonatomic, strong) NSMutableDictionary<NSNumber *, RDNA4ICBBind *> *fragmentBinds;
@property(nonatomic) MTLPrimitiveType primitiveType;
@property(nonatomic) NSUInteger vertexStart;
@property(nonatomic) NSUInteger vertexCount;
@property(nonatomic) NSUInteger instanceCount;
@property(nonatomic) NSUInteger baseInstance;
@property(nonatomic) NSUInteger indexCount;
@property(nonatomic) MTLIndexType indexType;
@property(nonatomic, strong) RDNA4Buffer *indexBuffer;
@property(nonatomic) NSUInteger indexBufferOffset;
@property(nonatomic) NSInteger baseVertex;
@property(nonatomic) NSUInteger patchControlPoints;
@property(nonatomic) NSUInteger patchStart;
@property(nonatomic) NSUInteger patchCount;
@property(nonatomic, strong) RDNA4Buffer *patchIndexBuffer;
@property(nonatomic) NSUInteger patchIndexBufferOffset;
@property(nonatomic, strong) RDNA4Buffer *controlPointIndexBuffer;
@property(nonatomic) NSUInteger controlPointIndexBufferOffset;
@property(nonatomic, strong) RDNA4Buffer *tessellationFactorBuffer;
@property(nonatomic) NSUInteger tessellationFactorBufferOffset;
@property(nonatomic) NSUInteger tessellationFactorInstanceStride;
- (void)reset;
- (void)reject:(NSString *)reason;
- (RDNA4ICBRecord *)rdna4Copy;
@end

@implementation RDNA4ICBRecord
- (instancetype)init {
    self = [super init];
    if (!self) return nil;
    _vertexBinds = [NSMutableDictionary dictionary];
    _fragmentBinds = [NSMutableDictionary dictionary];
    return self;
}
- (void)reset {
#define RDNA4_ICB_RESET_VALUE(field, value) do { field = (value); } while (0)
    RDNA4_ICB_RESET_VALUE(_kind, RDNA4ICBCommandEmpty);
    _error = nil;
    _pipeline = nil;
    [_vertexBinds removeAllObjects];
    [_fragmentBinds removeAllObjects];
    RDNA4_ICB_RESET_VALUE(_primitiveType, MTLPrimitiveTypePoint);
    RDNA4_ICB_RESET_VALUE(_vertexStart, 0);
    RDNA4_ICB_RESET_VALUE(_vertexCount, 0);
    RDNA4_ICB_RESET_VALUE(_instanceCount, 0);
    RDNA4_ICB_RESET_VALUE(_baseInstance, 0);
    RDNA4_ICB_RESET_VALUE(_indexCount, 0);
    RDNA4_ICB_RESET_VALUE(_indexType, MTLIndexTypeUInt16);
    _indexBuffer = nil;
    RDNA4_ICB_RESET_VALUE(_indexBufferOffset, 0);
    RDNA4_ICB_RESET_VALUE(_baseVertex, 0);
    RDNA4_ICB_RESET_VALUE(_patchControlPoints, 0);
    RDNA4_ICB_RESET_VALUE(_patchStart, 0);
    RDNA4_ICB_RESET_VALUE(_patchCount, 0);
    _patchIndexBuffer = nil;
    RDNA4_ICB_RESET_VALUE(_patchIndexBufferOffset, 0);
    _controlPointIndexBuffer = nil;
    RDNA4_ICB_RESET_VALUE(_controlPointIndexBufferOffset, 0);
    _tessellationFactorBuffer = nil;
    RDNA4_ICB_RESET_VALUE(_tessellationFactorBufferOffset, 0);
    RDNA4_ICB_RESET_VALUE(_tessellationFactorInstanceStride, 0);
#undef RDNA4_ICB_RESET_VALUE
}
- (void)reject:(NSString *)reason {
    if (!_error) _error = [(reason ?: @"invalid indirect render command") copy];
}
- (RDNA4ICBRecord *)rdna4Copy {
    RDNA4ICBRecord *copy = [RDNA4ICBRecord new];
    copy->_kind = _kind;
    copy->_error = [_error copy];
    copy->_pipeline = _pipeline;
    copy->_vertexBinds = [_vertexBinds mutableCopy];
    copy->_fragmentBinds = [_fragmentBinds mutableCopy];
    copy->_primitiveType = _primitiveType;
    copy->_vertexStart = _vertexStart;
    copy->_vertexCount = _vertexCount;
    copy->_instanceCount = _instanceCount;
    copy->_baseInstance = _baseInstance;
    copy->_indexCount = _indexCount;
    copy->_indexType = _indexType;
    copy->_indexBuffer = _indexBuffer;
    copy->_indexBufferOffset = _indexBufferOffset;
    copy->_baseVertex = _baseVertex;
    copy->_patchControlPoints = _patchControlPoints;
    copy->_patchStart = _patchStart;
    copy->_patchCount = _patchCount;
    copy->_patchIndexBuffer = _patchIndexBuffer;
    copy->_patchIndexBufferOffset = _patchIndexBufferOffset;
    copy->_controlPointIndexBuffer = _controlPointIndexBuffer;
    copy->_controlPointIndexBufferOffset = _controlPointIndexBufferOffset;
    copy->_tessellationFactorBuffer = _tessellationFactorBuffer;
    copy->_tessellationFactorBufferOffset = _tessellationFactorBufferOffset;
    copy->_tessellationFactorInstanceStride = _tessellationFactorInstanceStride;
    return copy;
}
@end

@interface RDNA4IndirectCommandBuffer : NSObject {
@private
    id _dev;
    NSString *_label;
    MTLResourceOptions _options;
    MTLPurgeableState _purge;
    NSUInteger _commandCount;
    NSUInteger _encodedSize;
    _Atomic uint64_t _uid;
    MTLIndirectCommandBufferDescriptor *_descriptor;
    MTLIndirectCommandType _commandTypes;
    BOOL _inheritPipelineState;
    BOOL _inheritBuffers;
    NSUInteger _maxVertexBufferBindCount;
    NSUInteger _maxFragmentBufferBindCount;
    NSMutableDictionary<NSNumber *, RDNA4ICBRecord *> *_records;
    NSMutableDictionary<NSNumber *, RDNA4IndirectRenderCommand *> *_commands;
    RDNA4ICBRecord *_emptyRecord;
    RDNA4IndirectRenderCommand *_invalidCommand;
    NSString *_fatalError;
    pthread_mutex_t _mu;
    BOOL _muReady;
}
- (instancetype)initWithDevice:(id)device
                    descriptor:(MTLIndirectCommandBufferDescriptor *)descriptor
              maxCommandCount:(NSUInteger)maxCommandCount
                       options:(MTLResourceOptions)options
                         error:(NSError **)error;
- (NSUInteger)size;
- (RDNA4ICBRecord *)rdna4LockedRecordAtIndex:(NSUInteger)index;
- (void)rdna4SetPipelineState:(id)pipeline atIndex:(NSUInteger)index;
- (void)rdna4SetBuffer:(id)buffer
                offset:(NSUInteger)offset
           bufferIndex:(NSUInteger)bufferIndex
          commandIndex:(NSUInteger)commandIndex
              fragment:(BOOL)fragment;
- (void)rdna4SetDrawAtIndex:(NSUInteger)index
               primitiveType:(MTLPrimitiveType)primitiveType
                 vertexStart:(NSUInteger)vertexStart
                 vertexCount:(NSUInteger)vertexCount
               instanceCount:(NSUInteger)instanceCount
                baseInstance:(NSUInteger)baseInstance;
- (void)rdna4SetIndexedDrawAtIndex:(NSUInteger)index
                      primitiveType:(MTLPrimitiveType)primitiveType
                         indexCount:(NSUInteger)indexCount
                          indexType:(MTLIndexType)indexType
                        indexBuffer:(id)indexBuffer
                  indexBufferOffset:(NSUInteger)indexBufferOffset
                      instanceCount:(NSUInteger)instanceCount
                         baseVertex:(NSInteger)baseVertex
                       baseInstance:(NSUInteger)baseInstance;
- (void)rdna4SetPatchDrawAtIndex:(NSUInteger)index
                 patchControlPoints:(NSUInteger)patchControlPoints
                        patchStart:(NSUInteger)patchStart
                        patchCount:(NSUInteger)patchCount
                    patchIndexBuffer:(id)patchIndexBuffer
              patchIndexBufferOffset:(NSUInteger)patchIndexBufferOffset
                     instanceCount:(NSUInteger)instanceCount
                      baseInstance:(NSUInteger)baseInstance
            tessellationFactorBuffer:(id)tessellationFactorBuffer
      tessellationFactorBufferOffset:(NSUInteger)tessellationFactorBufferOffset
    tessellationFactorBufferInstanceStride:(NSUInteger)tessellationFactorBufferInstanceStride
                           indexed:(BOOL)indexed
            controlPointIndexBuffer:(id)controlPointIndexBuffer
      controlPointIndexBufferOffset:(NSUInteger)controlPointIndexBufferOffset;
- (BOOL)rdna4RecordCanCommitDraw:(RDNA4ICBRecord *)record
                        commandType:(MTLIndirectCommandType)type
                            problem:(NSString **)problem;
- (void)rdna4ResetOneAtIndex:(NSUInteger)index;
- (BOOL)rdna4RecordResetRange:(NSRange)range
                commandBuffer:(RDNA4CommandBuffer *)commandBuffer
                       reason:(NSString **)reason;
- (BOOL)rdna4RecordCopyRange:(NSRange)sourceRange
                ofBuffer:(RDNA4IndirectCommandBuffer *)source
           toDestination:(RDNA4IndirectCommandBuffer *)destination
        destinationIndex:(NSUInteger)destinationIndex
               commandBuffer:(RDNA4CommandBuffer *)commandBuffer
                  reason:(NSString **)reason;
- (BOOL)rdna4ValidateRange:(NSRange)range reason:(NSString **)reason;
- (BOOL)rdna4PublishResetRangeLocked:(NSRange)range error:(NSError **)error;
- (BOOL)rdna4PublishCopyRangeLocked:(NSRange)sourceRange
                           toBuffer:(RDNA4IndirectCommandBuffer *)destination
                   destinationIndex:(NSUInteger)destinationIndex
                              error:(NSError **)error;
- (BOOL)rdna4ApplyResetRangeAtExecution:(NSRange)range error:(NSError **)error;
- (BOOL)rdna4ApplyCopyRangeAtExecution:(NSRange)sourceRange
                                toBuffer:(RDNA4IndirectCommandBuffer *)destination
                        destinationIndex:(NSUInteger)destinationIndex
                                   error:(NSError **)error;
- (NSUInteger)rdna4ExecuteOnEncoder:(RDNA4RenderEncoder *)encoder
                              range:(NSRange)range
                             reason:(NSString **)reason;
- (BOOL)rdna4SameConfigurationAs:(RDNA4IndirectCommandBuffer *)other;
@end

@interface RDNA4IndirectRenderCommand : NSObject {
@private
    __weak RDNA4IndirectCommandBuffer *_owner;
    NSUInteger _index;
    BOOL _validIndex;
}
- (instancetype)initWithOwner:(RDNA4IndirectCommandBuffer *)owner index:(NSUInteger)index valid:(BOOL)valid;
- (void)rdna4Reject:(NSString *)reason;
- (BOOL)rdna4CanMutate;
@end

static NSString *rdna4icb_range_problem(NSRange range, NSUInteger count, BOOL execution) {
    if (range.location > count || range.length > count - range.location)
        return [NSString stringWithFormat:@"range [%lu,+%lu) exceeds %lu command slot(s)",
                (unsigned long)range.location, (unsigned long)range.length, (unsigned long)count];
    if (execution && range.length > RDNA4ICB_MAX_EXECUTION_RANGE)
        return [NSString stringWithFormat:@"range length %lu exceeds the Mac2 limit of %lu commands",
                (unsigned long)range.length, (unsigned long)RDNA4ICB_MAX_EXECUTION_RANGE];
    return nil;
}

static NSError *rdna4icb_transaction_error(NSString *reason) {
    return rdna4_err(121, reason ?: @"indirect command buffer transaction failed");
}

static void rdna4icb_order_pair(RDNA4IndirectCommandBuffer *a,
                                RDNA4IndirectCommandBuffer *b,
                                RDNA4IndirectCommandBuffer **first,
                                RDNA4IndirectCommandBuffer **second) {
    uintptr_t aa = (uintptr_t)(__bridge void *)a;
    uintptr_t bb = (uintptr_t)(__bridge void *)b;
    *first = aa <= bb ? a : b;
    *second = aa <= bb ? b : a;
}

static BOOL rdna4icb_primitive_ok(MTLPrimitiveType type) {
    return type >= MTLPrimitiveTypePoint && type <= MTLPrimitiveTypeTriangleStrip;
}

static BOOL rdna4icb_u32_range_ok(NSUInteger start, NSUInteger count) {
    if (start > UINT32_MAX || count > UINT32_MAX) return NO;
    return count == 0 || start <= UINT32_MAX - (count - 1);
}

static BOOL rdna4icb_buffer_range_ok(id buffer, id device, NSUInteger offset,
                                    NSUInteger elementSize, NSUInteger elementCount,
                                    BOOL required) {
    if (!buffer) return !required;
    if (![buffer isKindOfClass:[RDNA4Buffer class]] || [(RDNA4Buffer *)buffer device] != device ||
        !elementSize || offset % elementSize) return NO;
    NSUInteger length = [(RDNA4Buffer *)buffer length];
    return offset <= length && elementCount <= (length - offset) / elementSize;
}

static BOOL rdna4icb_strided_range_ok(id buffer, id device, NSUInteger offset, NSUInteger stride,
                                     NSUInteger first, NSUInteger count, NSUInteger recordBytes) {
    if (![buffer isKindOfClass:[RDNA4Buffer class]] || [(RDNA4Buffer *)buffer device] != device) return NO;
    NSUInteger length = [(RDNA4Buffer *)buffer length];
    if (offset > length) return NO;
    if (!count) return YES;
    if (stride < recordBytes || !recordBytes || first > (NSUIntegerMax - offset) / stride) return NO;
    NSUInteger record = offset + first * stride;
    if (record > length || recordBytes > length - record) return NO;
    if (count <= 1) return YES;
    return count - 1 <= (length - record - recordBytes) / stride;
}

static BOOL rdna4icb_buffer_slot_view_ok(RDNA4Buffer *buffer, NSUInteger offset, BOOL uniform) {
    if (!buffer) return NO;
    uint64_t alignment = 1;
    uint64_t limit = UINT32_MAX;
    if (RDNA4R.ok) {
        alignment = uniform ? RDNA4R.lim.minUniformBufferOffsetAlignment
                            : RDNA4R.lim.minStorageBufferOffsetAlignment;
        limit = uniform ? RDNA4R.lim.maxUniformBufferRange
                        : RDNA4R.lim.maxStorageBufferRange;
    }
    uint64_t viewOffset = 0, viewSize = 0;
    return rdna4_buffer_view(buffer.length, offset,
                             [buffer rdna4VkOffset], alignment, limit,
                             &viewOffset, &viewSize);
}

static BOOL rdna4icb_stage_buffers_ok(const RDNA4PB *bindings, uint32_t count,
                                      const RDNA4StageState *state, id device) {
    for (uint32_t i = 0; i < count; ++i) {
        const RDNA4PB *binding = &bindings[i];
        if (binding->type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER &&
            binding->type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) continue;
        if (binding->addressTable) {
            for (uint32_t slot = 0; slot < 31; ++slot) {
                RDNA4Buffer *buffer = state->buf[slot].b;
                if (!buffer) continue;
                rdna4_buffer_table table = {0};
                if ([buffer device] != device ||
                    ![buffer gpuAddress] ||
                    !rdna4_buffer_table_set(&table, slot, [buffer gpuAddress],
                                            buffer.length, state->buf[slot].off, NO))
                    return NO;
            }
            continue;
        }
        if (binding->binding >= 31) return NO;
        BOOL uniform = binding->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        for (uint32_t element = 0; element < binding->count; ++element) {
            NSUInteger slot = binding->binding + element;
            if (slot >= 31) continue;
            RDNA4Buffer *buffer = state->buf[slot].b;
            if (buffer &&
                (!rdna4icb_buffer_slot_view_ok(buffer, state->buf[slot].off, uniform) ||
                 [buffer device] != device))
                return NO;
        }
    }
    return YES;
}

static BOOL rdna4icb_pipeline_supports_icb(id pipeline) {
    SEL selector = @selector(supportIndirectCommandBuffers);
    typedef BOOL (*RDNA4ICBBoolMsg)(id, SEL);
    RDNA4ICBBoolMsg fn = (RDNA4ICBBoolMsg)(void *)objc_msgSend;
    return pipeline && [pipeline respondsToSelector:selector] && fn(pipeline, selector);
}

static BOOL rdna4icb_read_bool(id object, SEL selector, BOOL *value) {
    if (!object || ![object respondsToSelector:selector]) return NO;
    typedef BOOL (*RDNA4ICBBoolMsg)(id, SEL);
    RDNA4ICBBoolMsg fn = (RDNA4ICBBoolMsg)(void *)objc_msgSend;
    *value = fn(object, selector);
    return YES;
}

static NSString *rdna4icb_descriptor_problem(MTLIndirectCommandBufferDescriptor *descriptor) {
    if (!descriptor) return @"nil indirect command buffer descriptor";
    // This object is the CPU draw/indexed baseline. Patch and compute command
    // types stay unavailable until their execution paths are wired.
    const MTLIndirectCommandType supported = MTLIndirectCommandTypeDraw | MTLIndirectCommandTypeDrawIndexed;
    if (!descriptor.commandTypes) return @"descriptor.v1 commandTypes is empty";
    if (descriptor.commandTypes & ~supported)
        return [NSString stringWithFormat:@"commandTypes 0x%lx include patch, compute, mesh or unknown commands outside the CPU draw/indexed ICB baseline",
                (unsigned long)descriptor.commandTypes];
    if (descriptor.maxVertexBufferBindCount > RDNA4ICB_MAX_BUFFER_BINDINGS ||
        descriptor.maxFragmentBufferBindCount > RDNA4ICB_MAX_BUFFER_BINDINGS)
        return [NSString stringWithFormat:@"buffer bind counts %lu/%lu exceed the Metal 2 limit of %lu",
                (unsigned long)descriptor.maxVertexBufferBindCount,
                (unsigned long)descriptor.maxFragmentBufferBindCount,
                (unsigned long)RDNA4ICB_MAX_BUFFER_BINDINGS];
    BOOL value = NO;
    const SEL inherited[] = {
        @selector(inheritDepthStencilState), @selector(inheritDepthBias),
        @selector(inheritDepthClipMode), @selector(inheritCullMode),
        @selector(inheritFrontFacingWinding), @selector(inheritTriangleFillMode),
    };
    for (size_t i = 0; i < sizeof inherited / sizeof inherited[0]; ++i) {
        if (rdna4icb_read_bool(descriptor, inherited[i], &value) && !value)
            return @"per-command render state inherited from the encoder is outside the fixed Metal 2 ICB baseline";
    }
    const SEL forbidden[] = {
        @selector(supportRayTracing), @selector(supportDynamicAttributeStride),
        @selector(supportColorAttachmentMapping),
    };
    for (size_t i = 0; i < sizeof forbidden / sizeof forbidden[0]; ++i)
        if (rdna4icb_read_bool(descriptor, forbidden[i], &value) && value)
            return @"ray tracing, dynamic attribute stride or color-attachment mapping is outside the fixed Metal 2 ICB baseline";
    return nil;
}

static BOOL rdna4icb_descriptor_equal(MTLIndirectCommandBufferDescriptor *a,
                                     MTLIndirectCommandBufferDescriptor *b) {
    if (!a || !b || a.commandTypes != b.commandTypes ||
        a.inheritPipelineState != b.inheritPipelineState ||
        a.inheritBuffers != b.inheritBuffers) return NO;
    if (!a.inheritBuffers &&
        (a.maxVertexBufferBindCount != b.maxVertexBufferBindCount ||
         a.maxFragmentBufferBindCount != b.maxFragmentBufferBindCount)) return NO;
    const SEL selectors[] = {
        @selector(inheritDepthStencilState), @selector(inheritDepthBias),
        @selector(inheritDepthClipMode), @selector(inheritCullMode),
        @selector(inheritFrontFacingWinding), @selector(inheritTriangleFillMode),
    };
    for (size_t i = 0; i < sizeof selectors / sizeof selectors[0]; ++i) {
        BOOL av = NO, bv = NO;
        BOOL ah = rdna4icb_read_bool(a, selectors[i], &av);
        BOOL bh = rdna4icb_read_bool(b, selectors[i], &bv);
        if (ah != bh || (ah && av != bv)) return NO;
    }
    return YES;
}

@implementation RDNA4IndirectRenderCommand
- (instancetype)initWithOwner:(RDNA4IndirectCommandBuffer *)owner index:(NSUInteger)index valid:(BOOL)valid {
    self = [super init];
    if (!self) return nil;
    _owner = owner;
    _index = index;
    _validIndex = valid;
    return self;
}
- (void)rdna4Reject:(NSString *)reason {
    RDNA4_ONCE("indirectRenderCommandAtIndex: invalid index or command mutation rejected: %s", reason.UTF8String);
}
- (BOOL)rdna4CanMutate {
    if (_validIndex && _owner) return YES;
    [self rdna4Reject:[NSString stringWithFormat:@"command index %lu is outside its indirect command buffer", (unsigned long)_index]];
    return NO;
}
- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)pipelineState {
    if ([self rdna4CanMutate]) [_owner rdna4SetPipelineState:pipelineState atIndex:_index];
}
- (void)setVertexBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index {
    if ([self rdna4CanMutate])
        [_owner rdna4SetBuffer:buffer offset:offset bufferIndex:index commandIndex:_index fragment:NO];
}
- (void)setFragmentBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index {
    if ([self rdna4CanMutate])
        [_owner rdna4SetBuffer:buffer offset:offset bufferIndex:index commandIndex:_index fragment:YES];
}
- (void)setVertexBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset
        attributeStride:(NSUInteger)stride atIndex:(NSUInteger)index {
    (void)buffer; (void)offset; (void)stride; (void)index;
    [self rdna4Reject:@"dynamic vertex attribute stride is outside the Metal 2 ICB baseline"];
}
- (void)setObjectThreadgroupMemoryLength:(NSUInteger)length atIndex:(NSUInteger)index {
    (void)length; (void)index;
    [self rdna4Reject:@"object threadgroup memory is outside the Metal 2 ICB baseline"];
}
- (void)setObjectBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index {
    (void)buffer; (void)offset; (void)index;
    [self rdna4Reject:@"object-stage buffers are outside the Metal 2 ICB baseline"];
}
- (void)setMeshBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index {
    (void)buffer; (void)offset; (void)index;
    [self rdna4Reject:@"mesh-stage buffers are outside the Metal 2 ICB baseline"];
}
- (void)drawMeshThreadgroups:(MTLSize)threadgroupsPerGrid
 threadsPerObjectThreadgroup:(MTLSize)threadsPerObjectThreadgroup
   threadsPerMeshThreadgroup:(MTLSize)threadsPerMeshThreadgroup {
    (void)threadgroupsPerGrid; (void)threadsPerObjectThreadgroup; (void)threadsPerMeshThreadgroup;
    [self rdna4Reject:@"mesh ICB draw is outside the Metal 2 ICB baseline"];
}
- (void)drawMeshThreads:(MTLSize)threadsPerGrid
 threadsPerObjectThreadgroup:(MTLSize)threadsPerObjectThreadgroup
   threadsPerMeshThreadgroup:(MTLSize)threadsPerMeshThreadgroup {
    (void)threadsPerGrid; (void)threadsPerObjectThreadgroup; (void)threadsPerMeshThreadgroup;
    [self rdna4Reject:@"mesh ICB draw is outside the Metal 2 ICB baseline"];
}
- (void)setBarrier { [self rdna4Reject:@"ICB barriers are outside the Metal 2 baseline"]; }
- (void)clearBarrier { [self rdna4Reject:@"ICB barriers are outside the Metal 2 baseline"]; }
- (void)setDepthStencilState:(id<MTLDepthStencilState>)depthStencilState {
    (void)depthStencilState;
    [self rdna4Reject:@"per-command depth/stencil state is outside the Metal 2 baseline"];
}
- (void)setDepthBias:(float)depthBias slopeScale:(float)slopeScale clamp:(float)clamp {
    (void)depthBias; (void)slopeScale; (void)clamp;
    [self rdna4Reject:@"per-command depth bias is outside the Metal 2 baseline"];
}
- (void)setDepthClipMode:(MTLDepthClipMode)depthClipMode {
    (void)depthClipMode;
    [self rdna4Reject:@"per-command depth clip mode is outside the Metal 2 baseline"];
}
- (void)setCullMode:(MTLCullMode)cullMode {
    (void)cullMode;
    [self rdna4Reject:@"per-command cull mode is outside the Metal 2 baseline"];
}
- (void)setFrontFacingWinding:(MTLWinding)frontFacingWinding {
    (void)frontFacingWinding;
    [self rdna4Reject:@"per-command winding is outside the Metal 2 baseline"];
}
- (void)setTriangleFillMode:(MTLTriangleFillMode)fillMode {
    (void)fillMode;
    [self rdna4Reject:@"per-command triangle fill mode is outside the Metal 2 baseline"];
}
- (void)drawPrimitives:(MTLPrimitiveType)primitiveType vertexStart:(NSUInteger)vertexStart
           vertexCount:(NSUInteger)vertexCount instanceCount:(NSUInteger)instanceCount
          baseInstance:(NSUInteger)baseInstance {
    if ([self rdna4CanMutate])
        [_owner rdna4SetDrawAtIndex:_index primitiveType:primitiveType vertexStart:vertexStart
                        vertexCount:vertexCount instanceCount:instanceCount baseInstance:baseInstance];
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)primitiveType indexCount:(NSUInteger)indexCount
                     indexType:(MTLIndexType)indexType indexBuffer:(id<MTLBuffer>)indexBuffer
           indexBufferOffset:(NSUInteger)indexBufferOffset instanceCount:(NSUInteger)instanceCount
                  baseVertex:(NSInteger)baseVertex baseInstance:(NSUInteger)baseInstance {
    if ([self rdna4CanMutate])
        [_owner rdna4SetIndexedDrawAtIndex:_index primitiveType:primitiveType indexCount:indexCount
                                indexType:indexType indexBuffer:indexBuffer
                        indexBufferOffset:indexBufferOffset instanceCount:instanceCount
                               baseVertex:baseVertex baseInstance:baseInstance];
}
- (void)drawPatches:(NSUInteger)numberOfPatchControlPoints patchStart:(NSUInteger)patchStart
           patchCount:(NSUInteger)patchCount patchIndexBuffer:(id<MTLBuffer>)patchIndexBuffer
 patchIndexBufferOffset:(NSUInteger)patchIndexBufferOffset instanceCount:(NSUInteger)instanceCount
         baseInstance:(NSUInteger)baseInstance tessellationFactorBuffer:(id<MTLBuffer>)buffer
tessellationFactorBufferOffset:(NSUInteger)offset
tessellationFactorBufferInstanceStride:(NSUInteger)instanceStride {
    if ([self rdna4CanMutate])
        [_owner rdna4SetPatchDrawAtIndex:_index patchControlPoints:numberOfPatchControlPoints
                             patchStart:patchStart patchCount:patchCount patchIndexBuffer:patchIndexBuffer
                   patchIndexBufferOffset:patchIndexBufferOffset instanceCount:instanceCount
                            baseInstance:baseInstance tessellationFactorBuffer:buffer
             tessellationFactorBufferOffset:offset
       tessellationFactorBufferInstanceStride:instanceStride indexed:NO
                controlPointIndexBuffer:nil controlPointIndexBufferOffset:0];
}
- (void)drawIndexedPatches:(NSUInteger)numberOfPatchControlPoints patchStart:(NSUInteger)patchStart
                patchCount:(NSUInteger)patchCount patchIndexBuffer:(id<MTLBuffer>)patchIndexBuffer
    patchIndexBufferOffset:(NSUInteger)patchIndexBufferOffset
      controlPointIndexBuffer:(id<MTLBuffer>)controlPointIndexBuffer
controlPointIndexBufferOffset:(NSUInteger)controlPointIndexBufferOffset
             instanceCount:(NSUInteger)instanceCount baseInstance:(NSUInteger)baseInstance
  tessellationFactorBuffer:(id<MTLBuffer>)buffer tessellationFactorBufferOffset:(NSUInteger)offset
tessellationFactorBufferInstanceStride:(NSUInteger)instanceStride {
    if ([self rdna4CanMutate])
        [_owner rdna4SetPatchDrawAtIndex:_index patchControlPoints:numberOfPatchControlPoints
                             patchStart:patchStart patchCount:patchCount patchIndexBuffer:patchIndexBuffer
                   patchIndexBufferOffset:patchIndexBufferOffset instanceCount:instanceCount
                            baseInstance:baseInstance tessellationFactorBuffer:buffer
             tessellationFactorBufferOffset:offset
       tessellationFactorBufferInstanceStride:instanceStride indexed:YES
                controlPointIndexBuffer:controlPointIndexBuffer
      controlPointIndexBufferOffset:controlPointIndexBufferOffset];
}
- (void)reset {
    if ([self rdna4CanMutate]) [_owner rdna4ResetOneAtIndex:_index];
}
@end

@implementation RDNA4IndirectCommandBuffer
- (instancetype)initWithDevice:(id)device
                    descriptor:(MTLIndirectCommandBufferDescriptor *)descriptor
              maxCommandCount:(NSUInteger)maxCommandCount
                       options:(MTLResourceOptions)options
                         error:(NSError **)error {
    self = [super init];
    if (!self) return nil;
    NSString *problem = rdna4icb_descriptor_problem(descriptor);
    unsigned storage = (unsigned)((options >> MTLResourceStorageModeShift) & 0xF);
    if (!problem && storage > MTLStorageModePrivate)
        problem = [NSString stringWithFormat:@"unsupported indirect command buffer storage mode %u", storage];
    if (!problem && !maxCommandCount)
        problem = @"maxCommandCount is zero";
    if (!problem && maxCommandCount > UINT32_MAX)
        problem = [NSString stringWithFormat:@"maxCommandCount %lu does not fit an indirect execution range", (unsigned long)maxCommandCount];
    NSUInteger slotBytes = class_getInstanceSize([RDNA4ICBRecord class]);
    if (!slotBytes) slotBytes = 1;
    if (!problem && maxCommandCount > NSUIntegerMax / slotBytes)
        problem = @"indirect command buffer size overflows NSUInteger";
    if (problem) {
        if (error) *error = rdna4_err(120, problem);
        return nil;
    }
    if (pthread_mutex_init(&_mu, NULL) != 0) {
        if (error) *error = rdna4_err(120, @"pthread_mutex_init for indirect command buffer failed");
        return nil;
    }
    _muReady = YES;
    _dev = device;
    _options = options;
    _purge = MTLPurgeableStateNonVolatile;
    _commandCount = maxCommandCount;
    // Host bookkeeping extent. The CPU ICB is not an Apple-format binary
    // command stream, so callers must not use this as an ABI layout promise.
    // Slot storage is sparse; maxCommandCount is a logical capacity, not a
    // request to allocate one ObjC record and command object per slot.
    _encodedSize = maxCommandCount * slotBytes;
    @try {
        _descriptor = [descriptor copy];
        _records = [NSMutableDictionary dictionary];
        _commands = [NSMutableDictionary dictionary];
        _emptyRecord = [RDNA4ICBRecord new];
        _invalidCommand = [[RDNA4IndirectRenderCommand alloc] initWithOwner:nil
                                                                    index:NSUIntegerMax
                                                                    valid:NO];
        if (!_descriptor || !_records || !_commands || !_emptyRecord || !_invalidCommand)
            [NSException raise:NSMallocException format:@"indirect command buffer bookkeeping allocation failed"];
    } @catch (NSException *exception) {
        if (error) *error = rdna4_err(120, exception.reason ?: @"indirect command buffer allocation failed");
        return nil;
    }
    _commandTypes = descriptor.commandTypes;
    _inheritPipelineState = descriptor.inheritPipelineState;
    _inheritBuffers = descriptor.inheritBuffers;
    _maxVertexBufferBindCount = descriptor.inheritBuffers ? 0 : descriptor.maxVertexBufferBindCount;
    _maxFragmentBufferBindCount = descriptor.inheritBuffers ? 0 : descriptor.maxFragmentBufferBindCount;
    return self;
}
- (void)dealloc {
    if (_muReady) pthread_mutex_destroy(&_mu);
}
- (id<MTLDevice>)device { return _dev; }
- (NSString *)label { return _label; }
- (void)setLabel:(NSString *)label { _label = [label copy]; }
- (NSUInteger)size { return _encodedSize; }
- (NSUInteger)allocatedSize { return _encodedSize; }
- (MTLStorageMode)storageMode { return (MTLStorageMode)((_options >> MTLResourceStorageModeShift) & 0xF); }
- (MTLCPUCacheMode)cpuCacheMode { return (MTLCPUCacheMode)(_options & 0xF); }
- (MTLHazardTrackingMode)hazardTrackingMode { return (MTLHazardTrackingMode)((_options >> MTLResourceHazardTrackingModeShift) & 0x3); }
- (NSUInteger)resourceOptions { return _options; }
- (NSUInteger)unfilteredResourceOptions { return _options; }
- (id<MTLHeap>)heap { return nil; }
- (NSUInteger)heapOffset { return 0; }
- (BOOL)isAliasable { return NO; }
- (void)makeAliasable {}
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state {
    pthread_mutex_lock(&_mu);
    MTLPurgeableState previous = _purge;
    if (state >= MTLPurgeableStateKeepCurrent && state <= MTLPurgeableStateEmpty) _purge = state;
    pthread_mutex_unlock(&_mu);
    return previous;
}
- (BOOL)isPurgeable { return _purge != MTLPurgeableStateNonVolatile && _purge != MTLPurgeableStateKeepCurrent; }
- (BOOL)isComplete { return YES; }
- (BOOL)isWriteComplete { return YES; }
- (void)waitUntilComplete {}
- (NSUInteger)protectionOptions { return 0; }
- (uint64_t)uniqueIdentifier { return rdna4_resource_uid(&_uid); }
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 130000
- (MTLResourceID)gpuResourceID API_AVAILABLE(macos(13.0)) {
    MTLResourceID identifier;
    identifier._impl = self.uniqueIdentifier;
    return identifier;
}
#endif
- (id<MTLIndirectRenderCommand>)indirectRenderCommandAtIndex:(NSUInteger)index {
    if (index >= _commandCount) {
        RDNA4LOG("indirectRenderCommandAtIndex:%lu is outside %lu command slot(s)",
                 (unsigned long)index, (unsigned long)_commandCount);
        return (id<MTLIndirectRenderCommand>)_invalidCommand;
    }
    NSNumber *key = @(index);
    pthread_mutex_lock(&_mu);
    RDNA4IndirectRenderCommand *command = _commands[key];
    if (!command && !_fatalError) {
        @try {
            command = [[RDNA4IndirectRenderCommand alloc] initWithOwner:self index:index valid:YES];
            if (command) _commands[key] = command;
        } @catch (NSException *exception) {
            _fatalError = [NSString stringWithFormat:@"indirect command slot %lu allocation failed: %@",
                           (unsigned long)index, exception.reason ?: @"out of memory"];
        }
    }
    pthread_mutex_unlock(&_mu);
    return (id<MTLIndirectRenderCommand>)(command ?: _invalidCommand);
}
- (id<MTLIndirectComputeCommand>)indirectComputeCommandAtIndex:(NSUInteger)index {
    (void)index;
    RDNA4_ONCE("indirectComputeCommandAtIndex: compute ICBs are outside the fixed Metal 2 render baseline");
    return nil;
}
- (RDNA4ICBRecord *)rdna4LockedRecordAtIndex:(NSUInteger)index {
    if (index >= _commandCount || _fatalError) return nil;
    NSNumber *key = @(index);
    RDNA4ICBRecord *record = _records[key];
    if (record) return record;
    @try {
        record = [RDNA4ICBRecord new];
        if (record) _records[key] = record;
    } @catch (NSException *exception) {
        _fatalError = [NSString stringWithFormat:@"indirect command slot %lu allocation failed: %@",
                       (unsigned long)index, exception.reason ?: @"out of memory"];
    }
    return record;
}
- (void)rdna4SetPipelineState:(id)pipeline atIndex:(NSUInteger)index {
    pthread_mutex_lock(&_mu);
    RDNA4ICBRecord *record = [self rdna4LockedRecordAtIndex:index];
    NSString *problem = nil;
    if (!record) problem = @"slot is outside the indirect command buffer";
    else if (record.error) problem = record.error;
    else if (record.kind != RDNA4ICBCommandEmpty) problem = @"render state cannot be changed after a draw in the same ICB slot";
    else if (_inheritPipelineState) problem = @"this ICB inherits its pipeline from the parent encoder";
    else if (![pipeline isKindOfClass:[RDNA4RenderPipelineState class]]) problem = @"pipeline is not an RDNA4RenderPipelineState";
    else if ([(id<MTLRenderPipelineState>)pipeline device] != _dev) problem = @"pipeline belongs to another device";
    else if (!rdna4icb_pipeline_supports_icb(pipeline)) problem = @"pipeline does not support indirect command buffers";
    if (problem) [record reject:problem];
    else record.pipeline = pipeline;
    pthread_mutex_unlock(&_mu);
}
- (void)rdna4SetBuffer:(id)buffer
                offset:(NSUInteger)offset
           bufferIndex:(NSUInteger)bufferIndex
          commandIndex:(NSUInteger)commandIndex
              fragment:(BOOL)fragment {
    pthread_mutex_lock(&_mu);
    RDNA4ICBRecord *record = [self rdna4LockedRecordAtIndex:commandIndex];
    NSUInteger limit = fragment ? _maxFragmentBufferBindCount : _maxVertexBufferBindCount;
    NSString *problem = nil;
    if (!record) problem = @"slot is outside the indirect command buffer";
    else if (record.error) problem = record.error;
    else if (record.kind != RDNA4ICBCommandEmpty) problem = @"buffer bindings cannot be changed after a draw in the same ICB slot";
    else if (_inheritBuffers) problem = @"this ICB inherits buffer bindings from the parent encoder";
    else if (bufferIndex >= limit) problem = [NSString stringWithFormat:@"%s buffer index %lu exceeds max bind count %lu",
                                              fragment ? "fragment" : "vertex", (unsigned long)bufferIndex, (unsigned long)limit];
    else if (![buffer isKindOfClass:[RDNA4Buffer class]]) problem = @"buffer is not an RDNA4Buffer";
    else if ([(RDNA4Buffer *)buffer device] != _dev) problem = @"buffer belongs to another device";
    else if (offset > [(RDNA4Buffer *)buffer length]) problem = @"buffer offset is outside the logical resource";
    if (problem) [record reject:problem];
    else {
        NSMutableDictionary<NSNumber *, RDNA4ICBBind *> *binds = fragment ? record.fragmentBinds : record.vertexBinds;
        @try {
            binds[@(bufferIndex)] = [[RDNA4ICBBind alloc] initWithBuffer:buffer offset:offset];
        } @catch (NSException *exception) {
            [record reject:[NSString stringWithFormat:@"buffer binding allocation failed: %@",
                            exception.reason ?: @"out of memory"]];
        }
    }
    pthread_mutex_unlock(&_mu);
}
- (BOOL)rdna4RecordCanCommitDraw:(RDNA4ICBRecord *)record
                        commandType:(MTLIndirectCommandType)type
                            problem:(NSString **)problem {
    if (!record) { *problem = @"slot is outside the indirect command buffer"; return NO; }
    if (record.error) { *problem = record.error; return NO; }
    if (record.kind != RDNA4ICBCommandEmpty) { *problem = @"an ICB slot accepts only one draw command"; return NO; }
    if (!(_commandTypes & type)) {
        *problem = [NSString stringWithFormat:@"this ICB descriptor does not permit command type 0x%lx", (unsigned long)type];
        return NO;
    }
    if (!_inheritPipelineState && !record.pipeline) {
        *problem = @"draw has no pipeline and this ICB does not inherit one";
        return NO;
    }
    return YES;
}
- (void)rdna4SetDrawAtIndex:(NSUInteger)index
               primitiveType:(MTLPrimitiveType)primitiveType
                 vertexStart:(NSUInteger)vertexStart
                 vertexCount:(NSUInteger)vertexCount
               instanceCount:(NSUInteger)instanceCount
                baseInstance:(NSUInteger)baseInstance {
    pthread_mutex_lock(&_mu);
    RDNA4ICBRecord *record = [self rdna4LockedRecordAtIndex:index];
    NSString *problem = nil;
    if (![self rdna4RecordCanCommitDraw:record commandType:MTLIndirectCommandTypeDraw problem:&problem]) {
        [record reject:problem];
    } else if (!rdna4icb_primitive_ok(primitiveType)) {
        problem = @"draw primitive type is invalid";
    } else if (!rdna4icb_u32_range_ok(vertexStart, vertexCount) ||
               !rdna4icb_u32_range_ok(baseInstance, instanceCount)) {
        problem = @"vertex or instance range exceeds Vulkan's 32-bit draw limits";
    }
    if (problem && record) {
        [record reject:problem];
    } else if (!problem) {
        record.kind = RDNA4ICBCommandDraw;
        record.primitiveType = primitiveType;
        record.vertexStart = vertexStart;
        record.vertexCount = vertexCount;
        record.instanceCount = instanceCount;
        record.baseInstance = baseInstance;
    }
    pthread_mutex_unlock(&_mu);
}
- (void)rdna4SetIndexedDrawAtIndex:(NSUInteger)index
                      primitiveType:(MTLPrimitiveType)primitiveType
                         indexCount:(NSUInteger)indexCount
                          indexType:(MTLIndexType)indexType
                        indexBuffer:(id)indexBuffer
                  indexBufferOffset:(NSUInteger)indexBufferOffset
                      instanceCount:(NSUInteger)instanceCount
                         baseVertex:(NSInteger)baseVertex
                       baseInstance:(NSUInteger)baseInstance {
    pthread_mutex_lock(&_mu);
    RDNA4ICBRecord *record = [self rdna4LockedRecordAtIndex:index];
    NSString *problem = nil;
    if (![self rdna4RecordCanCommitDraw:record commandType:MTLIndirectCommandTypeDrawIndexed problem:&problem]) {
        [record reject:problem];
    } else if (!rdna4icb_primitive_ok(primitiveType)) {
        problem = @"indexed draw primitive type is invalid";
    } else if ([indexBuffer isKindOfClass:[RDNA4Buffer class]] == NO) {
        problem = @"index buffer is not an RDNA4Buffer";
    } else if ([(RDNA4Buffer *)indexBuffer device] != _dev) {
        problem = @"index buffer belongs to another device";
    } else if (indexType != MTLIndexTypeUInt16 && indexType != MTLIndexTypeUInt32) {
        problem = @"index type is neither UInt16 nor UInt32";
    } else {
        NSUInteger width = indexType == MTLIndexTypeUInt32 ? 4 : 2;
        NSUInteger length = [(RDNA4Buffer *)indexBuffer length];
        if (indexBufferOffset > length || (indexBufferOffset % width) ||
            indexCount > (length - indexBufferOffset) / width)
            problem = @"index buffer offset, alignment or count is outside the logical resource";
        else if (!rdna4icb_u32_range_ok(baseInstance, instanceCount) ||
                 baseVertex < INT32_MIN || baseVertex > INT32_MAX)
            problem = @"indexed draw range exceeds Vulkan's 32-bit limits";
    }
    if (problem && record) {
        [record reject:problem];
    } else if (!problem) {
        record.kind = RDNA4ICBCommandDrawIndexed;
        record.primitiveType = primitiveType;
        record.indexCount = indexCount;
        record.indexType = indexType;
        record.indexBuffer = indexBuffer;
        record.indexBufferOffset = indexBufferOffset;
        record.instanceCount = instanceCount;
        record.baseVertex = baseVertex;
        record.baseInstance = baseInstance;
    }
    pthread_mutex_unlock(&_mu);
}
- (void)rdna4SetPatchDrawAtIndex:(NSUInteger)index
                 patchControlPoints:(NSUInteger)patchControlPoints
                        patchStart:(NSUInteger)patchStart
                        patchCount:(NSUInteger)patchCount
                    patchIndexBuffer:(id)patchIndexBuffer
              patchIndexBufferOffset:(NSUInteger)patchIndexBufferOffset
                     instanceCount:(NSUInteger)instanceCount
                      baseInstance:(NSUInteger)baseInstance
            tessellationFactorBuffer:(id)tessellationFactorBuffer
      tessellationFactorBufferOffset:(NSUInteger)tessellationFactorBufferOffset
    tessellationFactorBufferInstanceStride:(NSUInteger)tessellationFactorBufferInstanceStride
                           indexed:(BOOL)indexed
            controlPointIndexBuffer:(id)controlPointIndexBuffer
      controlPointIndexBufferOffset:(NSUInteger)controlPointIndexBufferOffset {
    pthread_mutex_lock(&_mu);
    RDNA4ICBRecord *record = [self rdna4LockedRecordAtIndex:index];
    MTLIndirectCommandType type = indexed ? RDNA4ICB_TYPE_DRAW_INDEXED_PATCHES : RDNA4ICB_TYPE_DRAW_PATCHES;
    NSString *problem = nil;
    if (![self rdna4RecordCanCommitDraw:record commandType:type problem:&problem]) {
        [record reject:problem];
    } else if (!patchControlPoints) {
        problem = @"patch control point count is zero";
    } else if (!rdna4icb_u32_range_ok(patchStart, patchCount) ||
               !rdna4icb_u32_range_ok(baseInstance, instanceCount)) {
        problem = @"patch or instance range exceeds Vulkan's 32-bit draw limits";
    } else {
        NSUInteger patchEnd = patchStart + patchCount;
        NSUInteger controlPointCount = 0;
        if (indexed && patchEnd && patchControlPoints > NSUIntegerMax / patchEnd) {
            problem = @"control-point index count overflows NSUInteger";
        } else if (indexed) {
            controlPointCount = patchEnd * patchControlPoints;
        }
        if (!problem && !rdna4icb_buffer_range_ok(patchIndexBuffer, _dev, patchIndexBufferOffset,
                                                  2, patchEnd, NO))
            problem = @"patch index buffer, device, range or alignment is invalid";
        else if (!problem && !rdna4icb_buffer_range_ok(tessellationFactorBuffer, _dev,
                                                       tessellationFactorBufferOffset, 4, 0, YES))
            problem = @"tessellation factor buffer, device or alignment is invalid";
        else if (!problem && !rdna4icb_strided_range_ok(tessellationFactorBuffer, _dev,
                                                        tessellationFactorBufferOffset,
                                                        tessellationFactorBufferInstanceStride,
                                                        baseInstance, instanceCount,
                                                        RDNA4ICB_MIN_TESS_FACTOR_BYTES))
            problem = @"tessellation factor instance stride or range is outside the logical resource";
        else if (!problem && indexed &&
                 !rdna4icb_buffer_range_ok(controlPointIndexBuffer, _dev,
                                           controlPointIndexBufferOffset, 2, controlPointCount, YES))
            problem = @"control-point index buffer, device, range or alignment is invalid";
    }
    if (problem && record) {
        [record reject:problem];
    } else if (!problem) {
        record.kind = indexed ? RDNA4ICBCommandDrawIndexedPatches : RDNA4ICBCommandDrawPatches;
        record.patchControlPoints = patchControlPoints;
        record.patchStart = patchStart;
        record.patchCount = patchCount;
        record.patchIndexBuffer = patchIndexBuffer;
        record.patchIndexBufferOffset = patchIndexBufferOffset;
        record.instanceCount = instanceCount;
        record.baseInstance = baseInstance;
        record.tessellationFactorBuffer = tessellationFactorBuffer;
        record.tessellationFactorBufferOffset = tessellationFactorBufferOffset;
        record.tessellationFactorInstanceStride = tessellationFactorBufferInstanceStride;
        record.controlPointIndexBuffer = controlPointIndexBuffer;
        record.controlPointIndexBufferOffset = controlPointIndexBufferOffset;
    }
    pthread_mutex_unlock(&_mu);
}
- (void)rdna4ResetOneAtIndex:(NSUInteger)index {
    pthread_mutex_lock(&_mu);
    RDNA4ICBRecord *record = index < _commandCount ? _records[@(index)] : nil;
    if (record) [record reset];
    pthread_mutex_unlock(&_mu);
}
- (BOOL)rdna4PublishResetRangeLocked:(NSRange)range error:(NSError **)error {
    @try {
        NSMutableDictionary<NSNumber *, RDNA4ICBRecord *> *replacement =
            [NSMutableDictionary dictionary];
        for (NSNumber *key in _records) {
            NSUInteger index = key.unsignedIntegerValue;
            if (index >= range.location && index - range.location < range.length) continue;
            replacement[key] = _records[key];
        }
        _records = replacement;
        if (range.location == 0 && range.length == _commandCount) _fatalError = nil;
        return YES;
    } @catch (NSException *exception) {
        if (error)
            *error = rdna4icb_transaction_error(
                [NSString stringWithFormat:@"reset transaction allocation failed: %@",
                 exception.reason ?: @"out of memory"]);
        return NO;
    }
}

- (BOOL)rdna4PublishCopyRangeLocked:(NSRange)sourceRange
                           toBuffer:(RDNA4IndirectCommandBuffer *)destination
                   destinationIndex:(NSUInteger)destinationIndex
                              error:(NSError **)error {
    @try {
        // Snapshot the complete source range first.  In particular, a
        // same-buffer overlapping copy must not observe records overwritten by
        // its own destination replacement.
        NSMutableDictionary<NSNumber *, RDNA4ICBRecord *> *copies =
            [NSMutableDictionary dictionary];
        for (NSNumber *key in _records) {
            NSUInteger index = key.unsignedIntegerValue;
            if (index < sourceRange.location ||
                index - sourceRange.location >= sourceRange.length) continue;
            copies[@(index - sourceRange.location)] = [_records[key] rdna4Copy];
        }

        // Build the complete replacement off to the side.  Only assigning the
        // finished dictionary publishes the copy.
        NSMutableDictionary<NSNumber *, RDNA4ICBRecord *> *replacement =
            [NSMutableDictionary dictionary];
        NSUInteger destinationEnd = destinationIndex + sourceRange.length;
        for (NSNumber *key in destination->_records) {
            NSUInteger index = key.unsignedIntegerValue;
            if (index >= destinationIndex && index < destinationEnd) continue;
            replacement[key] = destination->_records[key];
        }
        for (NSNumber *offset in copies)
            replacement[@(destinationIndex + offset.unsignedIntegerValue)] = copies[offset];
        destination->_records = replacement;
        return YES;
    } @catch (NSException *exception) {
        if (error)
            *error = rdna4icb_transaction_error(
                [NSString stringWithFormat:@"copy transaction allocation failed: %@",
                 exception.reason ?: @"out of memory"]);
        return NO;
    }
}

- (BOOL)rdna4ApplyResetRangeAtExecution:(NSRange)range error:(NSError **)error {
    pthread_mutex_lock(&_mu);
    NSString *problem = rdna4icb_range_problem(range, _commandCount, NO);
    BOOL ok = problem ? NO : [self rdna4PublishResetRangeLocked:range error:error];
    pthread_mutex_unlock(&_mu);
    if (problem && error) *error = rdna4icb_transaction_error(problem);
    return ok;
}

- (BOOL)rdna4ApplyCopyRangeAtExecution:(NSRange)sourceRange
                                toBuffer:(RDNA4IndirectCommandBuffer *)destination
                        destinationIndex:(NSUInteger)destinationIndex
                                   error:(NSError **)error {
    if (!destination) {
        if (error) *error = rdna4icb_transaction_error(@"copy destination was released");
        return NO;
    }
    if (![self rdna4SameConfigurationAs:destination]) {
        if (error) *error = rdna4icb_transaction_error(
            @"source and destination indirect command buffer configuration changed");
        return NO;
    }

    RDNA4IndirectCommandBuffer *first = nil, *second = nil;
    rdna4icb_order_pair(self, destination, &first, &second);
    pthread_mutex_lock(&first->_mu);
    if (second != first) pthread_mutex_lock(&second->_mu);
    NSString *problem = rdna4icb_range_problem(sourceRange, _commandCount, NO);
    if (!problem && destinationIndex > destination->_commandCount)
        problem = @"destination index is outside the indirect command buffer";
    else if (!problem && sourceRange.length > destination->_commandCount - destinationIndex)
        problem = @"source range does not fit at the destination index";
    else if (!problem && _fatalError)
        problem = _fatalError;
    else if (!problem && destination->_fatalError)
        problem = destination->_fatalError;
    BOOL ok = NO;
    if (!problem)
        ok = [self rdna4PublishCopyRangeLocked:sourceRange
                                     toBuffer:destination
                             destinationIndex:destinationIndex
                                        error:error];
    if (second != first) pthread_mutex_unlock(&second->_mu);
    pthread_mutex_unlock(&first->_mu);
    if (problem && error) *error = rdna4icb_transaction_error(problem);
    return ok;
}

- (BOOL)rdna4RecordResetRange:(NSRange)range
                commandBuffer:(RDNA4CommandBuffer *)commandBuffer
                       reason:(NSString **)reason {
    if (!commandBuffer) {
        if (reason) *reason = @"reset has no command buffer owner";
        return NO;
    }
    if ([commandBuffer device] != _dev) {
        if (reason) *reason = @"reset command buffer belongs to a different device";
        return NO;
    }
    pthread_mutex_lock(&_mu);
    NSString *problem = rdna4icb_range_problem(range, _commandCount, NO);
    pthread_mutex_unlock(&_mu);
    if (problem) {
        if (reason) *reason = problem;
        return NO;
    }
    if (!range.length) return YES;

    NSError *placementError = nil;
    BOOL queued = [commandBuffer rdna4EncodeOrderedHostAction:^BOOL(NSError **error) {
        return [self rdna4ApplyResetRangeAtExecution:range error:error];
    } error:&placementError];
    if (!queued && reason)
        *reason = placementError.localizedDescription ?: @"cannot place the reset transaction";
    return queued;
}

- (void)resetWithRange:(NSRange)range {
    pthread_mutex_lock(&_mu);
    NSString *problem = rdna4icb_range_problem(range, _commandCount, NO);
    if (!problem) {
        NSError *transactionError = nil;
        if (![self rdna4PublishResetRangeLocked:range error:&transactionError])
            problem = transactionError.localizedDescription ?: @"reset transaction failed";
        else
            problem = nil;
    } else {
        _fatalError = [problem copy];
    }
    pthread_mutex_unlock(&_mu);
    if (problem)
        RDNA4LOG("indirect command buffer resetRange [%lu,+%lu) refused: %s",
                 (unsigned long)range.location, (unsigned long)range.length, problem.UTF8String);
}
- (BOOL)rdna4SameConfigurationAs:(RDNA4IndirectCommandBuffer *)other {
    return other && _dev == other->_dev && rdna4icb_descriptor_equal(_descriptor, other->_descriptor);
}
- (BOOL)rdna4ValidateRange:(NSRange)range reason:(NSString **)reason {
    pthread_mutex_lock(&_mu);
    NSString *problem = rdna4icb_range_problem(range, _commandCount, NO);
    if (!problem && _fatalError) problem = _fatalError;
    if (problem && reason) *reason = problem;
    pthread_mutex_unlock(&_mu);
    return problem == nil;
}
- (BOOL)rdna4RecordCopyRange:(NSRange)sourceRange
                ofBuffer:(RDNA4IndirectCommandBuffer *)source
           toDestination:(RDNA4IndirectCommandBuffer *)destination
        destinationIndex:(NSUInteger)destinationIndex
               commandBuffer:(RDNA4CommandBuffer *)commandBuffer
                  reason:(NSString **)reason {
    if (!source || !destination) {
        if (reason) *reason = @"copy needs two indirect command buffers";
        return NO;
    }
    if (!commandBuffer) {
        if (reason) *reason = @"copy has no command buffer owner";
        return NO;
    }
    if ([commandBuffer device] != source->_dev) {
        if (reason) *reason = @"copy command buffer belongs to a different device";
        return NO;
    }
    if (![source rdna4SameConfigurationAs:destination]) {
        if (reason) *reason = @"source and destination indirect command buffers have different descriptors or devices";
        return NO;
    }
    RDNA4IndirectCommandBuffer *first = nil, *second = nil;
    rdna4icb_order_pair(source, destination, &first, &second);
    pthread_mutex_lock(&first->_mu);
    if (second != first) pthread_mutex_lock(&second->_mu);
    NSString *problem = rdna4icb_range_problem(sourceRange, source->_commandCount, NO);
    if (!problem && destinationIndex > destination->_commandCount) {
        problem = @"destination index is outside the indirect command buffer";
    } else if (!problem && sourceRange.length > destination->_commandCount - destinationIndex) {
        problem = @"source range does not fit at the destination index";
    } else if (!problem && source->_fatalError) {
        problem = source->_fatalError;
    } else if (!problem && destination->_fatalError) {
        problem = destination->_fatalError;
    }
    if (second != first) pthread_mutex_unlock(&second->_mu);
    pthread_mutex_unlock(&first->_mu);
    if (problem && reason) *reason = problem;
    if (problem || !sourceRange.length) return problem == nil;

    NSError *placementError = nil;
    BOOL queued = [commandBuffer rdna4EncodeOrderedHostAction:^BOOL(NSError **error) {
        return [source rdna4ApplyCopyRangeAtExecution:sourceRange
                                              toBuffer:destination
                                      destinationIndex:destinationIndex
                                                 error:error];
    } error:&placementError];
    if (!queued && reason)
        *reason = placementError.localizedDescription ?: @"cannot place the copy transaction";
    return queued;
}
- (NSUInteger)rdna4ExecuteOnEncoder:(RDNA4RenderEncoder *)encoder
                              range:(NSRange)range
                             reason:(NSString **)reason {
    NSMutableArray<RDNA4ICBRecord *> *snapshot = nil;
    NSString *problem = nil;
    pthread_mutex_lock(&_mu);
    problem = rdna4icb_range_problem(range, _commandCount, YES);
    if (!problem && _fatalError) problem = _fatalError;
    if (!problem && [encoder rdna4ICBDevice] != _dev)
        problem = @"indirect command buffer belongs to a different device";
    if (!problem) {
        @try {
            snapshot = [NSMutableArray arrayWithCapacity:range.length];
            for (NSUInteger i = range.location; i < range.location + range.length; ++i) {
                RDNA4ICBRecord *record = _records[@(i)];
                [snapshot addObject:record ? [record rdna4Copy] : _emptyRecord];
            }
        } @catch (NSException *exception) {
            problem = [NSString stringWithFormat:@"execution snapshot allocation failed: %@",
                       exception.reason ?: @"out of memory"];
            snapshot = nil;
        }
    }
    pthread_mutex_unlock(&_mu);

    // Validate the whole snapshot before replaying its first command.
    for (NSUInteger offset = 0; !problem && offset < snapshot.count; ++offset) {
        RDNA4ICBRecord *record = snapshot[offset];
        NSUInteger slot = range.location + offset;
        if (record.error) {
            problem = [NSString stringWithFormat:@"slot %lu: %@", (unsigned long)slot, record.error];
            break;
        }
        if (record.kind == RDNA4ICBCommandEmpty) continue;
        switch (record.kind) {
            case RDNA4ICBCommandDraw:
                if (!(_commandTypes & MTLIndirectCommandTypeDraw))
                    problem = [NSString stringWithFormat:@"slot %lu uses a draw the descriptor did not enable", (unsigned long)slot];
                break;
            case RDNA4ICBCommandDrawIndexed:
                if (!(_commandTypes & MTLIndirectCommandTypeDrawIndexed))
                    problem = [NSString stringWithFormat:@"slot %lu uses an indexed draw the descriptor did not enable", (unsigned long)slot];
                break;
            case RDNA4ICBCommandDrawPatches:
            case RDNA4ICBCommandDrawIndexedPatches:
                problem = [NSString stringWithFormat:@"slot %lu is a patch command; tessellation replay is not connected yet", (unsigned long)slot];
                break;
            case RDNA4ICBCommandEmpty:
                break;
        }
        if (problem) break;
        id pipeline = _inheritPipelineState ? [encoder rdna4ICBPipeline] : record.pipeline;
        if (!rdna4icb_pipeline_supports_icb(pipeline)) {
            problem = [NSString stringWithFormat:@"slot %lu has no ICB-capable %s pipeline",
                       (unsigned long)slot, _inheritPipelineState ? "inherited" : "explicit"];
            break;
        }
        if (![encoder rdna4ICBCompatiblePipeline:pipeline]) {
            problem = [NSString stringWithFormat:@"slot %lu pipeline is incompatible with the render pass", (unsigned long)slot];
            break;
        }
        if (_inheritBuffers) {
            if (![encoder rdna4ICBInheritedBuffersSatisfyPipeline:pipeline]) {
                problem = [NSString stringWithFormat:@"slot %lu inherits buffers that do not satisfy its pipeline", (unsigned long)slot];
                break;
            }
        } else if (![encoder rdna4ICBExplicitBuffersSatisfyRecord:record pipeline:pipeline]) {
            problem = [NSString stringWithFormat:@"slot %lu explicit buffers do not satisfy its pipeline", (unsigned long)slot];
            break;
        }
    }

    if (problem) {
        if (reason) *reason = problem;
        return 0;
    }

    for (RDNA4ICBRecord *record in snapshot) {
        [encoder rdna4ICBRetainResource:record.pipeline];
        for (RDNA4ICBBind *bind in record.vertexBinds.allValues)
            [encoder rdna4ICBRetainResource:bind.buffer];
        for (RDNA4ICBBind *bind in record.fragmentBinds.allValues)
            [encoder rdna4ICBRetainResource:bind.buffer];
        [encoder rdna4ICBRetainResource:record.indexBuffer];
        [encoder rdna4ICBRetainResource:record.patchIndexBuffer];
        [encoder rdna4ICBRetainResource:record.controlPointIndexBuffer];
        [encoder rdna4ICBRetainResource:record.tessellationFactorBuffer];
    }

    NSUInteger replayed = 0;
    id<MTLRenderCommandEncoder> direct = (id<MTLRenderCommandEncoder>)encoder;
    for (RDNA4ICBRecord *record in snapshot) {
        if (record.kind == RDNA4ICBCommandEmpty) continue;
        if (!_inheritPipelineState) [direct setRenderPipelineState:(id)record.pipeline];
        if (!_inheritBuffers) {
            [encoder rdna4ICBClearExplicitBuffers];
            for (NSNumber *slot in record.vertexBinds) {
                RDNA4ICBBind *bind = record.vertexBinds[slot];
                [direct setVertexBuffer:(id)bind.buffer offset:bind.offset atIndex:slot.unsignedIntegerValue];
            }
            for (NSNumber *slot in record.fragmentBinds) {
                RDNA4ICBBind *bind = record.fragmentBinds[slot];
                [direct setFragmentBuffer:(id)bind.buffer offset:bind.offset atIndex:slot.unsignedIntegerValue];
            }
        }
        if (record.kind == RDNA4ICBCommandDraw) {
            if (record.vertexCount && record.instanceCount)
                [direct drawPrimitives:record.primitiveType vertexStart:record.vertexStart
                           vertexCount:record.vertexCount instanceCount:record.instanceCount
                          baseInstance:record.baseInstance];
        } else if (record.kind == RDNA4ICBCommandDrawIndexed) {
            if (record.indexCount && record.instanceCount)
                [direct drawIndexedPrimitives:record.primitiveType indexCount:record.indexCount
                                    indexType:record.indexType indexBuffer:(id)record.indexBuffer
                            indexBufferOffset:record.indexBufferOffset instanceCount:record.instanceCount
                                   baseVertex:record.baseVertex baseInstance:record.baseInstance];
        }
        replayed++;
    }
    return replayed;
}
@end

@implementation RDNA4RenderEncoder (RDNA4IndirectCommands)
- (id)rdna4ICBDevice { return [_cb device]; }
- (RDNA4CommandBuffer *)rdna4ICBCommandBuffer { return _cb; }
- (RDNA4RenderPipelineState *)rdna4ICBPipeline { return _pso; }
- (BOOL)rdna4ICBCompatiblePipeline:(id)pipeline {
    if (![pipeline isKindOfClass:[RDNA4RenderPipelineState class]]) return NO;
    RDNA4RenderPipelineState *real = [(RDNA4RenderPipelineState *)pipeline rdna4Real];
    RDNA4RenderPipelineState *use = real ?: pipeline;
    return [use rdna4DSVk] == _dsVkFmt && [use rdna4SampleCount] == _encSamples;
}
- (BOOL)rdna4ICBInheritedBuffersSatisfyPipeline:(id)pipeline {
    if (![pipeline isKindOfClass:[RDNA4RenderPipelineState class]]) return NO;
    RDNA4RenderPipelineState *use = [(RDNA4RenderPipelineState *)pipeline rdna4Real] ?: pipeline;
    const uint32_t *needed = [use needBindings];
    uint32_t count = [use nNeed];
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t index = needed[i];
        if (index == 31) continue;
        if (index >= 31 || !_sv.buf[index].b || _sv.buf[index].off > [_sv.buf[index].b length]) return NO;
    }
    id device = [self rdna4ICBDevice];
    return rdna4icb_stage_buffers_ok([use pbV], [use npbV], &_sv, device) &&
           rdna4icb_stage_buffers_ok([use pbF], [use npbF], &_sf, device);
}
- (BOOL)rdna4ICBExplicitBuffersSatisfyRecord:(RDNA4ICBRecord *)record pipeline:(id)pipeline {
    if (![pipeline isKindOfClass:[RDNA4RenderPipelineState class]]) return NO;
    RDNA4RenderPipelineState *use = [(RDNA4RenderPipelineState *)pipeline rdna4Real] ?: pipeline;
    id device = [self rdna4ICBDevice];
    RDNA4StageState vertex = {0};
    RDNA4StageState fragment = {0};
    for (NSNumber *slot in record.vertexBinds) {
        NSUInteger index = slot.unsignedIntegerValue;
        RDNA4ICBBind *bind = record.vertexBinds[slot];
        if (index >= 31 || !bind.buffer || [bind.buffer device] != device ||
            bind.offset > bind.buffer.length)
            return NO;
        vertex.buf[index].b = bind.buffer;
        vertex.buf[index].off = bind.offset;
    }
    for (NSNumber *slot in record.fragmentBinds) {
        NSUInteger index = slot.unsignedIntegerValue;
        RDNA4ICBBind *bind = record.fragmentBinds[slot];
        if (index >= 31 || !bind.buffer || [bind.buffer device] != device ||
            bind.offset > bind.buffer.length)
            return NO;
        fragment.buf[index].b = bind.buffer;
        fragment.buf[index].off = bind.offset;
    }
    const uint32_t *needed = [use needBindings];
    uint32_t count = [use nNeed];
    for (uint32_t i = 0; i < count; ++i)
        if (needed[i] != 31 && !record.vertexBinds[@(needed[i])]) return NO;
    return rdna4icb_stage_buffers_ok([use pbV], [use npbV], &vertex, device) &&
           rdna4icb_stage_buffers_ok([use pbF], [use npbF], &fragment, device);
}
- (void)rdna4ICBClearExplicitBuffers {
    for (NSUInteger i = 0; i < 31; ++i) {
        _sv.buf[i].b = nil;
        _sv.buf[i].off = 0;
        _sf.buf[i].b = nil;
        _sf.buf[i].off = 0;
    }
}
- (void)rdna4ICBRetainResource:(id)resource { [_cb rdna4Retain:resource]; }
- (void)executeCommandsInBuffer:(id<MTLIndirectCommandBuffer>)indirectCommandBuffer withRange:(NSRange)executionRange {
    if (_hostWriteDropped) return;
    if (_ended || [_cb rdna4Error]) {
        [_cb rdna4Fail:@"executeCommandsInBuffer after endEncoding or after an earlier encode error"];
        return;
    }
    if (![indirectCommandBuffer isKindOfClass:[RDNA4IndirectCommandBuffer class]]) {
        [_cb rdna4Fail:@"executeCommandsInBuffer requires an RDNA4IndirectCommandBuffer"];
        return;
    }
    RDNA4IndirectCommandBuffer *icb = (RDNA4IndirectCommandBuffer *)indirectCommandBuffer;
    NSString *problem = nil;
    RDNA4RenderPipelineState *parentPipeline = _pso;
    MTLRenderPipelineReflection *parentReflection = _roReflection;
    RDNA4StageState parentVertex = _sv;
    RDNA4StageState parentFragment = _sf;
    [_cb rdna4Retain:icb];
    NSUInteger replayed = [icb rdna4ExecuteOnEncoder:self range:executionRange reason:&problem];
    _pso = parentPipeline;
    _roReflection = parentReflection;
    _sv = parentVertex;
    _sf = parentFragment;
    if (problem) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"executeCommandsInBuffer [%lu,+%lu): %@",
                        (unsigned long)executionRange.location, (unsigned long)executionRange.length, problem]];
        return;
    }
    RDNA4LOGR("executeCommandsInBuffer: replayed %lu command(s)", (unsigned long)replayed);
}
- (void)executeCommandsInBuffer:(id<MTLIndirectCommandBuffer>)indirectCommandBuffer
                 indirectBuffer:(id<MTLBuffer>)indirectRangeBuffer
             indirectBufferOffset:(NSUInteger)indirectBufferOffset {
    (void)indirectCommandBuffer; (void)indirectRangeBuffer; (void)indirectBufferOffset;
    if (_hostWriteDropped) return;
    [_cb rdna4Fail:@"GPU-read indirect execution ranges are part of the GPU ICB path and are not implemented"];
}
@end

@interface RDNA4BlitEncoder (RDNA4IndirectCommands)
@end

@implementation RDNA4BlitEncoder (RDNA4IndirectCommands)
- (void)resetCommandsInBuffer:(id<MTLIndirectCommandBuffer>)buffer withRange:(NSRange)range {
    if (_ended) { [_cb rdna4Fail:@"resetCommandsInBuffer after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    if (![buffer isKindOfClass:[RDNA4IndirectCommandBuffer class]]) {
        [_cb rdna4Fail:@"resetCommandsInBuffer requires an RDNA4IndirectCommandBuffer"];
        return;
    }
    NSString *problem = nil;
    if (![(RDNA4IndirectCommandBuffer *)buffer rdna4RecordResetRange:range
                                                       commandBuffer:_cb
                                                              reason:&problem])
        [_cb rdna4Fail:[NSString stringWithFormat:@"resetCommandsInBuffer [%lu,+%lu): %@",
                        (unsigned long)range.location, (unsigned long)range.length, problem]];
}
- (void)copyIndirectCommandBuffer:(id<MTLIndirectCommandBuffer>)source
                     sourceRange:(NSRange)sourceRange
                     destination:(id<MTLIndirectCommandBuffer>)destination
                destinationIndex:(NSUInteger)destinationIndex {
    if (_ended) { [_cb rdna4Fail:@"copyIndirectCommandBuffer after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    if (![source isKindOfClass:[RDNA4IndirectCommandBuffer class]] ||
        ![destination isKindOfClass:[RDNA4IndirectCommandBuffer class]]) {
        [_cb rdna4Fail:@"copyIndirectCommandBuffer requires RDNA4IndirectCommandBuffers"];
        return;
    }
    NSString *problem = nil;
    if (![(RDNA4IndirectCommandBuffer *)source rdna4RecordCopyRange:sourceRange
                                                    ofBuffer:(RDNA4IndirectCommandBuffer *)source
                                               toDestination:(RDNA4IndirectCommandBuffer *)destination
                                            destinationIndex:destinationIndex
                                                 commandBuffer:_cb
                                                        reason:&problem])
        [_cb rdna4Fail:[NSString stringWithFormat:@"copyIndirectCommandBuffer [%lu,+%lu) -> %lu: %@",
                        (unsigned long)sourceRange.location, (unsigned long)sourceRange.length,
                        (unsigned long)destinationIndex, problem]];
}
- (void)optimizeIndirectCommandBuffer:(id<MTLIndirectCommandBuffer>)buffer withRange:(NSRange)range {
    if (_ended) { [_cb rdna4Fail:@"optimizeIndirectCommandBuffer after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    if (![buffer isKindOfClass:[RDNA4IndirectCommandBuffer class]]) {
        [_cb rdna4Fail:@"optimizeIndirectCommandBuffer requires an RDNA4IndirectCommandBuffer"];
        return;
    }
    [_cb rdna4Retain:buffer];
    NSString *problem = nil;
    if (![(RDNA4IndirectCommandBuffer *)buffer rdna4ValidateRange:range reason:&problem])
        [_cb rdna4Fail:[NSString stringWithFormat:@"optimizeIndirectCommandBuffer [%lu,+%lu): %@",
                        (unsigned long)range.location, (unsigned long)range.length, problem]];
    // The CPU representation has no driver command stream to reorder.
}
@end

@interface GarnetDevice (RDNA4IndirectCommandBuffers)
- (id<MTLIndirectCommandBuffer>)newIndirectCommandBufferWithDescriptor:(MTLIndirectCommandBufferDescriptor *)descriptor
                                                       maxCommandCount:(NSUInteger)maxCommandCount
                                                                options:(MTLResourceOptions)options;
@end

@implementation GarnetDevice (RDNA4IndirectCommandBuffers)
- (id<MTLIndirectCommandBuffer>)newIndirectCommandBufferWithDescriptor:(MTLIndirectCommandBufferDescriptor *)descriptor
                                                       maxCommandCount:(NSUInteger)maxCommandCount
                                                                options:(MTLResourceOptions)options {
    NSError *error = nil;
    RDNA4IndirectCommandBuffer *buffer = [[RDNA4IndirectCommandBuffer alloc] initWithDevice:self
                                                                                 descriptor:descriptor
                                                                           maxCommandCount:maxCommandCount
                                                                                    options:options
                                                                                      error:&error];
    if (!buffer) {
        RDNA4LOG("newIndirectCommandBufferWithDescriptor: refused: %s", error.localizedDescription.UTF8String);
        return nil;
    }
    RDNA4LOG("newIndirectCommandBufferWithDescriptor: %lu command(s), %lu bytes, commandTypes 0x%lx inheritPipeline %d inheritBuffers %d bind counts %lu/%lu",
             (unsigned long)maxCommandCount, (unsigned long)buffer.size,
             (unsigned long)descriptor.commandTypes, descriptor.inheritPipelineState,
             descriptor.inheritBuffers, (unsigned long)descriptor.maxVertexBufferBindCount,
             (unsigned long)descriptor.maxFragmentBufferBindCount);
    return (id<MTLIndirectCommandBuffer>)buffer;
}
@end

#endif
