// garnet_icb.m - GarnetMetal modular compilation unit: indirect command buffers and GPU expansion.
#if !defined(GARNET_INTERNAL) && !defined(GARNET_METAL_MODULAR_BUILD)
#define GARNET_INTERNAL static
#endif
#ifdef GARNET_METAL_MODULAR_BUILD
#include "garnet_metal_internal.h"
#endif
#ifdef GARNET_METAL_INTERNAL_H
#include "garnet_luma_workgroup.h"
#include "garnet_icb_expand.h"
#endif
#include "garnet_icb_internal.h"

#ifndef RDNA4_ICB_INC
#define RDNA4_ICB_INC

@implementation RDNA4ICBBind
- (instancetype)initWithBuffer:(RDNA4Buffer *)buffer offset:(NSUInteger)offset {
    self = [super init];
    if (!self) return nil;
    _buffer = buffer;
    _offset = offset;
    return self;
}
@end

@implementation RDNA4ICBRecord
- (instancetype)init {
    self = [super init];
    if (!self) return nil;
    _vertexBinds = [NSMutableDictionary dictionary];
    _fragmentBinds = [NSMutableDictionary dictionary];
    _kernelBinds = [NSMutableDictionary dictionary];
    _threadgroupMemLengths = [NSMutableDictionary dictionary];
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
    _computePipeline = nil;
    [_kernelBinds removeAllObjects];
    [_threadgroupMemLengths removeAllObjects];
    RDNA4_ICB_RESET_VALUE(_hasBarrier, NO);
    RDNA4_ICB_RESET_VALUE(_hasStageInRegion, NO);
    _stageInRegion = MTLRegionMake3D(0, 0, 0, 0, 0, 0);
    _gridSize = MTLSizeMake(0, 0, 0);
    _threadgroupSize = MTLSizeMake(0, 0, 0);
#undef RDNA4_ICB_RESET_VALUE
}
- (void)reject:(NSString *)reason {
    if (!_error) _error = [(reason ?: @"invalid indirect command") copy];
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
    copy->_computePipeline = _computePipeline;
    copy->_kernelBinds = [_kernelBinds mutableCopy];
    copy->_threadgroupMemLengths = [_threadgroupMemLengths mutableCopy];
    copy->_hasBarrier = _hasBarrier;
    copy->_hasStageInRegion = _hasStageInRegion;
    copy->_stageInRegion = _stageInRegion;
    copy->_gridSize = _gridSize;
    copy->_threadgroupSize = _threadgroupSize;
    return copy;
}
@end


GARNET_INTERNAL NSString *rdna4icb_range_problem(NSRange range, NSUInteger count, BOOL execution) {
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

GARNET_INTERNAL BOOL rdna4icb_stage_buffers_ok(const RDNA4PB *bindings, uint32_t count,
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

GARNET_INTERNAL BOOL rdna4icb_read_uint(id object, SEL selector, NSUInteger *value) {
    if (!object || ![object respondsToSelector:selector]) return NO;
    typedef NSUInteger (*RDNA4ICBUIntMsg)(id, SEL);
    RDNA4ICBUIntMsg fn = (RDNA4ICBUIntMsg)(void *)objc_msgSend;
    *value = fn(object, selector);
    return YES;
}

static NSString *rdna4icb_descriptor_problem(MTLIndirectCommandBufferDescriptor *descriptor) {
    if (!descriptor) return @"nil indirect command buffer descriptor";
    const MTLIndirectCommandType supported = RDNA4ICB_RENDER_TYPES | RDNA4ICB_COMPUTE_TYPES;
    if (!descriptor.commandTypes) return @"descriptor.v1 commandTypes is empty";
    if (descriptor.commandTypes & ~supported)
        return [NSString stringWithFormat:@"commandTypes 0x%lx include mesh or unknown commands outside supported render/compute ICB types",
                (unsigned long)descriptor.commandTypes];
    BOOL hasRender = (descriptor.commandTypes & RDNA4ICB_RENDER_TYPES) != 0;
    BOOL hasCompute = (descriptor.commandTypes & RDNA4ICB_COMPUTE_TYPES) != 0;
    if (hasRender && hasCompute)
        return @"commandTypes cannot mix render and compute commands on the same indirect command buffer";
    if (hasRender) {
        if (descriptor.maxVertexBufferBindCount > RDNA4ICB_MAX_BUFFER_BINDINGS ||
            descriptor.maxFragmentBufferBindCount > RDNA4ICB_MAX_BUFFER_BINDINGS)
            return [NSString stringWithFormat:@"buffer bind counts %lu/%lu exceed the limit of %lu",
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
    } else {
        NSUInteger maxKernelBinds = 0;
        if (rdna4icb_read_uint(descriptor, @selector(maxKernelBufferBindCount), &maxKernelBinds) &&
            maxKernelBinds > RDNA4ICB_MAX_BUFFER_BINDINGS)
            return [NSString stringWithFormat:@"maxKernelBufferBindCount %lu exceeds the limit of %lu",
                    (unsigned long)maxKernelBinds, (unsigned long)RDNA4ICB_MAX_BUFFER_BINDINGS];
    }
    BOOL value = NO;
    const SEL forbidden[] = {
        @selector(supportRayTracing), @selector(supportDynamicAttributeStride),
        @selector(supportColorAttachmentMapping),
    };
    for (size_t i = 0; i < sizeof forbidden / sizeof forbidden[0]; ++i)
        if (rdna4icb_read_bool(descriptor, forbidden[i], &value) && value)
            return @"ray tracing, dynamic attribute stride or color-attachment mapping is outside supported ICB capabilities";
    return nil;
}

static BOOL rdna4icb_descriptor_equal(MTLIndirectCommandBufferDescriptor *a,
                                     MTLIndirectCommandBufferDescriptor *b) {
    if (!a || !b || a.commandTypes != b.commandTypes ||
        a.inheritPipelineState != b.inheritPipelineState ||
        a.inheritBuffers != b.inheritBuffers) return NO;
    BOOL isCompute = (a.commandTypes & RDNA4ICB_COMPUTE_TYPES) != 0;
    if (!a.inheritBuffers) {
        if (isCompute) {
            NSUInteger ka = 0, kb = 0;
            (void)rdna4icb_read_uint(a, @selector(maxKernelBufferBindCount), &ka);
            (void)rdna4icb_read_uint(b, @selector(maxKernelBufferBindCount), &kb);
            if (ka != kb) return NO;
        } else if (a.maxVertexBufferBindCount != b.maxVertexBufferBindCount ||
                   a.maxFragmentBufferBindCount != b.maxFragmentBufferBindCount) {
            return NO;
        }
    }
    if (!isCompute) {
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

@implementation RDNA4IndirectComputeCommand
#ifdef VK_VERSION_1_0
+ (void)load {
    { const char *n[] = { "MTLIndirectCommandBuffer", "MTLResource" }; rdna4_add_protocols([RDNA4IndirectCommandBuffer class], n, 2, "RDNA4IndirectCommandBuffer"); }
    { const char *n[] = { "MTLIndirectRenderCommand" }; rdna4_add_protocols([RDNA4IndirectRenderCommand class], n, 1, "RDNA4IndirectRenderCommand"); }
    { const char *n[] = { "MTLIndirectComputeCommand" }; rdna4_add_protocols([RDNA4IndirectComputeCommand class], n, 1, "RDNA4IndirectComputeCommand"); }
}
#endif
- (instancetype)initWithOwner:(RDNA4IndirectCommandBuffer *)owner index:(NSUInteger)index valid:(BOOL)valid {
    self = [super init];
    if (!self) return nil;
    _owner = owner;
    _index = index;
    _validIndex = valid;
    return self;
}
- (void)rdna4Reject:(NSString *)reason {
    RDNA4_ONCE("indirectComputeCommandAtIndex: invalid index or command mutation rejected: %s", reason.UTF8String);
    if (_validIndex && _owner) [_owner rdna4RejectComputeAtIndex:_index reason:reason];
}
- (BOOL)rdna4CanMutate {
    if (_validIndex && _owner) return YES;
    RDNA4_ONCE("indirectComputeCommandAtIndex: command index %lu is outside its indirect command buffer", (unsigned long)_index);
    return NO;
}
- (void)setComputePipelineState:(id<MTLComputePipelineState>)pipelineState {
    if ([self rdna4CanMutate]) [_owner rdna4SetComputePipelineState:pipelineState atIndex:_index];
}
- (void)setKernelBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index {
    if ([self rdna4CanMutate])
        [_owner rdna4SetKernelBuffer:buffer offset:offset bufferIndex:index commandIndex:_index];
}
- (void)setKernelBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset
        attributeStride:(NSUInteger)stride atIndex:(NSUInteger)index {
    if (![self rdna4CanMutate]) return;
    if (stride == 0 || stride == (NSUInteger)-1) {
        [_owner rdna4SetKernelBuffer:buffer offset:offset bufferIndex:index commandIndex:_index];
        return;
    }
    [self rdna4Reject:@"dynamic attribute stride is not enabled on this compute ICB"];
}
- (void)concurrentDispatchThreadgroups:(MTLSize)threadgroupsPerGrid
                 threadsPerThreadgroup:(MTLSize)threadsPerThreadgroup {
    if ([self rdna4CanMutate])
        [_owner rdna4SetComputeDispatchAtIndex:_index
                                      gridSize:threadgroupsPerGrid
                               threadgroupSize:threadsPerThreadgroup
                                  exactThreads:NO];
}
- (void)concurrentDispatchThreads:(MTLSize)threadsPerGrid
            threadsPerThreadgroup:(MTLSize)threadsPerThreadgroup {
    if ([self rdna4CanMutate])
        [_owner rdna4SetComputeDispatchAtIndex:_index
                                      gridSize:threadsPerGrid
                               threadgroupSize:threadsPerThreadgroup
                                  exactThreads:YES];
}
- (void)setBarrier {
    if ([self rdna4CanMutate]) [_owner rdna4SetBarrier:YES atIndex:_index];
}
- (void)clearBarrier {
    if ([self rdna4CanMutate]) [_owner rdna4SetBarrier:NO atIndex:_index];
}
- (void)setImageblockWidth:(NSUInteger)width height:(NSUInteger)height {
    (void)width; (void)height;
    [self rdna4Reject:@"imageblock dimensions are Apple Silicon TBDR-only and unsupported on RDNA4"];
}
- (void)setThreadgroupMemoryLength:(NSUInteger)length atIndex:(NSUInteger)index {
    if ([self rdna4CanMutate])
        [_owner rdna4SetThreadgroupMemoryLength:length atIndex:index commandIndex:_index];
}
- (void)setStageInRegion:(MTLRegion)region {
    if ([self rdna4CanMutate]) [_owner rdna4SetStageInRegion:region atIndex:_index];
}
- (void)reset {
    if ([self rdna4CanMutate]) [_owner rdna4ResetOneAtIndex:_index];
}
- (MTLIndirectCommandType)getCommandType {
    return (_validIndex && _owner) ? [_owner rdna4CommandTypeAtIndex:_index] : (MTLIndirectCommandType)0;
}
- (BOOL)hasBarrier {
    return (_validIndex && _owner) ? [_owner rdna4HasBarrierAtIndex:_index] : NO;
}
- (NSUInteger)getThreadgroupMemoryLengthAtIndex:(NSUInteger)index {
    return (_validIndex && _owner) ? [_owner rdna4ThreadgroupMemoryLengthAtIndex:index commandIndex:_index] : 0;
}
- (MTLRegion)getStageInRegion {
    return (_validIndex && _owner) ? [_owner rdna4StageInRegionAtIndex:_index] : MTLRegionMake3D(0, 0, 0, 0, 0, 0);
}
- (BOOL)getOptimizedStatus { return NO; }
- (id)getPipelineState {
    return (_validIndex && _owner) ? [_owner rdna4ComputePipelineAtIndex:_index] : nil;
}
- (uint64_t)getPipelineStateUniqueIdentifier {
    id pso = [self getPipelineState];
    if (pso && [pso respondsToSelector:@selector(uniqueIdentifier)]) {
        return ((uint64_t (*)(id, SEL))objc_msgSend)(pso, @selector(uniqueIdentifier));
    }
    return 0;
}
- (void *)getKernelBufferAtIndex:(NSUInteger)index {
    id buf = (_validIndex && _owner) ? [_owner rdna4KernelBufferAtIndex:index commandIndex:_index] : nil;
    return (__bridge void *)buf;
}
- (MTLDispatchThreadgroupsIndirectArguments)getDispatchThreadgroupsArguments {
    MTLDispatchThreadgroupsIndirectArguments out = {{ 0, 0, 0 }};
    if (_validIndex && _owner) {
        MTLSize g = [_owner rdna4GridSizeAtIndex:_index];
        out.threadgroupsPerGrid[0] = (uint32_t)g.width;
        out.threadgroupsPerGrid[1] = (uint32_t)g.height;
        out.threadgroupsPerGrid[2] = (uint32_t)g.depth;
    }
    return out;
}
- (MTLStageInRegionIndirectArguments)getStageInRegionArguments {
    MTLStageInRegionIndirectArguments out = {{ 0, 0, 0 }, { 0, 0, 0 }};
    if (_validIndex && _owner) {
        MTLRegion r = [_owner rdna4StageInRegionAtIndex:_index];
        out.stageInOrigin[0] = (uint32_t)r.origin.x;
        out.stageInOrigin[1] = (uint32_t)r.origin.y;
        out.stageInOrigin[2] = (uint32_t)r.origin.z;
        out.stageInSize[0] = (uint32_t)r.size.width;
        out.stageInSize[1] = (uint32_t)r.size.height;
        out.stageInSize[2] = (uint32_t)r.size.depth;
    }
    return out;
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
        _computeCommands = [NSMutableDictionary dictionary];
        _emptyRecord = [RDNA4ICBRecord new];
        _invalidCommand = [[RDNA4IndirectRenderCommand alloc] initWithOwner:nil
                                                                    index:NSUIntegerMax
                                                                    valid:NO];
        _invalidComputeCommand = [[RDNA4IndirectComputeCommand alloc] initWithOwner:nil
                                                                              index:NSUIntegerMax
                                                                              valid:NO];
        if (!_descriptor || !_records || !_commands || !_computeCommands || !_emptyRecord ||
            !_invalidCommand || !_invalidComputeCommand)
            [NSException raise:NSMallocException format:@"indirect command buffer bookkeeping allocation failed"];
    } @catch (NSException *exception) {
        if (error) *error = rdna4_err(120, exception.reason ?: @"indirect command buffer allocation failed");
        return nil;
    }
    _commandTypes = descriptor.commandTypes;
    _isCompute = ((_commandTypes & RDNA4ICB_COMPUTE_TYPES) != 0);
    _inheritPipelineState = descriptor.inheritPipelineState;
    _inheritBuffers = descriptor.inheritBuffers;
    _maxVertexBufferBindCount = (descriptor.inheritBuffers || _isCompute) ? 0 : descriptor.maxVertexBufferBindCount;
    _maxFragmentBufferBindCount = (descriptor.inheritBuffers || _isCompute) ? 0 : descriptor.maxFragmentBufferBindCount;
    NSUInteger kBinds = 0;
    if (!descriptor.inheritBuffers && _isCompute)
        (void)rdna4icb_read_uint(descriptor, @selector(maxKernelBufferBindCount), &kBinds);
    _maxKernelBufferBindCount = kBinds;
#ifdef VK_VERSION_1_0
    _cpuDirtySlots = [NSMutableSet set];
    _cpuResetRanges = [NSMutableArray array];
    _candidatePipelines = [NSMutableArray array];
    _gpuRetained = [NSMutableSet set];
#endif
    return self;
}
- (BOOL)rdna4IsComputeICB { return _isCompute; }
#ifdef VK_VERSION_1_0
- (BOOL)rdna4UseGPUExecution {
    pthread_mutex_lock(&_mu);
    BOOL useGPU = (_gpuProduced || _gpuBacking != nil);
    pthread_mutex_unlock(&_mu);
    return useGPU;
}
#endif
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
    if (_isCompute) {
        RDNA4_ONCE("indirectRenderCommandAtIndex: cannot fetch a render command from a compute ICB");
        return nil;
    }
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
    if (!_isCompute) {
        RDNA4_ONCE("indirectComputeCommandAtIndex: cannot fetch a compute command from a render ICB");
        return nil;
    }
    if (index >= _commandCount) {
        RDNA4LOG("indirectComputeCommandAtIndex:%lu is outside %lu command slot(s)",
                 (unsigned long)index, (unsigned long)_commandCount);
        return (id<MTLIndirectComputeCommand>)_invalidComputeCommand;
    }
    NSNumber *key = @(index);
    pthread_mutex_lock(&_mu);
    RDNA4IndirectComputeCommand *command = _computeCommands[key];
    if (!command && !_fatalError) {
        @try {
            command = [[RDNA4IndirectComputeCommand alloc] initWithOwner:self index:index valid:YES];
            if (command) _computeCommands[key] = command;
        } @catch (NSException *exception) {
            _fatalError = [NSString stringWithFormat:@"indirect compute command slot %lu allocation failed: %@",
                           (unsigned long)index, exception.reason ?: @"out of memory"];
        }
    }
    pthread_mutex_unlock(&_mu);
    return (id<MTLIndirectComputeCommand>)(command ?: _invalidComputeCommand);
}
- (void)rdna4SetComputePipelineState:(id)pipeline atIndex:(NSUInteger)index {
    pthread_mutex_lock(&_mu);
    RDNA4ICBRecord *record = [self rdna4LockedRecordAtIndex:index];
    NSString *problem = nil;
    if (!record) problem = @"slot is outside the indirect command buffer";
    else if (!_isCompute) problem = @"compute pipeline cannot be set on a render ICB";
    else if (record.error) problem = record.error;
    else if (record.kind != RDNA4ICBCommandEmpty) problem = @"compute pipeline cannot be changed after a dispatch in the same ICB slot";
    else if (_inheritPipelineState) problem = @"this compute ICB inherits its pipeline from the parent encoder";
#ifdef VK_VERSION_1_0
    else if (![pipeline isKindOfClass:[RDNA4ComputePipelineState class]]) problem = @"pipeline is not an RDNA4ComputePipelineState";
#else
    else if (!pipeline || ![pipeline respondsToSelector:@selector(device)]) problem = @"pipeline is not a compute pipeline state";
#endif
    else if ([(id<MTLComputePipelineState>)pipeline device] != _dev) problem = @"pipeline belongs to another device";
    else if (!rdna4icb_pipeline_supports_icb(pipeline)) problem = @"compute pipeline does not support indirect command buffers";
    if (problem) {
        [record reject:problem];
#ifdef VK_VERSION_1_0
        if (_gpuBacking && index < _commandCount) [_cpuDirtySlots addObject:@(index)];
#endif
    } else {
        record.computePipeline = pipeline;
#ifdef VK_VERSION_1_0
        if (_gpuBacking) [_cpuDirtySlots addObject:@(index)];
#endif
    }
    pthread_mutex_unlock(&_mu);
}
- (void)rdna4SetKernelBuffer:(id)buffer
                      offset:(NSUInteger)offset
                 bufferIndex:(NSUInteger)bufferIndex
                commandIndex:(NSUInteger)commandIndex {
    pthread_mutex_lock(&_mu);
    RDNA4ICBRecord *record = [self rdna4LockedRecordAtIndex:commandIndex];
    NSString *problem = nil;
    if (!record) problem = @"slot is outside the indirect command buffer";
    else if (!_isCompute) problem = @"kernel buffer cannot be set on a render ICB";
    else if (record.error) problem = record.error;
    else if (record.kind != RDNA4ICBCommandEmpty) problem = @"kernel buffer bindings cannot be changed after a dispatch in the same ICB slot";
    else if (_inheritBuffers) problem = @"this compute ICB inherits buffer bindings from the parent encoder";
    else if (bufferIndex >= _maxKernelBufferBindCount || bufferIndex >= RDNA4ICB_MAX_BUFFER_BINDINGS)
        problem = [NSString stringWithFormat:@"kernel buffer index %lu exceeds max bind count %lu",
                   (unsigned long)bufferIndex, (unsigned long)_maxKernelBufferBindCount];
    else if (!buffer) {
        if (offset != 0) problem = @"nil kernel buffer with non-zero offset";
    } else if (![buffer isKindOfClass:[RDNA4Buffer class]]) problem = @"buffer is not an RDNA4Buffer";
    else if ([(RDNA4Buffer *)buffer device] != _dev) problem = @"buffer belongs to another device";
    else if (offset > [(RDNA4Buffer *)buffer length]) problem = @"buffer offset is outside the logical resource";
    if (problem) {
        [record reject:problem];
#ifdef VK_VERSION_1_0
        if (_gpuBacking && commandIndex < _commandCount) [_cpuDirtySlots addObject:@(commandIndex)];
#endif
    } else {
        @try {
            if (buffer) record.kernelBinds[@(bufferIndex)] = [[RDNA4ICBBind alloc] initWithBuffer:buffer offset:offset];
            else [record.kernelBinds removeObjectForKey:@(bufferIndex)];
#ifdef VK_VERSION_1_0
            if (_gpuBacking) [_cpuDirtySlots addObject:@(commandIndex)];
#endif
        } @catch (NSException *exception) {
            [record reject:[NSString stringWithFormat:@"kernel buffer binding allocation failed: %@",
                            exception.reason ?: @"out of memory"]];
        }
    }
    pthread_mutex_unlock(&_mu);
}
- (void)rdna4SetComputeDispatchAtIndex:(NSUInteger)index
                              gridSize:(MTLSize)gridSize
                       threadgroupSize:(MTLSize)tgSize
                          exactThreads:(BOOL)exactThreads {
    pthread_mutex_lock(&_mu);
    RDNA4ICBRecord *record = [self rdna4LockedRecordAtIndex:index];
    MTLIndirectCommandType cmdType = exactThreads ? RDNA4ICB_TYPE_CONCURRENT_DISPATCH_THREADS : RDNA4ICB_TYPE_CONCURRENT_DISPATCH;
    NSString *problem = nil;
    if (!record) problem = @"slot is outside the indirect command buffer";
    else if (!_isCompute) problem = @"compute dispatch cannot be encoded on a render ICB";
    else if (record.error) problem = record.error;
    else if (record.kind != RDNA4ICBCommandEmpty) problem = @"an ICB slot accepts only one compute dispatch command";
    else if (!(_commandTypes & cmdType))
        problem = [NSString stringWithFormat:@"this compute ICB descriptor does not permit command type 0x%lx", (unsigned long)cmdType];
    else if (!_inheritPipelineState && !record.computePipeline)
        problem = @"compute dispatch has no pipeline and this ICB does not inherit one";
    else if (!tgSize.width || !tgSize.height || !tgSize.depth)
        problem = @"threadsPerThreadgroup has a zero dimension";
    else if (gridSize.width > UINT32_MAX || gridSize.height > UINT32_MAX || gridSize.depth > UINT32_MAX ||
             tgSize.width > UINT32_MAX || tgSize.height > UINT32_MAX || tgSize.depth > UINT32_MAX)
        problem = @"compute dispatch dimensions exceed 32-bit limits";
    if (problem && record) {
        [record reject:problem];
#ifdef VK_VERSION_1_0
        if (_gpuBacking && index < _commandCount) [_cpuDirtySlots addObject:@(index)];
#endif
    } else if (!problem) {
        record.kind = exactThreads ? RDNA4ICBCommandConcurrentDispatchThreads : RDNA4ICBCommandConcurrentDispatch;
        record.gridSize = gridSize;
        record.threadgroupSize = tgSize;
#ifdef VK_VERSION_1_0
        if (_gpuBacking) [_cpuDirtySlots addObject:@(index)];
#endif
    }
    pthread_mutex_unlock(&_mu);
}
- (void)rdna4SetBarrier:(BOOL)enable atIndex:(NSUInteger)index {
    pthread_mutex_lock(&_mu);
    RDNA4ICBRecord *record = [self rdna4LockedRecordAtIndex:index];
    if (record && _isCompute && !record.error) {
        record.hasBarrier = enable;
#ifdef VK_VERSION_1_0
        if (_gpuBacking) [_cpuDirtySlots addObject:@(index)];
#endif
    } else if (record && !_isCompute) {
        [record reject:@"barriers are only supported on compute ICBs"];
    }
    pthread_mutex_unlock(&_mu);
}
- (void)rdna4SetThreadgroupMemoryLength:(NSUInteger)length
                                atIndex:(NSUInteger)tgIndex
                           commandIndex:(NSUInteger)commandIndex {
    pthread_mutex_lock(&_mu);
    RDNA4ICBRecord *record = [self rdna4LockedRecordAtIndex:commandIndex];
    NSString *problem = nil;
    if (!record) problem = @"slot is outside the indirect command buffer";
    else if (!_isCompute) problem = @"threadgroup memory length cannot be set on a render ICB";
    else if (record.error) problem = record.error;
    else if (tgIndex >= RDNA4ICB_MAX_TG_MEM_INDICES)
        problem = [NSString stringWithFormat:@"threadgroup memory index %lu exceeds limit %lu",
                   (unsigned long)tgIndex, (unsigned long)RDNA4ICB_MAX_TG_MEM_INDICES];
    else if ((length & 15u) != 0 || length > UINT32_MAX)
        problem = @"threadgroup memory length must be 16-byte aligned and fit 32 bits";
#ifdef VK_VERSION_1_0
    else if (RDNA4R.ok && RDNA4R.lim.maxComputeSharedMemorySize && length > RDNA4R.lim.maxComputeSharedMemorySize)
        problem = @"threadgroup memory length exceeds device shared memory limit";
#endif
    if (problem && record) {
        [record reject:problem];
#ifdef VK_VERSION_1_0
        if (_gpuBacking && commandIndex < _commandCount) [_cpuDirtySlots addObject:@(commandIndex)];
#endif
    } else if (!problem) {
        record.threadgroupMemLengths[@(tgIndex)] = @((uint32_t)length);
#ifdef VK_VERSION_1_0
        if (_gpuBacking) [_cpuDirtySlots addObject:@(commandIndex)];
#endif
    }
    pthread_mutex_unlock(&_mu);
}
- (void)rdna4SetStageInRegion:(MTLRegion)region atIndex:(NSUInteger)index {
    pthread_mutex_lock(&_mu);
    RDNA4ICBRecord *record = [self rdna4LockedRecordAtIndex:index];
    NSString *problem = nil;
    if (!record) problem = @"slot is outside the indirect command buffer";
    else if (!_isCompute) problem = @"stage-in region cannot be set on a render ICB";
    else if (record.error) problem = record.error;
    else if (region.origin.x > UINT32_MAX || region.origin.y > UINT32_MAX || region.origin.z > UINT32_MAX ||
             region.size.width > UINT32_MAX || region.size.height > UINT32_MAX || region.size.depth > UINT32_MAX ||
             region.origin.x > UINT32_MAX - region.size.width ||
             region.origin.y > UINT32_MAX - region.size.height ||
             region.origin.z > UINT32_MAX - region.size.depth)
        problem = @"stage-in region exceeds 32-bit bounds";
    if (problem && record) {
        [record reject:problem];
#ifdef VK_VERSION_1_0
        if (_gpuBacking && index < _commandCount) [_cpuDirtySlots addObject:@(index)];
#endif
    } else if (!problem) {
        record.hasStageInRegion = YES;
        record.stageInRegion = region;
#ifdef VK_VERSION_1_0
        if (_gpuBacking) [_cpuDirtySlots addObject:@(index)];
#endif
    }
    pthread_mutex_unlock(&_mu);
}
- (void)rdna4RejectComputeAtIndex:(NSUInteger)index reason:(NSString *)reason {
    pthread_mutex_lock(&_mu);
    RDNA4ICBRecord *record = [self rdna4LockedRecordAtIndex:index];
    if (record) {
        [record reject:reason];
#ifdef VK_VERSION_1_0
        if (_gpuBacking && index < _commandCount) [_cpuDirtySlots addObject:@(index)];
#endif
    }
    pthread_mutex_unlock(&_mu);
}
- (MTLIndirectCommandType)rdna4CommandTypeAtIndex:(NSUInteger)index {
    pthread_mutex_lock(&_mu);
    RDNA4ICBRecord *record = index < _commandCount ? _records[@(index)] : nil;
    MTLIndirectCommandType t = (MTLIndirectCommandType)0;
    if (record && !record.error) {
        if (record.kind == RDNA4ICBCommandConcurrentDispatch) t = RDNA4ICB_TYPE_CONCURRENT_DISPATCH;
        else if (record.kind == RDNA4ICBCommandConcurrentDispatchThreads) t = RDNA4ICB_TYPE_CONCURRENT_DISPATCH_THREADS;
    }
    pthread_mutex_unlock(&_mu);
    return t;
}
- (BOOL)rdna4HasBarrierAtIndex:(NSUInteger)index {
    pthread_mutex_lock(&_mu);
    RDNA4ICBRecord *record = index < _commandCount ? _records[@(index)] : nil;
    BOOL b = (record && !record.error) ? record.hasBarrier : NO;
    pthread_mutex_unlock(&_mu);
    return b;
}
- (NSUInteger)rdna4ThreadgroupMemoryLengthAtIndex:(NSUInteger)tgIndex commandIndex:(NSUInteger)index {
    pthread_mutex_lock(&_mu);
    RDNA4ICBRecord *record = index < _commandCount ? _records[@(index)] : nil;
    NSUInteger len = (record && !record.error) ? [record.threadgroupMemLengths[@(tgIndex)] unsignedIntegerValue] : 0;
    pthread_mutex_unlock(&_mu);
    return len;
}
- (MTLRegion)rdna4StageInRegionAtIndex:(NSUInteger)index {
    pthread_mutex_lock(&_mu);
    RDNA4ICBRecord *record = index < _commandCount ? _records[@(index)] : nil;
    MTLRegion r = (record && !record.error && record.hasStageInRegion)
        ? record.stageInRegion : MTLRegionMake3D(0, 0, 0, 0, 0, 0);
    pthread_mutex_unlock(&_mu);
    return r;
}
- (MTLSize)rdna4GridSizeAtIndex:(NSUInteger)index {
    pthread_mutex_lock(&_mu);
    RDNA4ICBRecord *record = index < _commandCount ? _records[@(index)] : nil;
    MTLSize g = (record && !record.error) ? record.gridSize : MTLSizeMake(0, 0, 0);
    pthread_mutex_unlock(&_mu);
    return g;
}
- (id)rdna4ComputePipelineAtIndex:(NSUInteger)index {
    pthread_mutex_lock(&_mu);
    RDNA4ICBRecord *record = index < _commandCount ? _records[@(index)] : nil;
    id p = (record && !record.error) ? record.computePipeline : nil;
    pthread_mutex_unlock(&_mu);
    return p;
}
- (id)rdna4KernelBufferAtIndex:(NSUInteger)bufferIndex commandIndex:(NSUInteger)index {
    pthread_mutex_lock(&_mu);
    RDNA4ICBRecord *record = index < _commandCount ? _records[@(index)] : nil;
    id b = (record && !record.error) ? record.kernelBinds[@(bufferIndex)].buffer : nil;
    pthread_mutex_unlock(&_mu);
    return b;
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
    else {
        record.pipeline = pipeline;
#ifdef VK_VERSION_1_0
        if (_gpuBacking) [_cpuDirtySlots addObject:@(index)];
#endif
    }
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
#ifdef VK_VERSION_1_0
            if (_gpuBacking) [_cpuDirtySlots addObject:@(commandIndex)];
#endif
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
#ifdef VK_VERSION_1_0
        if (_gpuBacking) [_cpuDirtySlots addObject:@(index)];
#endif
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
#ifdef VK_VERSION_1_0
        if (_gpuBacking) [_cpuDirtySlots addObject:@(index)];
#endif
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
    } else if (patchControlPoints > 32) {
        problem = @"patch control point count exceeds 32";
    } else if (!rdna4icb_u32_range_ok(patchStart, patchCount) ||
               !rdna4icb_u32_range_ok(baseInstance, instanceCount)) {
        problem = @"patch or instance range exceeds Vulkan's 32-bit draw limits";
    } else {
        NSUInteger patchEnd = patchCount ? (patchStart + patchCount) : 0;
        NSUInteger controlPointCount = 0;
        if (indexed && patchEnd && patchControlPoints > NSUIntegerMax / patchEnd) {
            problem = @"control-point index count overflows NSUInteger";
        } else if (indexed) {
            controlPointCount = patchEnd * patchControlPoints;
        }
        if (!problem && !rdna4icb_buffer_range_ok(patchIndexBuffer, _dev, patchIndexBufferOffset,
                                                  4, patchEnd, NO))
            problem = @"patch index buffer, device, range or 4-byte alignment is invalid";
        else if (!problem && !rdna4icb_buffer_range_ok(tessellationFactorBuffer, _dev,
                                                       tessellationFactorBufferOffset, 4, 0, YES))
            problem = @"tessellation factor buffer, device or 4-byte alignment is invalid";
        else if (!problem && ((tessellationFactorBufferInstanceStride & 3) ||
                              tessellationFactorBufferInstanceStride > UINT32_MAX))
            problem = @"tessellation factor instance stride is misaligned or exceeds 32 bits";
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
#ifdef VK_VERSION_1_0
        if (_gpuBacking) [_cpuDirtySlots addObject:@(index)];
#endif
    }
    pthread_mutex_unlock(&_mu);
}
- (void)rdna4ResetOneAtIndex:(NSUInteger)index {
    pthread_mutex_lock(&_mu);
    RDNA4ICBRecord *record = index < _commandCount ? _records[@(index)] : nil;
    if (record) [record reset];
#ifdef VK_VERSION_1_0
    if (_gpuBacking && index < _commandCount) [_cpuDirtySlots addObject:@(index)];
#endif
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

#ifdef VK_VERSION_1_0
static void rdna4icb_retain_record_resources(NSMutableSet *set, RDNA4ICBRecord *rec) {
    if (!set || !rec) return;
    for (RDNA4ICBBind *b in rec.vertexBinds.allValues) if (b.buffer) [set addObject:b.buffer];
    for (RDNA4ICBBind *b in rec.fragmentBinds.allValues) if (b.buffer) [set addObject:b.buffer];
    for (RDNA4ICBBind *b in rec.kernelBinds.allValues) if (b.buffer) [set addObject:b.buffer];
    if (rec.computePipeline) [set addObject:rec.computePipeline];
    if (rec.indexBuffer) [set addObject:rec.indexBuffer];
    if (rec.patchIndexBuffer) [set addObject:rec.patchIndexBuffer];
    if (rec.controlPointIndexBuffer) [set addObject:rec.controlPointIndexBuffer];
    if (rec.tessellationFactorBuffer) [set addObject:rec.tessellationFactorBuffer];
}
static void rdna4icb_fill_slot_from_record(RDNA4ICBRecord *record, BOOL isCompute, rdna4_icb_slot_t *out, id *outPso) {
    memset(out, 0, sizeof *out);
    if (outPso) *outPso = nil;
    if (!record) return;
    if (isCompute) {
        rdna4_icb_compute_slot_t *cs = (rdna4_icb_compute_slot_t *)out;
        if (record.error) cs->flags |= RDNA4_ICB_SLOT_FLAG_INVALID;
        if (record.hasBarrier) cs->flags |= RDNA4_ICB_SLOT_FLAG_BARRIER;
        if (record.hasStageInRegion) {
            cs->flags |= RDNA4_ICB_SLOT_FLAG_STAGE_IN_REGION;
            cs->stage_in_origin[0] = (uint32_t)record.stageInRegion.origin.x;
            cs->stage_in_origin[1] = (uint32_t)record.stageInRegion.origin.y;
            cs->stage_in_origin[2] = (uint32_t)record.stageInRegion.origin.z;
            cs->stage_in_size[0] = (uint32_t)record.stageInRegion.size.width;
            cs->stage_in_size[1] = (uint32_t)record.stageInRegion.size.height;
            cs->stage_in_size[2] = (uint32_t)record.stageInRegion.size.depth;
        }
        if (record.computePipeline) {
            cs->flags |= RDNA4_ICB_SLOT_FLAG_PIPELINE_SET;
            cs->pipeline_token = [(RDNA4ComputePipelineState *)record.computePipeline rdna4ICBToken];
            if (outPso) *outPso = record.computePipeline;
        }
        for (NSNumber *slot in record.kernelBinds) {
            NSUInteger s = slot.unsignedIntegerValue;
            RDNA4ICBBind *bind = record.kernelBinds[slot];
            if (s < 31 && bind.buffer && [bind.buffer gpuAddress]) {
                cs->kernel_bda[s] = [bind.buffer gpuAddress] + bind.offset;
                cs->kernel_bind_mask |= (1u << s);
            }
        }
        for (NSNumber *tgKey in record.threadgroupMemLengths) {
            NSUInteger ti = tgKey.unsignedIntegerValue;
            if (ti < RDNA4_ICB_MAX_TG_MEM_INDICES) {
                cs->threadgroup_mem_length[ti] = record.threadgroupMemLengths[tgKey].unsignedIntValue;
                cs->threadgroup_mem_mask |= (1u << ti);
            }
        }
        if (record.kind == RDNA4ICBCommandConcurrentDispatch ||
            record.kind == RDNA4ICBCommandConcurrentDispatchThreads) {
            cs->kind = (record.kind == RDNA4ICBCommandConcurrentDispatchThreads)
                ? RDNA4_ICB_KIND_CONCURRENT_DISPATCH_THREADS : RDNA4_ICB_KIND_CONCURRENT_DISPATCH;
            cs->grid_size[0] = (uint32_t)record.gridSize.width;
            cs->grid_size[1] = (uint32_t)record.gridSize.height;
            cs->grid_size[2] = (uint32_t)record.gridSize.depth;
            cs->threadgroup_size[0] = (uint32_t)record.threadgroupSize.width;
            cs->threadgroup_size[1] = (uint32_t)record.threadgroupSize.height;
            cs->threadgroup_size[2] = (uint32_t)record.threadgroupSize.depth;
        }
        return;
    }
    if (record.error) out->flags |= RDNA4_ICB_SLOT_FLAG_INVALID;
    if (record.pipeline) {
        out->flags |= RDNA4_ICB_SLOT_FLAG_PIPELINE_SET;
        out->pipeline_token = [record.pipeline rdna4ICBToken];
        if (outPso) *outPso = record.pipeline;
    }
    for (NSNumber *slot in record.vertexBinds) {
        NSUInteger s = slot.unsignedIntegerValue;
        RDNA4ICBBind *bind = record.vertexBinds[slot];
        if (s < 31 && bind.buffer && [bind.buffer gpuAddress]) {
            out->vertex_bda[s] = [bind.buffer gpuAddress] + bind.offset;
            out->vertex_bind_mask |= (1u << s);
        }
    }
    for (NSNumber *slot in record.fragmentBinds) {
        NSUInteger s = slot.unsignedIntegerValue;
        RDNA4ICBBind *bind = record.fragmentBinds[slot];
        if (s < 31 && bind.buffer && [bind.buffer gpuAddress]) {
            out->fragment_bda[s] = [bind.buffer gpuAddress] + bind.offset;
            out->fragment_bind_mask |= (1u << s);
        }
    }
    switch (record.kind) {
        case RDNA4ICBCommandEmpty:
        case RDNA4ICBCommandConcurrentDispatch:
        case RDNA4ICBCommandConcurrentDispatchThreads:
            out->kind = RDNA4_ICB_KIND_EMPTY;
            break;
        case RDNA4ICBCommandDraw:
            out->kind = RDNA4_ICB_KIND_DRAW;
            out->primitive_type = (uint32_t)record.primitiveType;
            out->count_or_patch_count = (uint32_t)record.vertexCount;
            out->instance_count = (uint32_t)record.instanceCount;
            out->first_or_patch_start = (uint32_t)record.vertexStart;
            out->base_instance = (uint32_t)record.baseInstance;
            break;
        case RDNA4ICBCommandDrawIndexed:
            out->kind = RDNA4_ICB_KIND_DRAW_INDEXED;
            out->primitive_type = (uint32_t)record.primitiveType;
            out->index_type = (record.indexType == MTLIndexTypeUInt32) ? RDNA4_ICB_INDEX_UINT32 : RDNA4_ICB_INDEX_UINT16;
            out->count_or_patch_count = (uint32_t)record.indexCount;
            out->instance_count = (uint32_t)record.instanceCount;
            out->first_or_patch_start = 0u;
            out->base_vertex = (int32_t)record.baseVertex;
            out->base_instance = (uint32_t)record.baseInstance;
            if (record.indexBuffer && [record.indexBuffer gpuAddress])
                out->index_or_cp_bda = [record.indexBuffer gpuAddress] + record.indexBufferOffset;
            break;
        case RDNA4ICBCommandDrawPatches:
        case RDNA4ICBCommandDrawIndexedPatches:
            out->kind = (record.kind == RDNA4ICBCommandDrawIndexedPatches)
                ? RDNA4_ICB_KIND_DRAW_INDEXED_PATCHES : RDNA4_ICB_KIND_DRAW_PATCHES;
            out->patch_control_points = (uint32_t)record.patchControlPoints;
            out->count_or_patch_count = (uint32_t)record.patchCount;
            out->instance_count = (uint32_t)record.instanceCount;
            out->first_or_patch_start = (uint32_t)record.patchStart;
            out->base_instance = (uint32_t)record.baseInstance;
            out->tess_instance_stride = (uint32_t)record.tessellationFactorInstanceStride;
            if (record.patchIndexBuffer && [record.patchIndexBuffer gpuAddress])
                out->patch_index_bda = [record.patchIndexBuffer gpuAddress] + record.patchIndexBufferOffset;
            if (record.tessellationFactorBuffer && [record.tessellationFactorBuffer gpuAddress])
                out->tess_factor_bda = [record.tessellationFactorBuffer gpuAddress] + record.tessellationFactorBufferOffset;
            if (record.kind == RDNA4ICBCommandDrawIndexedPatches &&
                record.controlPointIndexBuffer && [record.controlPointIndexBuffer gpuAddress])
                out->index_or_cp_bda = [record.controlPointIndexBuffer gpuAddress] + record.controlPointIndexBufferOffset;
            break;
    }
}

GARNET_INTERNAL BOOL rdna4_icb_get_compute_expander(VkDescriptorSetLayout *outDsl, VkPipelineLayout *outPl, VkPipeline *outPipe, NSError **err) {
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    static VkDevice dev = VK_NULL_HANDLE;
    static VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    static VkPipelineLayout pl = VK_NULL_HANDLE;
    static VkPipeline pipe = VK_NULL_HANDLE;
    if (!rdna4_radv_open(err)) return NO;
    pthread_mutex_lock(&mu);
    if (dev != RDNA4R.dev) {
        if (dev) {
            if (pipe) vkDestroyPipeline(dev, pipe, NULL);
            if (pl) vkDestroyPipelineLayout(dev, pl, NULL);
            if (dsl) vkDestroyDescriptorSetLayout(dev, dsl, NULL);
        }
        dev = RDNA4R.dev; dsl = VK_NULL_HANDLE; pl = VK_NULL_HANDLE; pipe = VK_NULL_HANDLE;
    }
    if (!pipe) {
        VkShaderModule sm = VK_NULL_HANDLE;
        uint32_t *words = NULL; size_t nwords = 0;
        BOOL ok = NO;
        do {
            VkDescriptorSetLayoutBinding bnd[5];
            for (uint32_t i = 0; i < 5; i++)
                bnd[i] = (VkDescriptorSetLayoutBinding){ i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
            VkDescriptorSetLayoutCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 5, .pBindings = bnd };
            VkResult r = vkCreateDescriptorSetLayout(RDNA4R.dev, &dci, NULL, &dsl);
            if (r != VK_SUCCESS) { if (err) *err = rdna4_err(50, [NSString stringWithFormat:@"vkCreateDescriptorSetLayout(icb compute expand) = %d", r]); break; }
            VkPipelineLayoutCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl };
            r = vkCreatePipelineLayout(RDNA4R.dev, &pci, NULL, &pl);
            if (r != VK_SUCCESS) { if (err) *err = rdna4_err(46, [NSString stringWithFormat:@"vkCreatePipelineLayout(icb compute expand) = %d", r]); break; }
            if (!rdna4_icb_emit_compute_expand_spv(&words, &nwords)) { if (err) *err = rdna4_err(42, @"failed to emit GPU compute ICB expander SPIR-V"); break; }
            sm = rdna4_spv_module(words, nwords * 4, "icb-compute-expand", err);
            free(words); words = NULL;
            if (!sm) break;
            VkComputePipelineCreateInfo cpi = {
                .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                           .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main" },
                .layout = pl
            };
            r = vkCreateComputePipelines(RDNA4R.dev, RDNA4R.pc, 1, &cpi, NULL, &pipe);
            if (r != VK_SUCCESS) { if (err) *err = rdna4_err(47, [NSString stringWithFormat:@"vkCreateComputePipelines(icb compute expand) = %d", r]); break; }
            ok = YES;
        } while (0);
        if (words) free(words);
        if (sm) vkDestroyShaderModule(RDNA4R.dev, sm, NULL);
        if (!ok) {
            if (pipe) { vkDestroyPipeline(RDNA4R.dev, pipe, NULL); pipe = VK_NULL_HANDLE; }
            if (pl) { vkDestroyPipelineLayout(RDNA4R.dev, pl, NULL); pl = VK_NULL_HANDLE; }
            if (dsl) { vkDestroyDescriptorSetLayout(RDNA4R.dev, dsl, NULL); dsl = VK_NULL_HANDLE; }
            pthread_mutex_unlock(&mu);
            return NO;
        }
    }
    *outDsl = dsl; *outPl = pl; *outPipe = pipe;
    pthread_mutex_unlock(&mu);
    return YES;
}

GARNET_INTERNAL BOOL rdna4_icb_get_expander(VkDescriptorSetLayout *outDsl, VkPipelineLayout *outPl, VkPipeline *outPipe, NSError **err) {
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    static VkDevice dev = VK_NULL_HANDLE;
    static VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    static VkPipelineLayout pl = VK_NULL_HANDLE;
    static VkPipeline pipe = VK_NULL_HANDLE;
    if (!rdna4_radv_open(err)) return NO;
    pthread_mutex_lock(&mu);
    if (dev != RDNA4R.dev) {
        if (dev) {
            if (pipe) vkDestroyPipeline(dev, pipe, NULL);
            if (pl) vkDestroyPipelineLayout(dev, pl, NULL);
            if (dsl) vkDestroyDescriptorSetLayout(dev, dsl, NULL);
        }
        dev = RDNA4R.dev; dsl = VK_NULL_HANDLE; pl = VK_NULL_HANDLE; pipe = VK_NULL_HANDLE;
    }
    if (!pipe) {
        VkShaderModule sm = VK_NULL_HANDLE;
        uint32_t *words = NULL; size_t nwords = 0;
        BOOL ok = NO;
        do {
            VkDescriptorSetLayoutBinding bnd[8];
            for (uint32_t i = 0; i < 8; i++)
                bnd[i] = (VkDescriptorSetLayoutBinding){ i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
            VkDescriptorSetLayoutCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 8, .pBindings = bnd };
            VkResult r = vkCreateDescriptorSetLayout(RDNA4R.dev, &dci, NULL, &dsl);
            if (r != VK_SUCCESS) { if (err) *err = rdna4_err(50, [NSString stringWithFormat:@"vkCreateDescriptorSetLayout(icb expand) = %d", r]); break; }
            VkPipelineLayoutCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl };
            r = vkCreatePipelineLayout(RDNA4R.dev, &pci, NULL, &pl);
            if (r != VK_SUCCESS) { if (err) *err = rdna4_err(46, [NSString stringWithFormat:@"vkCreatePipelineLayout(icb expand) = %d", r]); break; }
            if (!rdna4_icb_emit_expand_spv(&words, &nwords)) { if (err) *err = rdna4_err(42, @"failed to emit GPU ICB expander SPIR-V"); break; }
            sm = rdna4_spv_module(words, nwords * 4, "icb-expand", err);
            free(words); words = NULL;
            if (!sm) break;
            VkComputePipelineCreateInfo cpi = {
                .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                           .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main" },
                .layout = pl
            };
            r = vkCreateComputePipelines(RDNA4R.dev, RDNA4R.pc, 1, &cpi, NULL, &pipe);
            if (r != VK_SUCCESS) { if (err) *err = rdna4_err(47, [NSString stringWithFormat:@"vkCreateComputePipelines(icb expand) = %d", r]); break; }
            ok = YES;
        } while (0);
        if (words) free(words);
        if (sm) vkDestroyShaderModule(RDNA4R.dev, sm, NULL);
        if (!ok) {
            if (pipe) { vkDestroyPipeline(RDNA4R.dev, pipe, NULL); pipe = VK_NULL_HANDLE; }
            if (pl) { vkDestroyPipelineLayout(RDNA4R.dev, pl, NULL); pl = VK_NULL_HANDLE; }
            if (dsl) { vkDestroyDescriptorSetLayout(RDNA4R.dev, dsl, NULL); dsl = VK_NULL_HANDLE; }
            pthread_mutex_unlock(&mu);
            return NO;
        }
    }
    *outDsl = dsl; *outPl = pl; *outPipe = pipe;
    pthread_mutex_unlock(&mu);
    return YES;
}

- (uint64_t)rdna4ICBCPUReadbackCount { return atomic_load(&_cpuReadbackCount); }

- (id)rdna4GPUBacking {
    pthread_mutex_lock(&_mu);
    RDNA4Buffer *b = _gpuBacking;
    pthread_mutex_unlock(&_mu);
    return b;
}

- (BOOL)rdna4RegisterCandidatePipelineLocked:(id)pipeline {
    if (_isCompute) {
        if (![pipeline isKindOfClass:[RDNA4ComputePipelineState class]]) return NO;
        RDNA4ComputePipelineState *pso = (RDNA4ComputePipelineState *)pipeline;
        if ([pso device] != _dev || ![pso supportIndirectCommandBuffers]) return NO;
        uint64_t tok = [pso rdna4ICBToken];
        if (!tok) return NO;
        for (id existing in _candidatePipelines) {
            if ([existing rdna4ICBToken] == tok) return YES;
        }
        if (_candidatePipelines.count >= RDNA4_ICB_MAX_CANDIDATE_PIPELINES) return NO;
        [_candidatePipelines addObject:pso];
        return YES;
    }
    if (![pipeline isKindOfClass:[RDNA4RenderPipelineState class]]) return NO;
    RDNA4RenderPipelineState *pso = (RDNA4RenderPipelineState *)pipeline;
    if ([pso device] != _dev || ![pso supportIndirectCommandBuffers]) return NO;
    uint64_t tok = [pso rdna4ICBToken];
    if (!tok) return NO;
    for (id existing in _candidatePipelines) {
        if ([existing rdna4ICBToken] == tok) return YES;
    }
    if (_candidatePipelines.count >= RDNA4_ICB_MAX_CANDIDATE_PIPELINES) return NO;
    [_candidatePipelines addObject:pso];
    return YES;
}

- (BOOL)rdna4RegisterCandidatePipeline:(id)pipeline {
    pthread_mutex_lock(&_mu);
    BOOL ok = [self rdna4RegisterCandidatePipelineLocked:pipeline];
    pthread_mutex_unlock(&_mu);
    return ok;
}

- (uint64_t)rdna4EnsureGPUBackingAddress {
    pthread_mutex_lock(&_mu);
    if (_gpuBacking) {
        uint64_t addr = [_gpuBacking gpuAddress];
        pthread_mutex_unlock(&_mu);
        return addr;
    }
    if (_commandCount > RDNA4_ICB_MAX_GPU_SLOTS || !RDNA4R.bufferAddress) {
        pthread_mutex_unlock(&_mu);
        return 0;
    }
    uint64_t backingBytes = rdna4_icb_backing_bytes((uint32_t)_commandCount);
    NSError *err = nil;
    RDNA4Buffer *backing = [[RDNA4Buffer alloc] initWithDevice:_dev
                                                        length:(NSUInteger)backingBytes
                                                       options:MTLResourceStorageModePrivate | RDNA4_OPT_NOZERO
                                                         error:&err];
    if (!backing || ![backing gpuAddress]) {
        pthread_mutex_unlock(&_mu);
        return 0;
    }
    NSUInteger recordCount = _records.count;
    size_t stgBytes = RDNA4_ICB_HEADER_BYTES + (size_t)recordCount * RDNA4_ICB_SLOT_BYTES;
    RDNA4Buffer *stg = [[RDNA4Buffer alloc] initWithDevice:_dev length:stgBytes options:0 error:&err];
    if (!stg || !stg.contents) {
        pthread_mutex_unlock(&_mu);
        return 0;
    }
    uint8_t *dst = (uint8_t *)stg.contents;
    rdna4_icb_header_t hdr = {0};
    hdr.magic = RDNA4_ICB_MAGIC;
    hdr.max_command_count = (uint32_t)_commandCount;
    hdr.command_types = (uint32_t)(_commandTypes & RDNA4_ICB_CMD_MASK);
    hdr.flags = (_inheritPipelineState ? RDNA4_ICB_FLAG_INHERIT_PIPELINE : 0u) |
                (_inheritBuffers ? RDNA4_ICB_FLAG_INHERIT_BUFFERS : 0u);
    hdr.max_vertex_binds = (uint32_t)(_isCompute ? _maxKernelBufferBindCount : _maxVertexBufferBindCount);
    hdr.max_fragment_binds = (uint32_t)(_isCompute ? 0u : _maxFragmentBufferBindCount);
    hdr.slot_stride = RDNA4_ICB_SLOT_BYTES;
    memcpy(dst, &hdr, sizeof hdr);

    NSUInteger nCopies = 1 + recordCount;
    VkBufferCopy *copies = calloc(nCopies, sizeof *copies);
    if (!copies) { pthread_mutex_unlock(&_mu); return 0; }
    copies[0] = (VkBufferCopy){ [stg rdna4VkOffset], [backing rdna4VkOffset], RDNA4_ICB_HEADER_BYTES };
    NSUInteger ci = 1;
    BOOL candOk = YES;
    for (NSNumber *key in _records) {
        NSUInteger idx = key.unsignedIntegerValue;
        RDNA4ICBRecord *rec = _records[key];
        rdna4_icb_slot_t *slotPtr = (rdna4_icb_slot_t *)(dst + RDNA4_ICB_HEADER_BYTES + (ci - 1) * RDNA4_ICB_SLOT_BYTES);
        id pso = nil;
        rdna4icb_fill_slot_from_record(rec, _isCompute, slotPtr, &pso);
        if (pso && ![self rdna4RegisterCandidatePipelineLocked:pso]) { candOk = NO; break; }
        rdna4icb_retain_record_resources(_gpuRetained, rec);
        copies[ci] = (VkBufferCopy){
            [stg rdna4VkOffset] + RDNA4_ICB_HEADER_BYTES + (VkDeviceSize)(ci - 1) * RDNA4_ICB_SLOT_BYTES,
            [backing rdna4VkOffset] + RDNA4_ICB_HEADER_BYTES + (VkDeviceSize)idx * RDNA4_ICB_SLOT_BYTES,
            RDNA4_ICB_SLOT_BYTES
        };
        ci++;
    }
    if (!candOk) { free(copies); pthread_mutex_unlock(&_mu); return 0; }
    BOOL uploaded = rdna4_oneshot(&err, ^(VkCommandBuffer cmd) {
        vkCmdFillBuffer(cmd, [backing vkBuffer], [backing rdna4VkOffset], backingBytes, 0);
        rdna4_full_barrier(cmd);
        vkCmdCopyBuffer(cmd, [stg vkBuffer], [backing vkBuffer], (uint32_t)nCopies, copies);
        rdna4_full_barrier(cmd);
    }, NULL);
    free(copies);
    if (!uploaded) { pthread_mutex_unlock(&_mu); return 0; }
    _gpuBacking = backing;
    [_cpuDirtySlots removeAllObjects];
    _cpuFullReset = NO;
    [_cpuResetRanges removeAllObjects];
    uint64_t addr = [_gpuBacking gpuAddress];
    pthread_mutex_unlock(&_mu);
    return addr;
}

- (BOOL)rdna4SyncCPUWritesToGPU:(NSError **)err {
    pthread_mutex_lock(&_mu);
    if (!_gpuBacking || (!_cpuDirtySlots.count && !_cpuFullReset && !_cpuResetRanges.count)) {
        pthread_mutex_unlock(&_mu);
        return YES;
    }
    RDNA4Buffer *backing = _gpuBacking;
    BOOL fullReset = _cpuFullReset;
    NSArray<NSValue *> *resetRanges = [_cpuResetRanges copy];
    NSArray<NSNumber *> *keys = [_cpuDirtySlots allObjects];
    NSMutableArray *recs = [NSMutableArray arrayWithCapacity:keys.count];
    for (NSNumber *k in keys) {
        RDNA4ICBRecord *rec = _records[k];
        if (rec) {
            id recPso = _isCompute ? rec.computePipeline : (id)rec.pipeline;
            if (recPso && ![self rdna4RegisterCandidatePipelineLocked:recPso]) {
                pthread_mutex_unlock(&_mu);
                if (err) *err = rdna4_err(120, @"indirect command buffer exceeds 32 candidate pipelines");
                return NO;
            }
            rdna4icb_retain_record_resources(_gpuRetained, rec);
            [recs addObject:rec];
        } else {
            [recs addObject:[NSNull null]];
        }
    }
    NSUInteger nRec = keys.count;
    RDNA4Buffer *stg = nil;
    VkBufferCopy *copies = NULL;
    if (nRec) {
        stg = [[RDNA4Buffer alloc] initWithDevice:_dev length:nRec * RDNA4_ICB_SLOT_BYTES options:0 error:err];
        copies = calloc(nRec, sizeof *copies);
        if (!stg || !stg.contents || !copies) {
            free(copies);
            pthread_mutex_unlock(&_mu);
            return NO;
        }
        uint8_t *dst = (uint8_t *)stg.contents;
        for (NSUInteger i = 0; i < nRec; i++) {
            id entry = recs[i];
            RDNA4ICBRecord *rec = (entry == [NSNull null]) ? nil : (RDNA4ICBRecord *)entry;
            rdna4icb_fill_slot_from_record(rec, _isCompute, (rdna4_icb_slot_t *)(dst + i * RDNA4_ICB_SLOT_BYTES), NULL);
            copies[i] = (VkBufferCopy){
                [stg rdna4VkOffset] + (VkDeviceSize)i * RDNA4_ICB_SLOT_BYTES,
                [backing rdna4VkOffset] + RDNA4_ICB_HEADER_BYTES + (VkDeviceSize)keys[i].unsignedIntegerValue * RDNA4_ICB_SLOT_BYTES,
                RDNA4_ICB_SLOT_BYTES
            };
        }
    }
    VkDeviceSize totalSlotBytes = (VkDeviceSize)_commandCount * RDNA4_ICB_SLOT_BYTES;
    BOOL synced = rdna4_oneshot(err, ^(VkCommandBuffer cmd) {
        if (fullReset && totalSlotBytes) {
            vkCmdFillBuffer(cmd, [backing vkBuffer], [backing rdna4VkOffset] + RDNA4_ICB_HEADER_BYTES, totalSlotBytes, 0);
        } else {
            for (NSValue *v in resetRanges) {
                NSRange r = v.rangeValue;
                if (!r.length) continue;
                vkCmdFillBuffer(cmd, [backing vkBuffer],
                                [backing rdna4VkOffset] + RDNA4_ICB_HEADER_BYTES + (VkDeviceSize)r.location * RDNA4_ICB_SLOT_BYTES,
                                (VkDeviceSize)r.length * RDNA4_ICB_SLOT_BYTES, 0);
            }
        }
        if (fullReset || resetRanges.count) rdna4_full_barrier(cmd);
        if (nRec) {
            vkCmdCopyBuffer(cmd, [stg vkBuffer], [backing vkBuffer], (uint32_t)nRec, copies);
            rdna4_full_barrier(cmd);
        }
    }, NULL);
    free(copies);
    if (synced) {
        [_cpuDirtySlots removeAllObjects];
        _cpuFullReset = NO;
        [_cpuResetRanges removeAllObjects];
    }
    pthread_mutex_unlock(&_mu);
    return synced;
}

- (BOOL)rdna4PrepareForGPUProducerOnCommandBuffer:(id)cb {
    if (!cb || !RDNA4R.bufferAddress) return NO;
    if (![self rdna4EnsureGPUBackingAddress]) {
        [(RDNA4CommandBuffer *)cb rdna4Fail:@"cannot allocate GPU backing for indirect command buffer"];
        return NO;
    }
    NSError *err = nil;
    if (![self rdna4SyncCPUWritesToGPU:&err]) {
        [(RDNA4CommandBuffer *)cb rdna4Fail:err.localizedDescription ?: @"cannot synchronize ICB CPU writes before compute"];
        return NO;
    }
    pthread_mutex_lock(&_mu);
    _gpuProduced = YES;
    RDNA4Buffer *backing = _gpuBacking;
    pthread_mutex_unlock(&_mu);
    [(RDNA4CommandBuffer *)cb rdna4RetainEncoded:self];
    if (backing) [(RDNA4CommandBuffer *)cb rdna4RetainEncoded:backing];
    return YES;
}
#endif

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

#ifdef VK_VERSION_1_0
    pthread_mutex_lock(&_mu);
    BOOL useGPU = (_gpuBacking != nil || _gpuProduced ||
                   (RDNA4R.bufferAddress && _commandCount <= RDNA4_ICB_MAX_GPU_SLOTS));
    pthread_mutex_unlock(&_mu);
    if (useGPU) {
        if (![self rdna4EnsureGPUBackingAddress]) {
            if (reason) *reason = @"cannot allocate GPU backing for indirect command buffer reset";
            return NO;
        }
        NSError *syncErr = nil;
        if (![self rdna4SyncCPUWritesToGPU:&syncErr]) {
            if (reason) *reason = syncErr.localizedDescription ?: @"cannot synchronize ICB before GPU reset";
            return NO;
        }
        VkCommandBuffer cmd = [commandBuffer vk];
        if (!cmd) {
            if (reason) *reason = @"command buffer lacks Vulkan backing for ICB reset";
            return NO;
        }
        pthread_mutex_lock(&_mu);
        _gpuProduced = YES;
        RDNA4Buffer *backing = _gpuBacking;
        pthread_mutex_unlock(&_mu);
        [commandBuffer rdna4RetainEncoded:self];
        [commandBuffer rdna4RetainEncoded:backing];
        VkDeviceSize off = [backing rdna4VkOffset] + RDNA4_ICB_HEADER_BYTES + (VkDeviceSize)range.location * RDNA4_ICB_SLOT_BYTES;
        VkDeviceSize bytes = (VkDeviceSize)range.length * RDNA4_ICB_SLOT_BYTES;
        vkCmdFillBuffer(cmd, [backing vkBuffer], off, bytes, 0);
        rdna4_full_barrier(cmd);
        return YES;
    }
#endif

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
        else {
            problem = nil;
#ifdef VK_VERSION_1_0
            if (_gpuBacking) {
                if (range.location == 0 && range.length == _commandCount) {
                    _cpuFullReset = YES;
                    _gpuProduced = NO;
                    [_candidatePipelines removeAllObjects];
                    [_cpuDirtySlots removeAllObjects];
                    [_cpuResetRanges removeAllObjects];
                    [_gpuRetained removeAllObjects];
                } else if (range.length) {
                    for (NSNumber *k in [_cpuDirtySlots allObjects]) {
                        NSUInteger idx = k.unsignedIntegerValue;
                        if (idx >= range.location && idx - range.location < range.length)
                            [_cpuDirtySlots removeObject:k];
                    }
                    [_cpuResetRanges addObject:[NSValue valueWithRange:range]];
                }
            }
#endif
        }
    } else {
        _fatalError = [problem copy];
    }
    pthread_mutex_unlock(&_mu);
    if (problem)
        RDNA4LOG_WARN("indirect command buffer resetRange [%lu,+%lu) refused: %s",
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

#ifdef VK_VERSION_1_0
    pthread_mutex_lock(&first->_mu);
    if (second != first) pthread_mutex_lock(&second->_mu);
    BOOL useGPU = (source->_gpuBacking != nil || source->_gpuProduced ||
                   destination->_gpuBacking != nil || destination->_gpuProduced ||
                   (RDNA4R.bufferAddress && source->_commandCount <= RDNA4_ICB_MAX_GPU_SLOTS &&
                    destination->_commandCount <= RDNA4_ICB_MAX_GPU_SLOTS));
    if (second != first) pthread_mutex_unlock(&second->_mu);
    pthread_mutex_unlock(&first->_mu);
    if (useGPU) {
        if (![source rdna4EnsureGPUBackingAddress] || ![destination rdna4EnsureGPUBackingAddress]) {
            if (reason) *reason = @"cannot allocate GPU backing for indirect command buffer copy";
            return NO;
        }
        NSError *syncErr = nil;
        if (![source rdna4SyncCPUWritesToGPU:&syncErr] ||
            (destination != source && ![destination rdna4SyncCPUWritesToGPU:&syncErr])) {
            if (reason) *reason = syncErr.localizedDescription ?: @"cannot synchronize ICB before GPU copy";
            return NO;
        }
        pthread_mutex_lock(&first->_mu);
        if (second != first) pthread_mutex_lock(&second->_mu);
        NSMutableArray *merged = [destination->_candidatePipelines mutableCopy];
        BOOL candFit = YES;
        for (id cand in source->_candidatePipelines) {
            uint64_t tok = [cand rdna4ICBToken];
            BOOL seen = NO;
            for (id ex in merged) {
                if ([ex rdna4ICBToken] == tok) { seen = YES; break; }
            }
            if (!seen) {
                if (merged.count >= RDNA4_ICB_MAX_CANDIDATE_PIPELINES) { candFit = NO; break; }
                [merged addObject:cand];
            }
        }
        if (!candFit) {
            if (second != first) pthread_mutex_unlock(&second->_mu);
            pthread_mutex_unlock(&first->_mu);
            if (reason) *reason = @"copy destination indirect command buffer exceeds 32 candidate pipelines";
            return NO;
        }
        VkCommandBuffer cmd = [commandBuffer vk];
        if (!cmd) {
            if (second != first) pthread_mutex_unlock(&second->_mu);
            pthread_mutex_unlock(&first->_mu);
            if (reason) *reason = @"command buffer lacks Vulkan backing for ICB copy";
            return NO;
        }
        destination->_candidatePipelines = merged;
        destination->_gpuProduced = YES;
        if (destination != source) [destination->_gpuRetained unionSet:source->_gpuRetained];
        RDNA4Buffer *srcBack = source->_gpuBacking;
        RDNA4Buffer *dstBack = destination->_gpuBacking;
        if (second != first) pthread_mutex_unlock(&second->_mu);
        pthread_mutex_unlock(&first->_mu);

        [commandBuffer rdna4RetainEncoded:source];
        [commandBuffer rdna4RetainEncoded:destination];
        [commandBuffer rdna4RetainEncoded:srcBack];
        [commandBuffer rdna4RetainEncoded:dstBack];
        VkDeviceSize srcOff = [srcBack rdna4VkOffset] + RDNA4_ICB_HEADER_BYTES + (VkDeviceSize)sourceRange.location * RDNA4_ICB_SLOT_BYTES;
        VkDeviceSize dstOff = [dstBack rdna4VkOffset] + RDNA4_ICB_HEADER_BYTES + (VkDeviceSize)destinationIndex * RDNA4_ICB_SLOT_BYTES;
        VkDeviceSize bytes = (VkDeviceSize)sourceRange.length * RDNA4_ICB_SLOT_BYTES;
        BOOL overlap = (source == destination) &&
                       (sourceRange.location < destinationIndex + sourceRange.length) &&
                       (destinationIndex < sourceRange.location + sourceRange.length);
        if (overlap) {
            NSError *scErr = nil;
            RDNA4Buffer *scratch = [[RDNA4Buffer alloc] initWithDevice:source->_dev
                                                                length:(NSUInteger)bytes
                                                               options:MTLResourceStorageModePrivate | RDNA4_OPT_NOZERO
                                                                 error:&scErr];
            if (!scratch) {
                if (reason) *reason = scErr.localizedDescription ?: @"cannot allocate scratch buffer for overlapping ICB copy";
                return NO;
            }
            [commandBuffer rdna4RetainEncoded:scratch];
            VkBufferCopy c1 = { srcOff, [scratch rdna4VkOffset], bytes };
            vkCmdCopyBuffer(cmd, [srcBack vkBuffer], [scratch vkBuffer], 1, &c1);
            rdna4_full_barrier(cmd);
            VkBufferCopy c2 = { [scratch rdna4VkOffset], dstOff, bytes };
            vkCmdCopyBuffer(cmd, [scratch vkBuffer], [dstBack vkBuffer], 1, &c2);
            rdna4_full_barrier(cmd);
        } else {
            VkBufferCopy c = { srcOff, dstOff, bytes };
            vkCmdCopyBuffer(cmd, [srcBack vkBuffer], [dstBack vkBuffer], 1, &c);
            rdna4_full_barrier(cmd);
        }
        return YES;
    }
#endif

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
    if (!problem && _isCompute)
        problem = @"cannot execute a compute indirect command buffer on a render encoder";
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
#ifdef VK_VERSION_1_0
                if (!(_commandTypes & RDNA4ICB_TYPE_DRAW_PATCHES))
                    problem = [NSString stringWithFormat:@"slot %lu uses drawPatches the descriptor did not enable", (unsigned long)slot];
#else
                problem = [NSString stringWithFormat:@"slot %lu uses drawPatches (requires Vulkan runtime)", (unsigned long)slot];
#endif
                break;
            case RDNA4ICBCommandDrawIndexedPatches:
#ifdef VK_VERSION_1_0
                if (!(_commandTypes & RDNA4ICB_TYPE_DRAW_INDEXED_PATCHES))
                    problem = [NSString stringWithFormat:@"slot %lu uses drawIndexedPatches the descriptor did not enable", (unsigned long)slot];
#else
                problem = [NSString stringWithFormat:@"slot %lu uses drawIndexedPatches (requires Vulkan runtime)", (unsigned long)slot];
#endif
                break;
            case RDNA4ICBCommandConcurrentDispatch:
            case RDNA4ICBCommandConcurrentDispatchThreads:
                problem = [NSString stringWithFormat:@"slot %lu uses a compute command on a render encoder", (unsigned long)slot];
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
#ifdef VK_VERSION_1_0
        {
            RDNA4RenderPipelineState *use = [(RDNA4RenderPipelineState *)pipeline rdna4Real] ?: pipeline;
            BOOL isPatch = (record.kind == RDNA4ICBCommandDrawPatches || record.kind == RDNA4ICBCommandDrawIndexedPatches);
            if ([use rdna4IsTessellation] != isPatch) {
                problem = [NSString stringWithFormat:@"slot %lu %s pipeline/command kind mismatch",
                           (unsigned long)slot, isPatch ? "patch command requires tessellation" : "draw command requires non-tessellation"];
                break;
            }
            if (isPatch) {
                const rdna4_tess_cfg_t *cfg = [use tessCfg];
                id device = [encoder rdna4ICBDevice];
                if (record.patchControlPoints > RDNA4R.maxTessPatchSize) {
                    problem = [NSString stringWithFormat:@"slot %lu patch control points exceed max (%u)", (unsigned long)slot, RDNA4R.maxTessPatchSize];
                    break;
                }
                if (!record.tessellationFactorBuffer || [record.tessellationFactorBuffer device] != device ||
                    (record.tessellationFactorBufferOffset & 3) ||
                    record.tessellationFactorBufferOffset > [record.tessellationFactorBuffer length]) {
                    problem = [NSString stringWithFormat:@"slot %lu tessellation factor buffer is invalid", (unsigned long)slot];
                    break;
                }
                uint32_t fbytes = rdna4_tess_factor_stride(cfg);
                if ((cfg->factor_step == RDNA4_TESS_FSTEP_CONST || (record.patchCount && record.instanceCount)) &&
                    fbytes > [record.tessellationFactorBuffer length] - record.tessellationFactorBufferOffset) {
                    problem = [NSString stringWithFormat:@"slot %lu tessellation factor buffer range is smaller than %u bytes", (unsigned long)slot, fbytes];
                    break;
                }
                if (record.kind == RDNA4ICBCommandDrawIndexedPatches) {
                    if (cfg->cp_idx_type == RDNA4_TESS_CPIDX_NONE) {
                        problem = [NSString stringWithFormat:@"slot %lu indexed patch draw requires tessellationControlPointIndexType != None", (unsigned long)slot];
                        break;
                    }
                    NSUInteger align = cfg->cp_idx_type == RDNA4_TESS_CPIDX_U32 ? 4u : 2u;
                    NSUInteger patchEnd = record.patchCount ? (record.patchStart + record.patchCount) : 0;
                    NSUInteger cpCount = patchEnd * record.patchControlPoints;
                    if (!record.controlPointIndexBuffer || [record.controlPointIndexBuffer device] != device ||
                        (record.controlPointIndexBufferOffset & (align - 1u)) ||
                        record.controlPointIndexBufferOffset > [record.controlPointIndexBuffer length] ||
                        cpCount > ([record.controlPointIndexBuffer length] - record.controlPointIndexBufferOffset) / align) {
                        problem = [NSString stringWithFormat:@"slot %lu control-point index buffer or alignment is invalid", (unsigned long)slot];
                        break;
                    }
                }
            }
        }
#endif
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
#ifdef VK_VERSION_1_0
        else if (record.kind == RDNA4ICBCommandDrawPatches) {
            if (record.patchCount && record.instanceCount) {
                [encoder setTessellationFactorBuffer:record.tessellationFactorBuffer
                                              offset:record.tessellationFactorBufferOffset
                                      instanceStride:record.tessellationFactorInstanceStride];
                [encoder drawPatches:record.patchControlPoints
                          patchStart:record.patchStart
                          patchCount:record.patchCount
                    patchIndexBuffer:record.patchIndexBuffer
              patchIndexBufferOffset:record.patchIndexBufferOffset
                       instanceCount:record.instanceCount
                        baseInstance:record.baseInstance];
            }
        } else if (record.kind == RDNA4ICBCommandDrawIndexedPatches) {
            if (record.patchCount && record.instanceCount) {
                [encoder setTessellationFactorBuffer:record.tessellationFactorBuffer
                                              offset:record.tessellationFactorBufferOffset
                                      instanceStride:record.tessellationFactorInstanceStride];
                [encoder drawIndexedPatches:record.patchControlPoints
                                 patchStart:record.patchStart
                                 patchCount:record.patchCount
                           patchIndexBuffer:record.patchIndexBuffer
                     patchIndexBufferOffset:record.patchIndexBufferOffset
                    controlPointIndexBuffer:record.controlPointIndexBuffer
              controlPointIndexBufferOffset:record.controlPointIndexBufferOffset
                              instanceCount:record.instanceCount
                               baseInstance:record.baseInstance];
            }
        }
#endif
        replayed++;
    }
    return replayed;
}
#ifdef VK_VERSION_1_0
- (NSUInteger)rdna4ExecuteComputeOnEncoder:(id)encoderObj
                                     range:(NSRange)range
                                    reason:(NSString **)reason {
    RDNA4ComputeEncoder *encoder = (RDNA4ComputeEncoder *)encoderObj;
    NSMutableArray<RDNA4ICBRecord *> *snapshot = nil;
    NSString *problem = nil;
    pthread_mutex_lock(&_mu);
    problem = rdna4icb_range_problem(range, _commandCount, YES);
    if (!problem && _fatalError) problem = _fatalError;
    if (!problem && !_isCompute)
        problem = @"cannot execute a render indirect command buffer on a compute encoder";
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

    // Validate every slot in the snapshot atomically before dispatching the first command.
    rdna4dc_caps caps = rdna4_device_caps();
    for (NSUInteger offset = 0; !problem && offset < snapshot.count; ++offset) {
        RDNA4ICBRecord *record = snapshot[offset];
        NSUInteger slot = range.location + offset;
        if (record.error) {
            problem = [NSString stringWithFormat:@"slot %lu: %@", (unsigned long)slot, record.error];
            break;
        }
        if (record.kind == RDNA4ICBCommandEmpty) continue;
        switch (record.kind) {
            case RDNA4ICBCommandConcurrentDispatch:
                if (!(_commandTypes & RDNA4ICB_TYPE_CONCURRENT_DISPATCH))
                    problem = [NSString stringWithFormat:@"slot %lu uses concurrentDispatchThreadgroups the descriptor did not enable", (unsigned long)slot];
                break;
            case RDNA4ICBCommandConcurrentDispatchThreads:
                if (!(_commandTypes & RDNA4ICB_TYPE_CONCURRENT_DISPATCH_THREADS))
                    problem = [NSString stringWithFormat:@"slot %lu uses concurrentDispatchThreads the descriptor did not enable", (unsigned long)slot];
                break;
            default:
                problem = [NSString stringWithFormat:@"slot %lu is not a compute indirect command", (unsigned long)slot];
                break;
        }
        if (problem) break;

        id pipeline = _inheritPipelineState ? [encoder rdna4ICBComputePipeline] : record.computePipeline;
        if (![pipeline isKindOfClass:[RDNA4ComputePipelineState class]] || !rdna4icb_pipeline_supports_icb(pipeline)) {
            problem = [NSString stringWithFormat:@"slot %lu has no ICB-capable %s compute pipeline",
                       (unsigned long)slot, _inheritPipelineState ? "inherited" : "explicit"];
            break;
        }
        RDNA4ComputePipelineState *use = [(RDNA4ComputePipelineState *)pipeline rdna4Real] ?: (RDNA4ComputePipelineState *)pipeline;
        if ([use device] != _dev) {
            problem = [NSString stringWithFormat:@"slot %lu compute pipeline belongs to a different device", (unsigned long)slot];
            break;
        }
        if (![use noop]) {
            MTLSize tg = record.threadgroupSize;
            MTLSize grid = record.gridSize;
            if (!rdna4dc_workgroup_ok(&caps, tg.width, tg.height, tg.depth)) {
                problem = [NSString stringWithFormat:@"slot %lu threadsPerThreadgroup %lux%lux%lu is zero or exceeds device limits",
                           (unsigned long)slot, (unsigned long)tg.width, (unsigned long)tg.height, (unsigned long)tg.depth];
                break;
            }
            if ([use mode] == 0) {
                const uint32_t *nom = [use nominalLocal];
                if (tg.width != nom[0] || tg.height != nom[1] || tg.depth != nom[2]) {
                    problem = [NSString stringWithFormat:@"slot %lu threadsPerThreadgroup %lux%lux%lu != Workgroups local size %ux%ux%u",
                               (unsigned long)slot, (unsigned long)tg.width, (unsigned long)tg.height, (unsigned long)tg.depth,
                               nom[0], nom[1], nom[2]];
                    break;
                }
                if (record.kind == RDNA4ICBCommandConcurrentDispatchThreads &&
                    grid.width && grid.height && grid.depth &&
                    ((grid.width % tg.width) || (grid.height % tg.height) || (grid.depth % tg.depth))) {
                    problem = [NSString stringWithFormat:@"slot %lu non-multiple concurrentDispatchThreads requires a ThreadsDynamic kernel",
                               (unsigned long)slot];
                    break;
                }
            }
            NSUInteger totalTgMem = 0;
            for (NSNumber *tgIdx in record.threadgroupMemLengths) {
                NSUInteger idx = tgIdx.unsignedIntegerValue;
                NSUInteger len = record.threadgroupMemLengths[tgIdx].unsignedIntegerValue;
                if (!rdna4_icb_compute_tg_mem_ok((uint32_t)idx, (uint64_t)len, caps.shared_memory) ||
                    len > caps.shared_memory - totalTgMem) {
                    problem = [NSString stringWithFormat:@"slot %lu threadgroup memory at index %lu (%lu B) exceeds alignment or device limit (%u B)",
                               (unsigned long)slot, (unsigned long)idx, (unsigned long)len, caps.shared_memory];
                    break;
                }
                totalTgMem += len;
            }
            if (problem) break;
            if ([use rdna4LumaWorkgroup]) {
                NSUInteger tg0 = record.threadgroupMemLengths[@(0)].unsignedIntegerValue;
                uint32_t tgArr[3] = { (uint32_t)tg.width, (uint32_t)tg.height, (uint32_t)tg.depth };
                if (tg0 && (!rdna4_luma_workgroup_bytes_ok(tg0, caps.shared_memory) ||
                            !rdna4_luma_workgroup_fits(tg0, tgArr))) {
                    problem = [NSString stringWithFormat:@"slot %lu luma threadgroup memory (%lu B) is incompatible with threadgroup size",
                               (unsigned long)slot, (unsigned long)tg0];
                    break;
                }
            }
        }
        if (_inheritBuffers) {
            if (![encoder rdna4ICBInheritedComputeBuffersSatisfyPipeline:use]) {
                problem = [NSString stringWithFormat:@"slot %lu inherits buffers that do not satisfy its compute pipeline", (unsigned long)slot];
                break;
            }
        } else if (![encoder rdna4ICBExplicitComputeBuffersSatisfyRecord:record pipeline:use]) {
            problem = [NSString stringWithFormat:@"slot %lu explicit kernel buffers do not satisfy its compute pipeline", (unsigned long)slot];
            break;
        }
    }

    if (problem) {
        if (reason) *reason = problem;
        return 0;
    }

    for (RDNA4ICBRecord *record in snapshot) {
        [encoder rdna4ICBRetainResource:record.computePipeline];
        for (RDNA4ICBBind *bind in record.kernelBinds.allValues)
            [encoder rdna4ICBRetainResource:bind.buffer];
    }

    NSUInteger replayed = 0;
    id<MTLComputeCommandEncoder> direct = (id<MTLComputeCommandEncoder>)encoder;
    for (RDNA4ICBRecord *record in snapshot) {
        if (record.hasBarrier) [encoder rdna4ICBComputeBarrier];
        if (record.kind == RDNA4ICBCommandEmpty) continue;
        [encoder rdna4ICBResetPerSlotComputeState];
        if (!_inheritPipelineState) [direct setComputePipelineState:(id)record.computePipeline];
        if (!_inheritBuffers) {
            [encoder rdna4ICBClearExplicitComputeBuffers];
            for (NSNumber *slot in record.kernelBinds) {
                RDNA4ICBBind *bind = record.kernelBinds[slot];
                [direct setBuffer:(id)bind.buffer offset:bind.offset atIndex:slot.unsignedIntegerValue];
            }
        }
        for (NSNumber *tgIdx in record.threadgroupMemLengths) {
            [direct setThreadgroupMemoryLength:record.threadgroupMemLengths[tgIdx].unsignedIntegerValue
                                       atIndex:tgIdx.unsignedIntegerValue];
        }
        if (record.hasStageInRegion) [direct setStageInRegion:record.stageInRegion];
        if (record.gridSize.width && record.gridSize.height && record.gridSize.depth) {
            if (record.kind == RDNA4ICBCommandConcurrentDispatch)
                [direct dispatchThreadgroups:record.gridSize threadsPerThreadgroup:record.threadgroupSize];
            else
                [direct dispatchThreads:record.gridSize threadsPerThreadgroup:record.threadgroupSize];
        }
        replayed++;
    }
    return replayed;
}

- (void)rdna4ExecuteComputeGPUOnEncoder:(id)encoder
                            directRange:(NSRange)range
                         indirectBuffer:(id)indirectBuf
                         indirectOffset:(NSUInteger)indirectOff
                            useIndirect:(BOOL)useIndirect {
    [(RDNA4ComputeEncoder *)encoder rdna4ExecuteComputeICBGPU:self
                                                  directRange:range
                                               indirectBuffer:indirectBuf
                                               indirectOffset:indirectOff
                                                  useIndirect:useIndirect];
}
#endif
@end

#endif
