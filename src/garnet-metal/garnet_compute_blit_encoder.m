// garnet_compute_blit_encoder.m - GarnetMetal modular compilation unit.
#if !defined(GARNET_INTERNAL) && !defined(GARNET_METAL_MODULAR_BUILD)
#define GARNET_INTERNAL static
#endif
#ifdef GARNET_METAL_MODULAR_BUILD
#include "garnet_metal_internal.h"
#endif
#include "garnet_arguments_internal.h"
#include "garnet_rt_internal.h"
#include "garnet_texture_transfer.h"
#include "garnet_commands_internal.h"
#include "garnet_luma_workgroup.h"

// ---------------------------------------------------------------------------------------------------------------
// RDNA4ComputeEncoder (11e R2)
// ---------------------------------------------------------------------------------------------------------------




@implementation RDNA4ComputeEncoder
- (instancetype)initWithCommandBuffer:(id)cb dispatchType:(NSUInteger)dispatchType {
    self = [super initWithCommandBuffer:cb]; if (!self) return nil;
    _cb = cb;
    _dispatchType = garnet_comp_norm_dispatch_type(dispatchType);
    _pendingSerialBarrier = NO;
    _resourceBindings = [RDNA4ResourceBindings new];
    _compSegSerial = [_cb rdna4SegmentSerial];
    _compStateDirty = YES;
    RDNA4LOGR("RDNA4ComputeEncoder %p (dispatchType %lu)", (__bridge void *)self, (unsigned long)_dispatchType);
    return self;
}
- (instancetype)initWithCommandBuffer:(id)cb compute:(int)dummy {
    (void)dummy;
    return [self initWithCommandBuffer:cb dispatchType:GARNET_COMP_DISPATCH_SERIAL];
}
- (instancetype)initWithCommandBuffer:(id)cb descriptor:(MTLComputePassDescriptor *)d {
    const NSUInteger dt = (d && [d respondsToSelector:@selector(dispatchType)])
        ? d.dispatchType : GARNET_COMP_DISPATCH_SERIAL;
    self = [self initWithCommandBuffer:cb dispatchType:dt];
    if (!self || !d) return self;
    if ([d respondsToSelector:@selector(sampleBufferAttachments)]) {
        MTLComputePassSampleBufferAttachmentDescriptorArray *sbas = d.sampleBufferAttachments;
        for (NSUInteger k = 0; sbas && k < RDNA4CNT_MAX_PASS_ATTACHMENTS; k++) {
            MTLComputePassSampleBufferAttachmentDescriptor *att = sbas[k];
            if (!att || !att.sampleBuffer) continue;
            NSString *cwhy = nil;
            id<RDNA4CounterSampleBufferInternal> csb = rdna4_counter_buffer_check(att.sampleBuffer, [_cb device], 0, NO, &cwhy);
            if (!csb || !rdna4_counter_pass_index_valid(csb, att.startOfEncoderSampleIndex) ||
                !rdna4_counter_pass_index_valid(csb, att.endOfEncoderSampleIndex)) {
                RDNA4LOG_WARN("compute pass: sampleBufferAttachments[%lu] invalid (%s); refused",
                              (unsigned long)k, cwhy ? cwhy.UTF8String : "sample index out of range");
                _ended = YES;
                [super endEncoding];
                return nil;
            }
        }
        for (NSUInteger k = 0; sbas && k < RDNA4CNT_MAX_PASS_ATTACHMENTS; k++) {
            MTLComputePassSampleBufferAttachmentDescriptor *att = sbas[k];
            if (!att || !att.sampleBuffer) continue;
            [_cb rdna4EnsureGfxSegment];
            id<RDNA4CounterSampleBufferInternal> csb = (id<RDNA4CounterSampleBufferInternal>)att.sampleBuffer;
            if (att.startOfEncoderSampleIndex != MTLCounterDontSample)
                [csb rdna4SampleOnCommandBuffer:_cb vkCommandBuffer:[_cb vk]
                                    sampleIndex:att.startOfEncoderSampleIndex barrier:NO
                                     stageFlags:VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT who:"compute-pass-start"];
            if (att.endOfEncoderSampleIndex != MTLCounterDontSample && _nCsbEnd < RDNA4CNT_MAX_PASS_ATTACHMENTS) {
                _csbEnd[_nCsbEnd] = csb;
                _csbEndIdx[_nCsbEnd] = att.endOfEncoderSampleIndex;
                _nCsbEnd++;
            }
        }
    }
    return self;
}
RDNA4_DNR(RDNA4ComputeEncoder)
- (void)rdna4OwnResource:(id)object kind:(uint32_t)kind
                fragment:(BOOL)fragment index:(NSUInteger)index {
    [_resourceBindings setObject:object
                             key:rdna4_resource_slot_key(kind, fragment, index)];
}
- (void)rdna4SnapshotResources {
    [_resourceBindings snapshotToCommandBuffer:_cb];
}
- (NSString *)label { return _lbl; }
- (void)setLabel:(NSString *)l { _lbl = [l copy]; }
- (NSUInteger)getType { return 2; }
- (NSUInteger)dispatchType { return _dispatchType; }
- (void)rdna4ResetComputeCache {
    _compStateDirty = YES;
    _boundPipe = VK_NULL_HANDLE;
    _boundLayout = VK_NULL_HANDLE;
    _boundSet = VK_NULL_HANDLE;
    _boundPushValid = NO;
    _boundPCValid = NO;
}
- (void)setComputePipelineState:(id)p {
    if (![p isKindOfClass:[RDNA4ComputePipelineState class]]) { [_cb rdna4Fail:@"setComputePipelineState: not an RDNA4ComputePipelineState"]; return; }
    _pso = p; _roReflection = [_pso reflection];
    _st.scanPlan = [_pso scanPlan];
    _st.rtAsRefl = [_pso rdna4RtAsRefl];
    [self rdna4ResetComputeCache];
    [self rdna4OwnResource:p kind:RDNA4_RESOURCE_PIPELINE
                  fragment:NO index:0];
}
- (void)setBuffer:(id)b offset:(NSUInteger)o atIndex:(NSUInteger)i {
    if (i >= 31 || (b && ![b isKindOfClass:[RDNA4Buffer class]])) { [_cb rdna4Fail:@"compute setBuffer: bad index or class"]; return; }
    rdna4_stage_set_buf(&_st, i, b, o);
    _st.rt.as[i] = nil;
    _st.rt.ift[i] = nil;
    _st.rt.iftMask &= ~(1u << i);
    _compStateDirty = YES;
    [self rdna4OwnResource:nil kind:RDNA4_RESOURCE_ACCEL fragment:NO index:i];
    [self rdna4OwnResource:b kind:RDNA4_RESOURCE_BUFFER fragment:NO index:i];
}
- (void)setBuffer:(id)b offset:(NSUInteger)o attributeStride:(NSUInteger)stride atIndex:(NSUInteger)i {
    if (stride != 0 && stride != (NSUInteger)-1)
        RDNA4LOG("compute setBuffer:attributeStride:%lu atIndex:%lu ignoring dynamic stride", (unsigned long)stride, (unsigned long)i);
    [self setBuffer:b offset:o atIndex:i];
}
- (void)setBufferOffset:(NSUInteger)o atIndex:(NSUInteger)i { if (i < 31) { _st.buf[i].off = o; _compStateDirty = YES; } }
- (void)setBuffers:(const id __unsafe_unretained *)b offsets:(const NSUInteger *)o withRange:(NSRange)r { for (NSUInteger k = 0; k < r.length; k++) [self setBuffer:b[k] offset:o[k] atIndex:r.location + k]; }
- (void)setBytes:(const void *)p length:(NSUInteger)n atIndex:(NSUInteger)i {
    if (i >= 31 || !p) { [_cb rdna4Fail:@"compute setBytes: bad index/bytes"]; return; }
    RDNA4Buffer *b = nil; NSUInteger off = 0;
    if ([_cb rdna4Bytes:p length:n buffer:&b offset:&off]) [self setBuffer:b offset:off atIndex:i];
}
- (void)setVisibleFunctionTable:(id<MTLVisibleFunctionTable>)t atBufferIndex:(NSUInteger)i {
    if ([_cb rdna4Error]) return;
    if (i >= 31) { [_cb rdna4Fail:@"compute setVisibleFunctionTable: buffer index >= 31"]; return; }
    if (!t) { [self setBuffer:nil offset:0 atIndex:i]; return; }
    if (![t isKindOfClass:[RDNA4VisibleFunctionTable class]] || [t device] != [_cb device]) {
        [_cb rdna4Fail:@"compute setVisibleFunctionTable: not an RDNA4VisibleFunctionTable for this device"];
        return;
    }
    RDNA4VisibleFunctionTable *vft = (RDNA4VisibleFunctionTable *)t;
    [vft rdna4RetainIntoCommandBuffer:_cb];
    [self setBuffer:[vft rdna4BackingBuffer] offset:0 atIndex:i];
}
- (void)setVisibleFunctionTables:(const id<MTLVisibleFunctionTable> __unsafe_unretained *)t withBufferRange:(NSRange)r {
    if (r.length == 0) return;
    if (!t || r.location > 31 || r.length > 31 - r.location) {
        [_cb rdna4Fail:@"compute setVisibleFunctionTables: invalid range or NULL array"];
        return;
    }
    for (NSUInteger k = 0; k < r.length; k++) [self setVisibleFunctionTable:t[k] atBufferIndex:r.location + k];
}
- (void)setAccelerationStructure:(id<MTLAccelerationStructure>)accel atBufferIndex:(NSUInteger)i {
    if ([_cb rdna4Error]) return;
    if (!rdna4_rt_encoder_bind_accel(_cb, [_cb device], "compute", &_st.rt, accel, i)) return;
    rdna4_stage_set_buf(&_st, i, nil, 0);
    _compStateDirty = YES;
    [self rdna4OwnResource:nil kind:RDNA4_RESOURCE_BUFFER fragment:NO index:i];
    [self rdna4OwnResource:accel kind:RDNA4_RESOURCE_ACCEL fragment:NO index:i];
}
- (void)setIntersectionFunctionTable:(id<MTLIntersectionFunctionTable>)t atBufferIndex:(NSUInteger)i {
    if ([_cb rdna4Error]) return;
    RDNA4Buffer *backing = nil;
    if (!rdna4_rt_encoder_bind_ift(_cb, [_cb device], "compute", &_st.rt, t, i, &backing)) return;
    [self setBuffer:backing offset:0 atIndex:i];
    if (i < RDNA4RTR_MAX_BUFFER_SLOTS) {
        _st.rt.ift[i] = t;
        if (t) _st.rt.iftMask |= (1u << i);
        else _st.rt.iftMask &= ~(1u << i);
    }
}
- (void)setIntersectionFunctionTables:(const id<MTLIntersectionFunctionTable> __unsafe_unretained *)t withBufferRange:(NSRange)r {
    if ([_cb rdna4Error]) return;
    (void)rdna4_rt_encoder_bind_ifts(_cb, [_cb device], "compute", &_st.rt, t, r,
                                     ^(RDNA4Buffer *buf, NSUInteger idx) {
                                         [self setBuffer:buf offset:0 atIndex:idx];
                                         id tbl = t[idx - r.location];
                                         if (idx < RDNA4RTR_MAX_BUFFER_SLOTS) {
                                             self->_st.rt.ift[idx] = tbl;
                                             if (tbl) self->_st.rt.iftMask |= (1u << idx);
                                             else self->_st.rt.iftMask &= ~(1u << idx);
                                         }
                                     });
}
- (void)setTexture:(id)t atIndex:(NSUInteger)i {
    if (i >= 128 || (t && ![t isKindOfClass:[RDNA4Texture class]])) { [_cb rdna4Fail:@"compute setTexture: bad index or class"]; return; }
    _st.tex[i] = t;
    _compStateDirty = YES;
    [self rdna4OwnResource:t kind:RDNA4_RESOURCE_TEXTURE fragment:NO index:i];
}
- (void)setTextures:(const id __unsafe_unretained *)t withRange:(NSRange)r { for (NSUInteger k = 0; k < r.length; k++) [self setTexture:t[k] atIndex:r.location + k]; }
- (void)setSamplerState:(id)s lodMinClamp:(float)a lodMaxClamp:(float)b
                 override:(BOOL)override atIndex:(NSUInteger)i {
    if ([_cb rdna4Error]) return;
    if (i >= 16 || (s && ![s isKindOfClass:[RDNA4SamplerState class]])) { [_cb rdna4Fail:@"compute setSamplerState: bad index or class"]; return; }
    RDNA4SamplerBinding *binding = nil;
    if (s) {
        NSError *error = nil;
        RDNA4SamplerState *sampler = (RDNA4SamplerState *)s;
        binding = override
            ? [sampler rdna4BindingWithLODMinClamp:a lodMaxClamp:b error:&error]
            : [sampler rdna4Binding:&error];
        if (!binding) {
            [_cb rdna4Fail:error.localizedDescription ?: @"compute sampler binding unavailable"];
            return;
        }
        [_cb rdna4RetainArgument:binding];
        if ([_cb rdna4Error]) return;
    }
    RDNA4SamplerBinding *old = _st.smp[i];
    _st.smp[i] = binding;
    _compStateDirty = YES;
    if (binding) {
        if (!_samplerBindings) _samplerBindings = [NSMutableSet set];
        [_samplerBindings addObject:binding];
    }
    if (old && old != binding) {
        BOOL used = NO;
        for (NSUInteger k = 0; k < 16 && !used; ++k) used = _st.smp[k] == old;
        if (!used) [_samplerBindings removeObject:old];
    }
}
- (void)setSamplerState:(id)s atIndex:(NSUInteger)i {
    [self setSamplerState:s lodMinClamp:0 lodMaxClamp:0 override:NO atIndex:i];
}
- (void)setSamplerState:(id)s lodMinClamp:(float)a lodMaxClamp:(float)b atIndex:(NSUInteger)i { [self setSamplerState:s lodMinClamp:a lodMaxClamp:b override:YES atIndex:i]; }
- (void)setSamplerStates:(const id __unsafe_unretained *)s withRange:(NSRange)r { for (NSUInteger k = 0; k < r.length; k++) [self setSamplerState:s[k] atIndex:r.location + k]; }
- (void)setSamplerStates:(const id __unsafe_unretained *)s
             lodMinClamps:(const float *)lo
             lodMaxClamps:(const float *)hi
                withRange:(NSRange)r {
    if (r.length && (!s || !lo || !hi)) {
        [_cb rdna4Fail:@"compute setSamplerStates:lodMinClamps:lodMaxClamps: nil array"];
        return;
    }
    for (NSUInteger k = 0; k < r.length; k++)
        [self setSamplerState:s[k] lodMinClamp:lo[k] lodMaxClamp:hi[k]
            override:YES atIndex:r.location + k];
}
- (void)setThreadgroupMemoryLength:(NSUInteger)length atIndex:(NSUInteger)index {
    if (index >= RDNA4_ICB_MAX_TG_MEM_INDICES) {
        RDNA4LOG("compute setThreadgroupMemoryLength at index %lu exceeds limit %u", (unsigned long)index, RDNA4_ICB_MAX_TG_MEM_INDICES);
        return;
    }
    _tgMemLengths[index] = length;
    if (index == 0) _threadgroupBytes = length;
    _compStateDirty = YES;
}
- (void)setStageInRegion:(MTLRegion)region {
    _stageInRegion = region;
    _hasStageInRegion = YES;
}
- (void)rdna4ICBComputeBarrier {
    VkCommandBuffer cmd = [_cb vk];
    if (!cmd) return;
    VkMemoryBarrier mb = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_UNIFORM_READ_BIT,
    };
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &mb, 0, NULL, 0, NULL);
    _pendingSerialBarrier = NO;
}
- (void)memoryBarrierWithScope:(NSUInteger)s {
    if (garnet_comp_scope_needs_full_barrier((uint64_t)s)) {
        rdna4_full_barrier([_cb vk]);
        _pendingSerialBarrier = NO;
    } else {
        [self rdna4ICBComputeBarrier];
    }
}
- (void)memoryBarrierWithResources:(const id __unsafe_unretained *)r count:(NSUInteger)n {
    (void)r; (void)n;
    [self rdna4ICBComputeBarrier];
}
- (void)updateFence:(id)f {
    if (_ended) { [_cb rdna4Fail:@"compute updateFence after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    [_cb rdna4UpdateFence:f];
    _pendingSerialBarrier = NO;
}
- (void)waitForFence:(id)f {
    if (_ended) { [_cb rdna4Fail:@"compute waitForFence after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    [_cb rdna4WaitForFence:f];
    _pendingSerialBarrier = NO;
}
- (void)sampleCountersInBuffer:(id<MTLCounterSampleBuffer>)sampleBuffer
                 atSampleIndex:(NSUInteger)sampleIndex
                   withBarrier:(BOOL)barrier {
    if (_ended) { [_cb rdna4Fail:@"compute sampleCountersInBuffer after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    NSString *why = nil;
    id<RDNA4CounterSampleBufferInternal> csb = rdna4_counter_buffer_check(sampleBuffer, [_cb device], sampleIndex, YES, &why);
    if (!csb) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"compute sampleCountersInBuffer: %@", why ?: @"invalid"]];
        return;
    }
    [_cb rdna4EnsureGfxSegment];
    if ([_cb rdna4Error]) return;
    [csb rdna4SampleOnCommandBuffer:_cb vkCommandBuffer:[_cb vk] sampleIndex:sampleIndex barrier:barrier
                         stageFlags:(barrier ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT)
                                who:"compute"];
}
RDNA4_ENCODER_SHARED
- (void)pushDebugGroup:(NSString *)s { (void)s; }
- (void)popDebugGroup {}
- (void)insertDebugSignpost:(NSString *)s { (void)s; }

// grid: threads (dispatchThreads) or groups*tpt (dispatchThreadgroups). The Metal call's threadsPerThreadgroup must equal the local size the
// module was translated with (meta local_size); otherwise the command buffer completes with an NSError instead of dispatching.
- (void)rdna4Dispatch:(MTLSize)grid tpt:(MTLSize)tpt exactThreads:(BOOL)exact indirect:(RDNA4Buffer *)indirect offset:(NSUInteger)offset {
    if (_ended) { [_cb rdna4Fail:@"dispatch after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    if (!_pso) { [_cb rdna4Fail:@"dispatch without a compute pipeline state"]; return; }
    { RDNA4ComputePipelineState *r = [_pso rdna4Real];   // hot-swap (see the render encoder)
      if (r) {
          _pso = r;
          _st.scanPlan = [_pso scanPlan];
          _st.rtAsRefl = [_pso rdna4RtAsRefl];
          [self rdna4ResetComputeCache];
          [self rdna4OwnResource:r kind:RDNA4_RESOURCE_PIPELINE
                        fragment:NO index:0];
          RDNA4LOGR("HOT-SWAP: compute encoder now uses the real pipeline %p", (__bridge void *)r);
      } }
    { const uint64_t segSer = [_cb rdna4SegmentSerial];
      if (segSer != _compSegSerial) {
          _compSegSerial = segSer;
          _pendingSerialBarrier = NO;
          [self rdna4ResetComputeCache];
      } }
    MTLComputePipelineReflection *reflection = nil;
    [self rdna4SnapshotResources];
    if (_st.roBufMask && rdna4nc_any_readonly_buffer()) {
        reflection = [_pso reflection] ?: _roReflection;
        if (rdna4nc_stage_writes(&_st, rdna4nc_stage_arguments(reflection, RDNA4NC_COMPUTE), RDNA4NC_COMPUTE, "compute dispatch", &_roWarn)) {
            [_cb rdna4Fail:@"compute dispatch writes a read-only no-copy buffer"];
            return;
        }
    }
    if ([_pso noop]) { RDNA4LOGR("COMPUTE FALLBACK: dispatch %lux%lux%lu (tpt %lux%lux%lu) skipped, placeholder pipeline encodes nothing", (unsigned long)grid.width, (unsigned long)grid.height, (unsigned long)grid.depth, (unsigned long)tpt.width, (unsigned long)tpt.height, (unsigned long)tpt.depth); return; }
    const NSUInteger threadgroupBytes = [_pso rdna4LumaWorkgroup] ? _threadgroupBytes : 0;
    if (_threadgroupBytes && !threadgroupBytes) RDNA4LOGR("compute dispatch uses fixed threadgroup storage (requested %lu bytes at index 0)", (unsigned long)_threadgroupBytes);
    if (threadgroupBytes && !rdna4_luma_workgroup_bytes_ok(threadgroupBytes, rdna4_device_caps().shared_memory)) {
        [_cb rdna4Fail:@"luma threadgroup memory length is not a whole number of float4 elements or exceeds the device limit"];
        return;
    }
    uint32_t nom[3] = { [_pso nominalLocal][0], [_pso nominalLocal][1], [_pso nominalLocal][2] };
    if ([_pso mode] == 1) {
        // #12 R2: a ThreadsDynamic module declares its local size through spec ids 0..2 (checked: every kernel in spvcache), so the
        // dispatch's own threadsPerThreadgroup is the nominal local size; a pipeline variant per (x,y,z) is built lazily and cached.
        rdna4dc_caps caps = rdna4_device_caps();
        if (!rdna4dc_workgroup_ok(&caps, tpt.width, tpt.height, tpt.depth)) {
            [_cb rdna4Fail:[NSString stringWithFormat:@"threadsPerThreadgroup %lux%lux%lu is zero or outside the RADV limits (per axis %u,%u,%u; invocations %u)", (unsigned long)tpt.width, (unsigned long)tpt.height, (unsigned long)tpt.depth,
                          caps.workgroup_size[0], caps.workgroup_size[1], caps.workgroup_size[2], caps.invocations]]; return; }
        if (tpt.width != nom[0] || tpt.height != nom[1] || tpt.depth != nom[2])
            RDNA4LOGR("compute dispatch: threadsPerThreadgroup %lux%lux%lu overrides the translated local size %ux%ux%u (spec ids 0..2)", (unsigned long)tpt.width, (unsigned long)tpt.height, (unsigned long)tpt.depth, nom[0], nom[1], nom[2]);
        nom[0] = (uint32_t)tpt.width; nom[1] = (uint32_t)tpt.height; nom[2] = (uint32_t)tpt.depth;
    } else if (tpt.width != nom[0] || tpt.height != nom[1] || tpt.depth != nom[2]) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"threadsPerThreadgroup %lux%lux%lu != the SPIR-V local size %ux%ux%u the kernel was translated with (metal2vulkan --local; this module is Workgroups, local size is not specialisable)",
                      (unsigned long)tpt.width, (unsigned long)tpt.height, (unsigned long)tpt.depth, nom[0], nom[1], nom[2]]]; return; }
    if (threadgroupBytes && !rdna4_luma_workgroup_fits(threadgroupBytes, nom)) {
        [_cb rdna4Fail:@"luma threadgroup memory requires one float4 per invocation"];
        return;
    }
    uint32_t g[3] = { (uint32_t)grid.width, (uint32_t)grid.height, (uint32_t)grid.depth };
    if (!indirect && (!g[0] || !g[1] || !g[2])) return;
    id dev = [_cb device];
    const RDNA4ArgumentTables *argTables = [_pso argumentTables];
    const BOOL needArgPrep =
        (argTables && argTables->active) ||
        [_pso rdna4HasAddressRegistry] ||
        (_st.scanPlan && _st.scanPlan->plan.count) ||
        _argumentResources.count != 0 ||
        _argumentWritable.count != 0 ||
        _st.rt.iftMask != 0 ||
        [_cb rdna4HasResidency] ||
        rdna4_rt_any_arg_owners();
    if ([_cb rdna4CurrentSegmentIsCompute]) {
        __block BOOL hasIncompatTex = NO;
        if (_compStateDirty || needArgPrep) {
            rdna4_each_tex([_pso pb], [_pso npb], &_st, dev, ^(RDNA4Texture *t, VkImageLayout l, BOOL writable) {
                (void)l; (void)writable;
                if (hasIncompatTex) return;
                const int isIos = ([t rdna4IsIOS] || [t rdna4IsDisp]) ? 1 : 0;
                const int isDs = ([t rdna4Aspects] & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) ? 1 : 0;
                const VkImageLayout curL = [t layout];
                const int inAtt = (curL == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL ||
                                   curL == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) ? 1 : 0;
                if (!garnet_async_compute_texture_ok(isIos, isDs, inAtt)) hasIncompatTex = YES;
            });
        }
        const int hasIcbOrRt = (rdna4_icb_any_arg_owners() || rdna4_rt_any_arg_owners()) ? 1 : 0;
        if (garnet_async_compute_needs_gfx_fallback(1, needArgPrep ? 1 : 0, hasIcbOrRt, hasIncompatTex ? 1 : 0)) {
            [_cb rdna4EnsureGfxSegment];
            if ([_cb rdna4Error]) return;
            _compSegSerial = [_cb rdna4SegmentSerial];
            _pendingSerialBarrier = NO;
            [self rdna4ResetComputeCache];
        }
    }
    NSMutableSet *argumentResources = nil;
    if (needArgPrep) {
        if (_pendingSerialBarrier) {
            rdna4_full_barrier([_cb vk]);
            _pendingSerialBarrier = NO;
        }
        if (!reflection) reflection = [_pso reflection] ?: _roReflection;
        argumentResources = [_argumentResources mutableCopy] ?: [NSMutableSet set];
        NSMutableSet *argumentWritable = [_argumentWritable mutableCopy] ?: [NSMutableSet set];
        rdna4_argument_stage_resources(argumentResources, argumentWritable, [_pso pb], [_pso npb], &_st, rdna4nc_stage_arguments(reflection, RDNA4NC_COMPUTE));
        RDNA4ArgumentScanPrepared scan = {0};
        const RDNA4StageState *scanStages[] = {&_st};
        NSString *scanError = nil;
        if (!rdna4_argument_scan_prepare(&scan, scanStages, 1, argumentWritable, dev,
                                         &scanError)) {
            [_cb rdna4Fail:scanError ?: @"argument scan preparation failed"];
            return;
        }
        BOOL argumentsReady = [_cb rdna4PrepareArguments:argTables
            resources:argumentResources writable:argumentWritable scan:&scan
            textures:^(RDNA4Texture *t, VkImageLayout layout, BOOL writable) {
            rdna4_ios_usek(self->_cb, t, YES, writable, RDNA4DF_W_COMPUTE);
            rdna4_tex_to([self->_cb vk], t, layout);
        }];
        rdna4_argument_scan_prepared_destroy(&scan);
        if (!argumentsReady) return;
    }
    if (_compStateDirty || needArgPrep) {
        const BOOL compSeg = [_cb rdna4CurrentSegmentIsCompute];
        rdna4_each_tex([_pso pb], [_pso npb], &_st, dev, ^(RDNA4Texture *t, VkImageLayout l, BOOL writable) {
            rdna4_ios_usek(self->_cb, t, YES, writable, RDNA4DF_W_COMPUTE);
            if (compSeg) rdna4_tex_to_compute([self->_cb vk], t, l);
            else rdna4_tex_to([self->_cb vk], t, l);
        });
    }
    if (rdna4_icb_any_arg_owners() || rdna4_rt_any_arg_owners()) {
        NSMutableSet *icbRefs = [NSMutableSet set];
        NSMutableSet *icbPsoRefs = [NSMutableSet set];
        for (uint32_t i = 0; i < 31; i++) {
            if (_st.buf[i].b) {
                rdna4_argbuf_icb_snapshot(_st.buf[i].b, icbRefs, icbPsoRefs);
                rdna4_rt_retain_argbuf_resources(_st.buf[i].b, _cb);
            }
        }
        for (id res in _argumentResources) {
            if ([res isKindOfClass:[RDNA4Buffer class]])
                rdna4_argbuf_icb_snapshot((RDNA4Buffer *)res, icbRefs, icbPsoRefs);
        }
        for (id res in argumentResources) {
            if ([res isKindOfClass:[RDNA4Buffer class]])
                rdna4_rt_retain_argbuf_resources((RDNA4Buffer *)res, _cb);
        }
        for (id pso in icbPsoRefs) {
            [_cb rdna4RetainEncoded:pso];
        }
        for (id<RDNA4ICBInternal> icb in icbRefs) {
            if ([icb device] != dev) {
                [_cb rdna4Fail:@"compute dispatch references indirect command buffer from another device"];
                return;
            }
            if (![icb rdna4PrepareForGPUProducerOnCommandBuffer:_cb]) return;
            [_cb rdna4RetainEncoded:icb];
            id backing = [icb rdna4GPUBacking];
            if (backing) [_cb rdna4RetainEncoded:backing];
            for (id pso in icbPsoRefs) {
                if ([icb respondsToSelector:@selector(rdna4IsComputeICB)]) {
                    BOOL wantCompute = [(id<RDNA4ComputeICBInternal>)icb rdna4IsComputeICB];
                    BOOL isComputePso = [pso isKindOfClass:[RDNA4ComputePipelineState class]];
                    if (wantCompute != isComputePso && icbRefs.count > 1) continue;
                }
                if (![icb rdna4RegisterCandidatePipeline:pso]) {
                    [_cb rdna4Fail:@"indirect command buffer exceeds 32 candidate pipelines or pipeline type mismatch"];
                    return;
                }
            }
        }
    }
    VkCommandBuffer cmd = [_cb vk];
    const VkPipelineLayout layout = [_pso layout];
    const uint32_t npb = [_pso npb];
    const RDNA4PB *pb = [_pso pb];
    BOOL canCache = !needArgPrep && ![_pso argumentTables]->active && ![_pso rdna4HasAddressRegistry];
    if (canCache && _compStateDirty) {
        for (uint32_t i = 0; i < npb; i++) {
            if (pb[i].addressTable || pb[i].samplerCoordinateFlags || pb[i].embedded || pb[i].rtPrimDataTable) { canCache = NO; break; }
        }
    }
    const BOOL usePush = [_pso pushDesc];
    VkDescriptorSet s = VK_NULL_HANDLE;
    BOOL needBindSet = NO;
    if (npb) {
        if (usePush) {
            if (!(canCache && !_compStateDirty && _boundLayout == layout && _boundPushValid)) {
                if (![_cb rdna4FillOrPushSet:VK_NULL_HANDLE pushCmd:cmd pushLayout:layout pushSet:0
                                    bindings:pb count:npb state:&_st stage:"compute" fromRender:NO]) return;
                if (canCache) { _boundLayout = layout; _boundSet = VK_NULL_HANDLE; _boundPushValid = YES; }
                else { _boundLayout = VK_NULL_HANDLE; _boundSet = VK_NULL_HANDLE; _boundPushValid = NO; }
            }
        } else if (canCache && !_compStateDirty && _boundLayout == layout && _boundSet != VK_NULL_HANDLE) {
            s = _boundSet;
        } else {
            s = [_cb rdna4AllocSet:[_pso dsl]];
            if (!s || ![_cb rdna4FillSet:s bindings:pb count:npb state:&_st stage:"compute"]) return;
            needBindSet = YES;
            if (canCache) { _boundLayout = layout; _boundSet = s; _boundPushValid = NO; }
            else { _boundLayout = VK_NULL_HANDLE; _boundSet = VK_NULL_HANDLE; _boundPushValid = NO; }
        }
    }
    [_cb rdna4BindArguments:[_pso argumentTables] point:VK_PIPELINE_BIND_POINT_COMPUTE layout:layout];
    if (indirect || [_pso mode] == 0) {   // Whole workgroups, with CPU or GPU dispatch counts.
        if (exact && (g[0] % nom[0] || g[1] % nom[1] || g[2] % nom[2])) { [_cb rdna4Fail:@"dispatchThreads with a partial boundary threadgroup needs a ThreadsDynamic translation (this module is Workgroups)"]; return; }
        const BOOL isInd = indirect != nil;
        VkPipeline pipeline = VK_NULL_HANDLE;
        if (_boundPipe && _boundLocal[0] == nom[0] && _boundLocal[1] == nom[1] && _boundLocal[2] == nom[2] &&
            _boundIndirect == isInd && _boundTgBytes == threadgroupBytes) {
            pipeline = _boundPipe;
        } else {
            pipeline = [_pso pipelineForLocal:nom indirect:isInd threadgroupBytes:threadgroupBytes];
            if (!pipeline) { [_cb rdna4Fail:@"cannot build a compute pipeline variant"]; return; }
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
            _boundPipe = pipeline;
            _boundLocal[0] = nom[0]; _boundLocal[1] = nom[1]; _boundLocal[2] = nom[2];
            _boundIndirect = isInd; _boundTgBytes = threadgroupBytes;
            _boundPCValid = NO;
        }
        if (s && needBindSet) vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &s, 0, NULL);
        if (indirect) {
            rdna4_full_barrier(cmd);
            _pendingSerialBarrier = NO;
            vkCmdDispatchIndirect(cmd, [indirect vkBuffer], [indirect rdna4VkOffset] + offset);
        } else {
            if (garnet_comp_needs_pre_barrier((unsigned)_dispatchType, _pendingSerialBarrier ? 1 : 0, 0))
                [self rdna4ICBComputeBarrier];
            vkCmdDispatch(cmd, exact ? g[0] / nom[0] : g[0], exact ? g[1] / nom[1] : g[1], exact ? g[2] / nom[2] : g[2]);
        }
    } else {                  // ThreadsDynamic: the metal2vulkan region plan, one dispatch per region
        RDNA4Region reg[8]; uint32_t threads[3], tgpg[3];
        for (int d = 0; d < 3; d++) { threads[d] = exact ? g[d] : g[d] * nom[d]; tgpg[d] = (threads[d] + nom[d] - 1) / nom[d]; }
        int nr = rdna4_plan(threads, nom, reg);
        if (s && needBindSet) vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &s, 0, NULL);
        if (garnet_comp_needs_pre_barrier((unsigned)_dispatchType, _pendingSerialBarrier ? 1 : 0, 0))
            [self rdna4ICBComputeBarrier];
        for (int i = 0; i < nr; i++) {
            VkPipeline p = VK_NULL_HANDLE;
            if (_boundPipe && _boundLocal[0] == reg[i].local[0] && _boundLocal[1] == reg[i].local[1] && _boundLocal[2] == reg[i].local[2] &&
                !_boundIndirect && _boundTgBytes == threadgroupBytes) {
                p = _boundPipe;
            } else {
                p = [_pso pipelineForLocal:reg[i].local indirect:NO threadgroupBytes:threadgroupBytes];
                if (!p) { [_cb rdna4Fail:@"cannot build a compute pipeline variant"]; return; }
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p);
                _boundPipe = p;
                _boundLocal[0] = reg[i].local[0]; _boundLocal[1] = reg[i].local[1]; _boundLocal[2] = reg[i].local[2];
                _boundIndirect = NO; _boundTgBytes = threadgroupBytes;
                _boundPCValid = NO;
            }
            uint32_t words[12] = { threads[0], threads[1], threads[2], reg[i].tbase[0], reg[i].tbase[1], reg[i].tbase[2], reg[i].gbase[0], reg[i].gbase[1], reg[i].gbase[2], tgpg[0], tgpg[1], tgpg[2] };
            if (!_boundPCValid || memcmp(_boundPC, words, sizeof words) != 0) {
                vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, [_pso pcOffset], sizeof words, words);
                memcpy(_boundPC, words, sizeof words);
                _boundPCValid = YES;
            }
            vkCmdDispatch(cmd, reg[i].groups[0], reg[i].groups[1], reg[i].groups[2]);
        }
        RDNA4LOGR("compute dispatch: threads %u,%u,%u local %u,%u,%u -> %d region(s)", threads[0], threads[1], threads[2], nom[0], nom[1], nom[2], nr);
    }
    if (canCache) _compStateDirty = NO;
    _pendingSerialBarrier = (_dispatchType == GARNET_COMP_DISPATCH_SERIAL);
    [_cb rdna4ArgumentCheckpoint];
}
- (void)dispatchThreadgroups:(MTLSize)g threadsPerThreadgroup:(MTLSize)t { [self rdna4Dispatch:g tpt:t exactThreads:NO indirect:nil offset:0]; }
- (void)dispatchThreads:(MTLSize)g threadsPerThreadgroup:(MTLSize)t { [self rdna4Dispatch:g tpt:t exactThreads:YES indirect:nil offset:0]; }
- (void)dispatchThreadgroupsWithIndirectBuffer:(id)buffer indirectBufferOffset:(NSUInteger)offset threadsPerThreadgroup:(MTLSize)t {
    if (![buffer isKindOfClass:[RDNA4Buffer class]] || offset % 4 || offset > [buffer length] || sizeof(VkDispatchIndirectCommand) > [buffer length] - offset) {
        [_cb rdna4Fail:@"indirect compute argument buffer range or alignment is invalid"]; return;
    }
    if (!rdna4nc_vk_offset_ok([(RDNA4Buffer *)buffer rdna4VkOffset] + offset, 4, RDNA4NC_ALIGN_INDIRECT)) {
        [_cb rdna4Fail:@"indirect compute Vulkan offset must be a multiple of 4"]; return;
    }
    [_cb rdna4RetainEncoded:buffer];
    [self rdna4Dispatch:MTLSizeMake(0,0,0) tpt:t exactThreads:NO indirect:buffer offset:offset];
}
- (void)executeCommandsInBuffer:(id<MTLIndirectCommandBuffer>)indirectCommandBuffer withRange:(NSRange)executionRange {
    if (_ended || [_cb rdna4Error]) {
        [_cb rdna4Fail:@"compute executeCommandsInBuffer after endEncoding or after an earlier encode error"];
        return;
    }
    if (!indirectCommandBuffer || ![indirectCommandBuffer respondsToSelector:@selector(rdna4IsComputeICB)]) {
        [_cb rdna4Fail:@"compute executeCommandsInBuffer requires an RDNA4IndirectCommandBuffer"];
        return;
    }
    id<RDNA4ComputeICBInternal> icb = (id<RDNA4ComputeICBInternal>)indirectCommandBuffer;
    if (![icb rdna4IsComputeICB]) {
        [_cb rdna4Fail:@"compute executeCommandsInBuffer cannot execute a render indirect command buffer"];
        return;
    }
    [_cb rdna4EnsureGfxSegment];
    if ([_cb rdna4Error]) return;
    if ([icb rdna4UseGPUExecution]) {
        [self rdna4ResetComputeCache];
        _pendingSerialBarrier = NO;
        [icb rdna4ExecuteComputeGPUOnEncoder:self
                                 directRange:executionRange
                              indirectBuffer:nil
                              indirectOffset:0
                                 useIndirect:NO];
        return;
    }
    NSString *problem = nil;
    RDNA4ComputePipelineState *parentPipeline = _pso;
    MTLComputePipelineReflection *parentReflection = _roReflection;
    RDNA4StageState parentState = _st;
    NSUInteger parentTgBytes = _threadgroupBytes;
    NSUInteger parentTgLengths[RDNA4_ICB_MAX_TG_MEM_INDICES];
    memcpy(parentTgLengths, _tgMemLengths, sizeof parentTgLengths);
    MTLRegion parentStageIn = _stageInRegion;
    BOOL parentHasStageIn = _hasStageInRegion;
    [_cb rdna4RetainEncoded:icb];
    NSUInteger replayed = [icb rdna4ExecuteComputeOnEncoder:self range:executionRange reason:&problem];
    _pso = parentPipeline;
    _roReflection = parentReflection;
    _st = parentState;
    [self rdna4OwnResource:_pso kind:RDNA4_RESOURCE_PIPELINE fragment:NO index:0];
    for (NSUInteger i = 0; i < 31; ++i) {
        [self rdna4OwnResource:_st.buf[i].b kind:RDNA4_RESOURCE_BUFFER fragment:NO index:i];
        [self rdna4OwnResource:_st.rt.as[i] kind:RDNA4_RESOURCE_ACCEL fragment:NO index:i];
    }
    _threadgroupBytes = parentTgBytes;
    memcpy(_tgMemLengths, parentTgLengths, sizeof _tgMemLengths);
    _stageInRegion = parentStageIn;
    _hasStageInRegion = parentHasStageIn;
    [self rdna4ResetComputeCache];
    if (problem) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"compute executeCommandsInBuffer [%lu,+%lu): %@",
                        (unsigned long)executionRange.location, (unsigned long)executionRange.length, problem]];
        return;
    }
    RDNA4LOGR("compute executeCommandsInBuffer: replayed %lu command(s)", (unsigned long)replayed);
}
- (void)executeCommandsInBuffer:(id<MTLIndirectCommandBuffer>)indirectCommandBuffer
                 indirectBuffer:(id<MTLBuffer>)indirectRangeBuffer
           indirectBufferOffset:(NSUInteger)indirectBufferOffset {
    if (_ended || [_cb rdna4Error]) {
        [_cb rdna4Fail:@"compute executeCommandsInBuffer:indirectBuffer: after endEncoding or after an earlier encode error"];
        return;
    }
    if (!indirectCommandBuffer || ![indirectCommandBuffer respondsToSelector:@selector(rdna4IsComputeICB)]) {
        [_cb rdna4Fail:@"compute executeCommandsInBuffer:indirectBuffer: requires an RDNA4IndirectCommandBuffer"];
        return;
    }
    id<RDNA4ComputeICBInternal> icb = (id<RDNA4ComputeICBInternal>)indirectCommandBuffer;
    if (![icb rdna4IsComputeICB]) {
        [_cb rdna4Fail:@"compute executeCommandsInBuffer:indirectBuffer: cannot execute a render indirect command buffer"];
        return;
    }
    [_cb rdna4EnsureGfxSegment];
    if ([_cb rdna4Error]) return;
    [self rdna4ResetComputeCache];
    _pendingSerialBarrier = NO;
    [icb rdna4ExecuteComputeGPUOnEncoder:self
                             directRange:NSMakeRange(0, 0)
                          indirectBuffer:indirectRangeBuffer
                          indirectOffset:indirectBufferOffset
                             useIndirect:YES];
}
- (void)endEncoding {
    if (_ended) return;
    _ended = YES;
    rdna4_full_barrier([_cb vk]);
    _pendingSerialBarrier = NO;
    if (_nCsbEnd) {
        BOOL cbOk = ![_cb rdna4Error];
        for (uint32_t k = 0; k < _nCsbEnd; k++) {
            if (cbOk && _csbEnd[k] && _csbEndIdx[k] != MTLCounterDontSample)
                [_csbEnd[k] rdna4SampleOnCommandBuffer:_cb vkCommandBuffer:[_cb vk]
                                           sampleIndex:_csbEndIdx[k] barrier:NO
                                            stageFlags:VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT
                                                   who:"compute-pass-end"];
            _csbEnd[k] = nil;
        }
        _nCsbEnd = 0u;
    }
    [_cb rdna4ArgumentCheckpoint];
    RDNA4LOGR("RDNA4ComputeEncoder endEncoding");
    [super endEncoding];
}
@end


// m11h9: one buffer<->image copy region for a level / slice (2D, 1D arrays) or a box (3D: baseArrayLayer 0, depth in the extent, bufferImageHeight from bytesPerImage).
// Vulkan specifies bufferRowLength and bufferImageHeight in TEXELS (for compressed formats, multiply block counts by the 4x4 block dimension).
static VkBufferImageCopy rdna4_bic(RDNA4Texture *t, NSUInteger off, NSUInteger bpr, NSUInteger bpi, NSUInteger level, NSUInteger slice, MTLOrigin o, MTLSize sz) {
    BOOL is3 = [t textureType] == MTLTextureType3D; uint32_t bpp = [t bytesPerPixel];
    uint32_t rowTexels = bpr && bpp ? (uint32_t)((bpr / bpp) * ([t isCompressed] ? 4u : 1u)) : 0u;
    uint32_t imgHeight = (is3 && sz.depth > 1 && bpr) ? (uint32_t)((bpi / bpr) * ([t isCompressed] ? 4u : 1u)) : 0u;
    return (VkBufferImageCopy){ .bufferOffset = off, .bufferRowLength = rowTexels, .bufferImageHeight = imgHeight,
        .imageSubresource = { [t rdna4CopyAspect], [t rdna4MipLevel:level], [t rdna4Slice:slice], 1 },   // bundle 10: colour, or the one depth / stencil aspect
        .imageOffset = { (int32_t)o.x, (int32_t)o.y, is3 ? (int32_t)o.z : 0 }, .imageExtent = { (uint32_t)sz.width, (uint32_t)sz.height, is3 ? (uint32_t)sz.depth : 1 } };
}
static BOOL rdna4_blit_buffer_layout(RDNA4Texture *texture, RDNA4Buffer *buffer,
    MTLSize size, NSUInteger offset, NSUInteger *rowStride, NSUInteger *imageStride,
    VkDeviceSize *vkOffset) {
    NSUInteger rowBytes, minimumImage;
    NSUInteger bpp = [texture bytesPerPixel];
    BOOL bc = [texture isCompressed];
    NSUInteger bw = bc ? (size.width + 3) / 4 : size.width;
    NSUInteger bh = bc ? (size.height + 3) / 4 : size.height;
    if (!bpp || __builtin_mul_overflow(bw, bpp, &rowBytes)) return NO;
    NSUInteger row = *rowStride ?: rowBytes;
    NSUInteger depth = [texture textureType] == MTLTextureType3D ? size.depth : 1;
    NSUInteger image = 0;
    if (depth > 1) {
        if (!row || __builtin_mul_overflow(row, bh, &minimumImage)) return NO;
        image = *imageStride ?: minimumImage;
        if (image % row || (image / row) * (bc ? 4 : 1) > UINT32_MAX) return NO;
    }
    rdna4_texture_transfer transfer;
    if (row % bpp || (row / bpp) * (bc ? 4 : 1) > UINT32_MAX ||
        !rdna4_texture_transfer_layout(bw, bh, depth, bpp, row, image, &transfer) ||
        !rdna4_buffer_range(buffer.length, offset, transfer.span)) return NO;
    VkDeviceSize relocated;
    if (__builtin_add_overflow([buffer rdna4VkOffset], (VkDeviceSize)offset, &relocated) ||
        !rdna4nc_vk_offset_ok(relocated, [texture rdna4CopyAspect] == VK_IMAGE_ASPECT_COLOR_BIT ? bpp : 4,
            RDNA4NC_ALIGN_IMAGE)) return NO;
    *rowStride = row; *imageStride = image; *vkOffset = relocated;
    return YES;
}
// Exact buffer rows touched by VkCmdCopyImageToBuffer. Keeping rows separate avoids
// retiring sampler owners that live in destinationBytesPerRow gaps.
static NSArray<NSValue *> *rdna4_blit_texture_buffer_ranges(RDNA4Texture *texture, MTLSize size,
        NSUInteger offset, NSUInteger bytesPerRow, NSUInteger bytesPerImage,
        NSUInteger bufferLength, NSRange *envelope) {
    if (!envelope || !texture || !size.width || !size.height) return @[];
    BOOL is3 = [texture textureType] == MTLTextureType3D;
    NSUInteger depth = is3 ? size.depth : 1;
    if (!depth) return @[];
    if (offset > bufferLength) return nil;
    BOOL bc = [texture isCompressed];
    NSUInteger bw = bc ? (size.width + 3) / 4 : size.width;
    NSUInteger bh = bc ? (size.height + 3) / 4 : size.height;
    NSUInteger pixelBytes = [texture bytesPerPixel], rowBytes = 0, minimumImageBytes = 0;
    if (!pixelBytes || __builtin_mul_overflow(bw, pixelBytes, &rowBytes)) return nil;
    NSUInteger rowStride = bytesPerRow ? bytesPerRow : rowBytes;
    if (rowStride < rowBytes || __builtin_mul_overflow(rowStride, bh,
                                                       &minimumImageBytes)) return nil;
    NSUInteger imageStride = depth > 1 && bytesPerImage ? bytesPerImage : minimumImageBytes;
    if (imageStride < minimumImageBytes) return nil;

    NSMutableArray<NSValue *> *ranges = [NSMutableArray array];
    NSUInteger first = NSUIntegerMax, last = 0;
    for (NSUInteger z = 0; z < depth; ++z) {
        NSUInteger imageOffset = 0;
        if (__builtin_mul_overflow(z, imageStride, &imageOffset) ||
            imageOffset > NSUIntegerMax - offset) return nil;
        for (NSUInteger y = 0; y < bh; ++y) {
            NSUInteger rowOffset = 0, start = 0, end = 0;
            if (__builtin_mul_overflow(y, rowStride, &rowOffset) ||
                rowOffset > NSUIntegerMax - imageOffset - offset) return nil;
            start = offset + imageOffset + rowOffset;
            if (start > bufferLength || rowBytes > bufferLength - start ||
                __builtin_add_overflow(start, rowBytes, &end)) return nil;
            if (first == NSUIntegerMax) first = start;
            if (ranges.count && NSMaxRange(ranges.lastObject.rangeValue) == start) {
                NSRange merged = NSMakeRange(ranges.lastObject.rangeValue.location,
                    start + rowBytes - ranges.lastObject.rangeValue.location);
                ranges[ranges.count - 1] = [NSValue valueWithRange:merged];
            } else {
                [ranges addObject:[NSValue valueWithRange:NSMakeRange(start, rowBytes)]];
            }
            last = end;
        }
    }
    if (!ranges.count) return ranges;
    *envelope = NSMakeRange(first, last - first);
    return ranges;
}
static BOOL rdna4_blit_region_ok(RDNA4Texture *t, MTLOrigin o, MTLSize sz, NSUInteger level, NSUInteger slice, const char *what) {
    return rdna4_region_ok(t, (MTLRegion){ o, sz }, level, slice, what);
}
static void rdna4_blit_note_overwrite(RDNA4CommandBuffer *cb, RDNA4Texture *texture,
        MTLOrigin origin, MTLSize size, NSUInteger level, NSUInteger slice) {
    if (level || slice || origin.x || origin.y || origin.z || size.depth != 1 ||
        [texture textureType] != MTLTextureType2D || [texture mipmapLevelCount] != 1 ||
        [texture rdna4Aspects] != VK_IMAGE_ASPECT_COLOR_BIT) return;
    [cb rdna4NoteSurfaceOverwrite:texture width:(uint32_t)size.width height:(uint32_t)size.height level:0];
}
@implementation RDNA4BlitEncoder
- (instancetype)initWithCommandBuffer:(id)cb blit:(int)dummy {
    (void)dummy;
    self = [super initWithCommandBuffer:cb];
    if (!self) return nil;
    _cb = cb;
    RDNA4LOGR("RDNA4BlitEncoder %p", (__bridge void *)self);
    return self;
}
- (instancetype)initWithCommandBuffer:(id)cb descriptor:(MTLBlitPassDescriptor *)d {
    self = [self initWithCommandBuffer:cb blit:0];
    if (!self || !d) return self;
    if ([d respondsToSelector:@selector(sampleBufferAttachments)]) {
        MTLBlitPassSampleBufferAttachmentDescriptorArray *sbas = d.sampleBufferAttachments;
        for (NSUInteger k = 0; sbas && k < RDNA4CNT_MAX_PASS_ATTACHMENTS; k++) {
            MTLBlitPassSampleBufferAttachmentDescriptor *att = sbas[k];
            if (!att || !att.sampleBuffer) continue;
            NSString *cwhy = nil;
            id<RDNA4CounterSampleBufferInternal> csb = rdna4_counter_buffer_check(att.sampleBuffer, [_cb device], 0, NO, &cwhy);
            if (!csb || !rdna4_counter_pass_index_valid(csb, att.startOfEncoderSampleIndex) ||
                !rdna4_counter_pass_index_valid(csb, att.endOfEncoderSampleIndex)) {
                RDNA4LOG_WARN("blit pass: sampleBufferAttachments[%lu] invalid (%s); refused",
                              (unsigned long)k, cwhy ? cwhy.UTF8String : "sample index out of range");
                _ended = YES;
                [super endEncoding];
                return nil;
            }
        }
        for (NSUInteger k = 0; sbas && k < RDNA4CNT_MAX_PASS_ATTACHMENTS; k++) {
            MTLBlitPassSampleBufferAttachmentDescriptor *att = sbas[k];
            if (!att || !att.sampleBuffer) continue;
            id<RDNA4CounterSampleBufferInternal> csb = (id<RDNA4CounterSampleBufferInternal>)att.sampleBuffer;
            if (att.startOfEncoderSampleIndex != MTLCounterDontSample)
                [csb rdna4SampleOnCommandBuffer:_cb vkCommandBuffer:[_cb vk]
                                    sampleIndex:att.startOfEncoderSampleIndex barrier:NO
                                     stageFlags:VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT who:"blit-pass-start"];
            if (att.endOfEncoderSampleIndex != MTLCounterDontSample && _nCsbEnd < RDNA4CNT_MAX_PASS_ATTACHMENTS) {
                _csbEnd[_nCsbEnd] = csb;
                _csbEndIdx[_nCsbEnd] = att.endOfEncoderSampleIndex;
                _nCsbEnd++;
            }
        }
    }
    return self;
}
RDNA4_DNR(RDNA4BlitEncoder)
- (NSString *)label { return _lbl; }
- (void)setLabel:(NSString *)l { _lbl = [l copy]; }
- (NSUInteger)getType { return 3; }
- (void)copyFromTexture:(id)src sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl sourceOrigin:(MTLOrigin)so sourceSize:(MTLSize)sz
               toBuffer:(id)dst destinationOffset:(NSUInteger)doff destinationBytesPerRow:(NSUInteger)bpr destinationBytesPerImage:(NSUInteger)bpi {
    if (_ended) { [_cb rdna4Fail:@"texture-to-buffer copy after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    RDNA4LOGR("blit copyFromTexture toBuffer: slice %lu level %lu origin %lu,%lu,%lu size %lux%lux%lu offset %lu bpr %lu bpi %lu", (unsigned long)ss, (unsigned long)sl,
           (unsigned long)so.x, (unsigned long)so.y, (unsigned long)so.z, (unsigned long)sz.width, (unsigned long)sz.height, (unsigned long)sz.depth, (unsigned long)doff, (unsigned long)bpr, (unsigned long)bpi);
    if (![src isKindOfClass:[RDNA4Texture class]] || ![dst isKindOfClass:[RDNA4Buffer class]]) { [_cb rdna4Fail:@"blit copyFromTexture:toBuffer: bad classes"]; return; }
    if (rdna4nc_drop_write(dst, RDNA4NC_TEXTURE_BUFFER, "copyFromTexture:toBuffer:", "BlitCommandEncoder")) { [_cb rdna4Fail:@"copyFromTexture:toBuffer: destination is read-only"]; return; }
    RDNA4Texture *t = src; RDNA4Buffer *b = dst;
    [_cb rdna4RetainArgument:t]; [_cb rdna4RetainArgument:b];
    if (![t rdna4CopyAspect] || [t rdna4Samples] > 1) { [_cb rdna4Fail:@"blit copyFromTexture:toBuffer: a combined depth/stencil texture or a multisample texture cannot be copied to a buffer here"]; return; }   // bundle 10
    if (!rdna4_blit_region_ok(t, so, sz, sl, ss, "blit copyFromTexture:toBuffer:")) { [_cb rdna4Fail:@"blit copyFromTexture:toBuffer: region outside the texture level"]; return; }
    VkDeviceSize vkoff;
    if (!rdna4_blit_buffer_layout(t, b, sz, doff, &bpr, &bpi, &vkoff)) {
        [_cb rdna4Fail:@"blit copyFromTexture:toBuffer: invalid buffer range, stride or Vulkan offset"]; return;
    }
    if ([t rdna4IsTexel]) {
        [self copyFromBuffer:[t rdna4TexelBuffer] sourceOffset:[t bufferOffset] + so.x * [t bytesPerPixel]
            toBuffer:b destinationOffset:doff size:sz.width * [t bytesPerPixel]];
        return;
    }
    NSRange argumentRange = NSMakeRange(0, 0);
    NSArray<NSValue *> *argumentRanges = rdna4_blit_texture_buffer_ranges(t, sz, doff, bpr, bpi,
                                                                           [b length], &argumentRange);
    if (!argumentRanges) {
        [_cb rdna4Fail:@"blit copyFromTexture:toBuffer: buffer layout overflows or exceeds the destination"];
        return;
    }
    if (argumentRanges.count) {
        // Texture contents are opaque to CPU argument scanning. A mapped
        // destination is refined from its final bytes; an unmapped destination
        // must conservatively retain the owners it had before this copy.
        RDNA4ArgumentWriteIntent *intent =
            rdna4_argument_sampler_intent_ranges(b, argumentRange, argumentRanges, nil, NO);
        if (!intent) {
            [_cb rdna4Fail:@"blit copyFromTexture:toBuffer: argument-write intent is invalid"];
            return;
        }
        [_cb rdna4ArgumentIntent:intent];
    }
    rdna4_ios_use(_cb, t, YES, NO);
    VkCommandBuffer cmd = [_cb vk];
    rdna4_tex_to(cmd, t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy bic = rdna4_bic(t, vkoff, bpr, bpi, sl, ss, so, sz);
    vkCmdCopyImageToBuffer(cmd, [t vkImage], rdna4_use_layout(t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL), [b vkBuffer], 1, &bic);
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                           .dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}
- (void)copyFromBuffer:(id)src sourceOffset:(NSUInteger)so sourceBytesPerRow:(NSUInteger)bpr sourceBytesPerImage:(NSUInteger)bpi sourceSize:(MTLSize)sz
             toTexture:(id)dst destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl destinationOrigin:(MTLOrigin)dorg {
    if (_ended) { [_cb rdna4Fail:@"buffer-to-texture copy after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    if (![src isKindOfClass:[RDNA4Buffer class]] || ![dst isKindOfClass:[RDNA4Texture class]]) { [_cb rdna4Fail:@"blit copyFromBuffer:toTexture: bad classes"]; return; }
    if (rdna4nc_drop_write(dst, RDNA4NC_BUFFER_TEXTURE, "copyFromBuffer:toTexture:", "BlitCommandEncoder")) { [_cb rdna4Fail:@"copyFromBuffer:toTexture: destination is read-only"]; return; }
    RDNA4Texture *t = dst; [_cb rdna4RetainArgument:src]; [_cb rdna4RetainArgument:t];
    if (![t rdna4CopyAspect] || [t rdna4Samples] > 1) { [_cb rdna4Fail:@"blit copyFromBuffer:toTexture: a combined depth/stencil texture or a multisample texture cannot be filled from a buffer here"]; return; }   // bundle 10
    if (!rdna4_blit_region_ok(t, dorg, sz, dl, ds, "blit copyFromBuffer:toTexture:")) { [_cb rdna4Fail:@"blit copyFromBuffer:toTexture: region outside the texture level"]; return; }
    VkDeviceSize vkoff;
    if (!rdna4_blit_buffer_layout(t, src, sz, so, &bpr, &bpi, &vkoff)) {
        [_cb rdna4Fail:@"blit copyFromBuffer:toTexture: invalid buffer range, stride or Vulkan offset"]; return;
    }
    if ([t rdna4IsTexel]) {
        [self copyFromBuffer:src sourceOffset:so toBuffer:[t rdna4TexelBuffer]
            destinationOffset:[t bufferOffset] + dorg.x * [t bytesPerPixel] size:sz.width * [t bytesPerPixel]];
        return;
    }
    if (![(RDNA4Buffer *)src rdna4Imported]) rdna4_blit_note_overwrite(_cb, t, dorg, sz, dl, ds);
    rdna4_ios_usek(_cb, t, YES, YES, RDNA4DF_W_BLIT);
    VkCommandBuffer cmd = [_cb vk];
    rdna4_tex_to(cmd, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy bic = rdna4_bic(t, vkoff, bpr, bpi, dl, ds, dorg, sz);
    vkCmdCopyBufferToImage(cmd, [(RDNA4Buffer *)src vkBuffer], [t vkImage], rdna4_use_layout(t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL), 1, &bic);
    rdna4_full_barrier(cmd);
}
- (void)copyFromTexture:(id)src sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl sourceOrigin:(MTLOrigin)so sourceSize:(MTLSize)sz
              toTexture:(id)dst destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl destinationOrigin:(MTLOrigin)dorg {
    if (_ended) { [_cb rdna4Fail:@"texture copy after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    if (![src isKindOfClass:[RDNA4Texture class]] || ![dst isKindOfClass:[RDNA4Texture class]]) { [_cb rdna4Fail:@"blit copyFromTexture:toTexture: bad classes"]; return; }
    if (rdna4nc_drop_write(dst, RDNA4NC_COPY_TEXTURE, "copyFromTexture:toTexture:", "BlitCommandEncoder")) { [_cb rdna4Fail:@"copyFromTexture:toTexture: destination is read-only"]; return; }
    RDNA4Texture *a = src, *b = dst; [_cb rdna4RetainArgument:a]; [_cb rdna4RetainArgument:b];
    if ([a rdna4Aspects] != [b rdna4Aspects] || [a rdna4Samples] != [b rdna4Samples]) { [_cb rdna4Fail:@"blit copyFromTexture:toTexture: the textures differ in aspects (colour / depth / stencil) or sample count"]; return; }   // bundle 10
    if (!rdna4_blit_region_ok(a, so, sz, sl, ss, "blit copyFromTexture:toTexture: (source)") || !rdna4_blit_region_ok(b, dorg, sz, dl, ds, "blit copyFromTexture:toTexture: (destination)")) {
        [_cb rdna4Fail:@"blit copyFromTexture:toTexture: region outside a texture level"]; return; }
    if ([a rdna4IsTexel] || [b rdna4IsTexel]) {
        if (a.pixelFormat != b.pixelFormat) { [_cb rdna4Fail:@"texture-buffer copy requires identical pixel formats"]; return; }
        NSUInteger row = sz.width * [a bytesPerPixel];
        if ([a rdna4IsTexel]) {
            [self copyFromBuffer:[a rdna4TexelBuffer] sourceOffset:[a bufferOffset] + so.x * [a bytesPerPixel]
                sourceBytesPerRow:row sourceBytesPerImage:0 sourceSize:sz toTexture:b
                destinationSlice:ds destinationLevel:dl destinationOrigin:dorg];
        } else {
            [self copyFromTexture:a sourceSlice:ss sourceLevel:sl sourceOrigin:so sourceSize:sz
                toBuffer:[b rdna4TexelBuffer] destinationOffset:[b bufferOffset] + dorg.x * [b bytesPerPixel]
                destinationBytesPerRow:row destinationBytesPerImage:0];
        }
        return;
    }
    BOOL sameImage = [a vkImage] == [b vkImage];
    uint32_t sourceMip = [a rdna4MipLevel:sl], destinationMip = [b rdna4MipLevel:dl];
    uint32_t sourceSlice = [a rdna4Slice:ss], destinationSlice = [b rdna4Slice:ds];
    if (sameImage && sourceMip == destinationMip && sourceSlice == destinationSlice && so.x < dorg.x + sz.width && dorg.x < so.x + sz.width &&
        so.y < dorg.y + sz.height && dorg.y < so.y + sz.height && so.z < dorg.z + sz.depth && dorg.z < so.z + sz.depth) {
        [_cb rdna4Fail:@"blit copyFromTexture:toTexture: source and destination overlap"]; return;
    }
    rdna4_ios_use(_cb, a, YES, NO);
    IOSurfaceRef sourceSurface = [a rdna4UCSurface], destinationSurface = [b rdna4UCSurface];
    if (!sameImage && destinationSurface && (sourceSurface ? IOSurfaceGetID(sourceSurface) != IOSurfaceGetID(destinationSurface)
                                                          : ![a rdna4IsIOS]))
        rdna4_blit_note_overwrite(_cb, b, dorg, sz, dl, ds);
    rdna4_ios_usek(_cb, b, YES, YES, RDNA4DF_W_BLIT);
    VkCommandBuffer cmd = [_cb vk];
    BOOL a3 = [a textureType] == MTLTextureType3D, b3 = [b textureType] == MTLTextureType3D;
    VkImageLayout sourceLayout = rdna4_use_layout(a, sameImage ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkImageLayout destinationLayout = rdna4_use_layout(b, sameImage ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    rdna4_tex_to(cmd, a, sourceLayout);
    rdna4_tex_to(cmd, b, destinationLayout);
    VkImageCopy ic = { .srcSubresource = { [a rdna4Aspects], sourceMip, sourceSlice, 1 }, .srcOffset = { (int32_t)so.x, (int32_t)so.y, a3 ? (int32_t)so.z : 0 },
        .dstSubresource = { [b rdna4Aspects], destinationMip, destinationSlice, 1 }, .dstOffset = { (int32_t)dorg.x, (int32_t)dorg.y, b3 ? (int32_t)dorg.z : 0 },
        .extent = { (uint32_t)sz.width, (uint32_t)sz.height, (a3 || b3) ? (uint32_t)sz.depth : 1 } };
    vkCmdCopyImage(cmd, [a vkImage], sourceLayout, [b vkImage], destinationLayout, 1, &ic);
    rdna4_full_barrier(cmd);
}
- (void)copyFromBuffer:(id)src sourceOffset:(NSUInteger)so toBuffer:(id)dst destinationOffset:(NSUInteger)doff size:(NSUInteger)size {
    if (_ended) { [_cb rdna4Fail:@"buffer copy after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    if (![src isKindOfClass:[RDNA4Buffer class]] || ![dst isKindOfClass:[RDNA4Buffer class]]) { [_cb rdna4Fail:@"blit copyFromBuffer:toBuffer: bad classes"]; return; }
    if (rdna4nc_drop_write(dst, RDNA4NC_COPY_BUFFER, "copyFromBuffer:toBuffer:", "BlitCommandEncoder")) { [_cb rdna4Fail:@"copyFromBuffer:toBuffer: destination is read-only"]; return; }
    RDNA4Buffer *a = src, *b = dst;
    if (so > a.length || size > a.length - so || doff > b.length || size > b.length - doff || ((so | doff | size) & 3)) {
        [_cb rdna4Fail:@"blit buffer copy range or macOS alignment is invalid"]; return;
    }
    if (!size) return;
    VkDeviceSize srcOffset = [a rdna4VkOffset] + so, dstOffset = [b rdna4VkOffset] + doff;
    if ([a vkBuffer] == [b vkBuffer] && srcOffset < dstOffset + size && dstOffset < srcOffset + size) {
        [_cb rdna4Fail:@"blit buffer copy source and destination overlap"]; return;
    }
    if ([a rdna4Imported] && [b rdna4Imported]) {
        uintptr_t ap = (uintptr_t)a.contents + so, bp = (uintptr_t)b.contents + doff;
        if (ap < bp ? bp - ap < size : ap - bp < size) {
            [_cb rdna4Fail:@"blit buffer copy imported source and destination overlap"]; return;
        }
    }
    [_cb rdna4RetainArgument:src]; [_cb rdna4RetainArgument:dst];
    RDNA4ArgumentWriteIntent *copyIntent = rdna4_argument_sampler_copy(a, so, b, doff, size);
    if (!copyIntent) {
        [_cb rdna4Fail:@"blit buffer copy argument-write intent is invalid"];
        return;
    }
    [_cb rdna4ArgumentIntent:copyIntent];
    uint64_t ao = 0, bo = 0;
    VkDeviceMemory am = [a rdna4NativeMemory:&ao], bm = [b rdna4NativeMemory:&bo];
    [_cb rdna4NativeRecord:(RDNA4NativeOp){ .src = am, .dst = bm,
        .srcOffset = ao <= UINT64_MAX - so ? ao + so : UINT64_MAX,
        .dstOffset = bo <= UINT64_MAX - doff ? bo + doff : UINT64_MAX, .size = size }];
    // Vulkan buffer copies allow the byte offsets introduced by an unaligned import.
    VkBufferCopy bc = { srcOffset, dstOffset, size };
    vkCmdCopyBuffer([_cb rdna4BlitVK], [(RDNA4Buffer *)src vkBuffer], [(RDNA4Buffer *)dst vkBuffer], 1, &bc);
    rdna4_transfer_barrier([_cb rdna4BlitVK]);
}
- (void)fillBuffer:(id)buf range:(NSRange)r value:(uint8_t)v {
    if (_ended) { [_cb rdna4Fail:@"buffer fill after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    if (![buf isKindOfClass:[RDNA4Buffer class]]) { [_cb rdna4Fail:@"blit fillBuffer: bad class"]; return; }
    if (rdna4nc_drop_write(buf, RDNA4NC_FILL, "fillBuffer:", "BlitCommandEncoder")) { [_cb rdna4Fail:@"fillBuffer: destination is read-only"]; return; }
    if (r.location > [buf length] || r.length > [buf length] - r.location) {
        [_cb rdna4Fail:@"blit fillBuffer: range outside the buffer"]; return;
    }
    if (!r.length) return;
    [_cb rdna4RetainArgument:buf];
    [_cb rdna4ArgumentWrite:buf range:r candidates:nil fillOnly:YES];
    uint32_t w = v * 0x01010101u;
    uint64_t bo = 0; VkDeviceMemory bm = [(RDNA4Buffer *)buf rdna4NativeMemory:&bo];
    [_cb rdna4NativeRecord:(RDNA4NativeOp){ .dst = bm,
        .dstOffset = bo <= UINT64_MAX - r.location ? bo + r.location : UINT64_MAX,
        .size = r.length, .pattern = w, .fill = true }];
    VkDeviceSize dstOffset = [(RDNA4Buffer *)buf rdna4VkOffset] + r.location;
    NSUInteger head = MIN(r.length, (4 - (dstOffset & 3)) & 3);
    NSUInteger tail = (r.length - head) & 3, middle = r.length - head - tail;
    RDNA4Buffer *edge = nil; NSUInteger offset = 0;
    // Metal accepts byte ranges; Vulkan fill only accepts whole aligned words.
    if ((head || tail) && ![_cb rdna4Bytes:&w length:sizeof w buffer:&edge offset:&offset]) return;
    if (middle) vkCmdFillBuffer([_cb rdna4BlitVK], [(RDNA4Buffer *)buf vkBuffer], dstOffset + head, middle, w);
    VkBufferCopy edges[2]; uint32_t count = 0;
    if (head) edges[count++] = (VkBufferCopy){ [edge rdna4VkOffset] + offset, dstOffset, head };
    if (tail) edges[count++] = (VkBufferCopy){ [edge rdna4VkOffset] + offset, dstOffset + head + middle, tail };
    if (count) vkCmdCopyBuffer([_cb vk], [edge vkBuffer], [(RDNA4Buffer *)buf vkBuffer], count, edges);
    rdna4_transfer_barrier([_cb rdna4BlitVK]);
}
- (void)updateFence:(id)f {
    if (_ended) { [_cb rdna4Fail:@"blit updateFence after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    [_cb rdna4UpdateFence:f];
}
- (void)waitForFence:(id)f {
    if (_ended) { [_cb rdna4Fail:@"blit waitForFence after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    [_cb rdna4WaitForFence:f];
}
- (void)sampleCountersInBuffer:(id<MTLCounterSampleBuffer>)sampleBuffer
                 atSampleIndex:(NSUInteger)sampleIndex
                   withBarrier:(BOOL)barrier {
    if (_ended) { [_cb rdna4Fail:@"blit sampleCountersInBuffer after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    NSString *why = nil;
    id<RDNA4CounterSampleBufferInternal> csb = rdna4_counter_buffer_check(sampleBuffer, [_cb device], sampleIndex, YES, &why);
    if (!csb) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"blit sampleCountersInBuffer: %@", why ?: @"invalid"]];
        return;
    }
    // Use [_cb vk] (not rdna4BlitVK) so the native blit fast path cannot bypass timestamp writes.
    [csb rdna4SampleOnCommandBuffer:_cb vkCommandBuffer:[_cb vk] sampleIndex:sampleIndex barrier:barrier
                         stageFlags:(barrier ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT)
                                who:"blit"];
}
- (void)resolveCounters:(id<MTLCounterSampleBuffer>)sampleBuffer
                inRange:(NSRange)range
      destinationBuffer:(id<MTLBuffer>)destinationBuffer
      destinationOffset:(NSUInteger)destinationOffset {
    if (_ended) { [_cb rdna4Fail:@"blit resolveCounters after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    NSString *why = nil;
    id<RDNA4CounterSampleBufferInternal> csb = rdna4_counter_buffer_check(sampleBuffer, [_cb device], 0, NO, &why);
    if (!csb) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"blit resolveCounters: %@", why ?: @"invalid"]];
        return;
    }
    if (!rdna4cnt_range_ok((uint64_t)[csb sampleCount], (uint64_t)range.location, (uint64_t)range.length)) {
        [_cb rdna4Fail:@"blit resolveCounters: range out of bounds"];
        return;
    }
    if (![destinationBuffer isKindOfClass:[RDNA4Buffer class]] || [(RDNA4Buffer *)destinationBuffer device] != [_cb device]) {
        [_cb rdna4Fail:@"blit resolveCounters: destinationBuffer is invalid"];
        return;
    }
    if (rdna4nc_drop_write(destinationBuffer, RDNA4NC_COPY_BUFFER, "resolveCounters:", "BlitCommandEncoder")) {
        [_cb rdna4Fail:@"blit resolveCounters: destination is read-only"];
        return;
    }
    RDNA4Buffer *dst = (RDNA4Buffer *)destinationBuffer;
    const uint64_t bytes = (uint64_t)range.length * sizeof(uint64_t);
    if ((destinationOffset & 3u) || !rdna4_buffer_range(dst.length, destinationOffset, bytes)) {
        [_cb rdna4Fail:@"blit resolveCounters: destinationBuffer range or alignment is invalid"];
        return;
    }
    [_cb rdna4RetainEncoded:csb];
    [_cb rdna4RetainEncoded:dst];
    [_cb rdna4ArgumentWrite:dst range:NSMakeRange(destinationOffset, (NSUInteger)bytes) candidates:nil fillOnly:YES];
    NSError *queueErr = nil;
    if (![_cb rdna4EncodeOrderedHostAction:^BOOL(NSError **actionErr) {
        uint64_t *tmp = (uint64_t *)malloc((size_t)bytes);
        if (!tmp) {
            if (actionErr) *actionErr = rdna4_err(66, @"blit resolveCounters: out of memory");
            return NO;
        }
        BOOL ok = [csb rdna4ResolveRange:range intoBytes:tmp error:actionErr];
        if (ok) {
            if ([dst contents]) {
                memcpy((uint8_t *)[dst contents] + destinationOffset, tmp, (size_t)bytes);
            } else {
                RDNA4Buffer *stg = [[RDNA4Buffer alloc] initWithDevice:[dst device]
                                                                length:(NSUInteger)bytes
                                                               options:MTLResourceStorageModeShared
                                                                 error:actionErr];
                if (!stg || ![stg contents]) {
                    if (actionErr && !*actionErr) *actionErr = rdna4_err(66, @"blit resolveCounters: staging buffer allocation failed");
                    ok = NO;
                } else {
                    memcpy([stg contents], tmp, (size_t)bytes);
                    BOOL unresolved = NO;
                    ok = rdna4_oneshot(actionErr, ^(VkCommandBuffer cmd) {
                        VkBufferCopy bc = { [stg rdna4VkOffset], [dst rdna4VkOffset] + destinationOffset, bytes };
                        vkCmdCopyBuffer(cmd, [stg vkBuffer], [dst vkBuffer], 1u, &bc);
                        rdna4_full_barrier(cmd);
                    }, &unresolved);
                    if (unresolved) {
                        (void)CFBridgingRetain(stg);
                        (void)CFBridgingRetain(dst);
                    }
                }
            }
        }
        free(tmp);
        return ok;
    } error:&queueErr]) {
        if (![_cb rdna4Error]) [_cb rdna4Fail:queueErr.localizedDescription ?: @"blit resolveCounters failed"];
    }
}
RDNA4_ENCODER_SHARED
// m11h9: a box-filtered chain, level L-1 -> L with vkCmdBlitImage (linear filter when RADV can filter the format, else nearest) inside one GENERAL-layout image (source and
// destination are different levels, so the regions never overlap); each blit is ordered after the previous by a full memory barrier.
- (void)generateMipmapsForTexture:(id)tex {
    if (![tex isKindOfClass:[RDNA4Texture class]]) { [_cb rdna4Fail:@"blit generateMipmapsForTexture: bad class"]; return; }
    if (rdna4nc_drop_write(tex, RDNA4NC_MIPMAPS, "generateMipmapsForTexture:", "BlitCommandEncoder")) { [_cb rdna4Fail:@"generateMipmapsForTexture: destination is read-only"]; return; }
    RDNA4Texture *t = tex; uint32_t nl = (uint32_t)[t mipmapLevelCount];
    if ([t rdna4IsDS] || [t rdna4Samples] > 1 || [t isCompressed]) { [_cb rdna4Fail:@"blit generateMipmapsForTexture: depth/stencil, multisample and compressed textures cannot be mip-mapped here"]; return; }   // bundle 10
    if (nl <= 1) { RDNA4_ONCE("generateMipmapsForTexture: the texture has a single level; nothing to generate"); return; }
    VkFormatProperties fp = {0}; vkGetPhysicalDeviceFormatProperties(RDNA4R.pd, [t vkFormat], &fp);
    if ((fp.optimalTilingFeatures & (VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT)) != (VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"generateMipmapsForTexture: vk format %d cannot be blitted on RADV (features 0x%x)", [t vkFormat], fp.optimalTilingFeatures]]; return; }
    VkFilter fl = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    [_cb rdna4RetainArgument:t];
    VkCommandBuffer cmd = [_cb vk];
    rdna4_tex_to(cmd, t, VK_IMAGE_LAYOUT_GENERAL);
    for (uint32_t L = 1; L < nl; L++) {
        rdna4_full_barrier(cmd);
        int32_t sw = (int32_t)MAX((NSUInteger)1, [t width] >> (L - 1)), sh = (int32_t)MAX((NSUInteger)1, [t height] >> (L - 1)), sd = (int32_t)MAX((NSUInteger)1, [t depth] >> (L - 1));
        int32_t dw = (int32_t)MAX((NSUInteger)1, [t width] >> L), dh = (int32_t)MAX((NSUInteger)1, [t height] >> L), dd = (int32_t)MAX((NSUInteger)1, [t depth] >> L);
        VkImageBlit bl = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, [t rdna4MipLevel:L - 1], [t rdna4Slice:0], [t rdna4Layers] }, .srcOffsets = { { 0, 0, 0 }, { sw, sh, sd } },
                           .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, [t rdna4MipLevel:L], [t rdna4Slice:0], [t rdna4Layers] }, .dstOffsets = { { 0, 0, 0 }, { dw, dh, dd } } };
        vkCmdBlitImage(cmd, [t vkImage], VK_IMAGE_LAYOUT_GENERAL, [t vkImage], VK_IMAGE_LAYOUT_GENERAL, 1, &bl, fl);
    }
    rdna4_full_barrier(cmd);
    RDNA4LOG("blit generateMipmapsForTexture: %u levels of %lux%lux%lu (vk format %d, %s filter)", nl, (unsigned long)[t width], (unsigned long)[t height], (unsigned long)[t depth], [t vkFormat], fl == VK_FILTER_LINEAR ? "linear" : "nearest");
}
- (void)copyFromTexture:(id)src sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl toTexture:(id)dst
      destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl sliceCount:(NSUInteger)sc levelCount:(NSUInteger)lc {
    if (_ended) { [_cb rdna4Fail:@"texture copy after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    if (![src isKindOfClass:[RDNA4Texture class]] || ![dst isKindOfClass:[RDNA4Texture class]]) {
        [_cb rdna4Fail:@"blit texture range copy: bad classes"]; return;
    }
    RDNA4Texture *a = src, *b = dst;
    NSUInteger al = [a mipmapLevelCount], bl = [b mipmapLevelCount], as = [a rdna4Layers], bs = [b rdna4Layers];
    if (a.pixelFormat != b.pixelFormat || [a rdna4Samples] != [b rdna4Samples]) {
        [_cb rdna4Fail:@"blit texture range copy: pixel format or sample count differs"]; return;
    }
    if (sl > al || lc > al - sl || dl > bl || lc > bl - dl || ss > as || sc > as - ss || ds > bs || sc > bs - ds) {
        [_cb rdna4Fail:@"blit texture range copy: mip or slice range outside a texture"]; return;
    }
    if (!sc || !lc) return;
    if (MAX((NSUInteger)1, a.width >> sl) != MAX((NSUInteger)1, b.width >> dl) ||
        MAX((NSUInteger)1, a.height >> sl) != MAX((NSUInteger)1, b.height >> dl) ||
        MAX((NSUInteger)1, a.depth >> sl) != MAX((NSUInteger)1, b.depth >> dl)) {
        [_cb rdna4Fail:@"blit texture range copy: starting mip dimensions differ"]; return;
    }
    uint32_t sourceMip = [a rdna4MipLevel:sl], destinationMip = [b rdna4MipLevel:dl];
    uint32_t sourceSlice = [a rdna4Slice:ss], destinationSlice = [b rdna4Slice:ds];
    if ([a vkImage] == [b vkImage] && sourceSlice < destinationSlice + sc && destinationSlice < sourceSlice + sc &&
        sourceMip < destinationMip + lc && destinationMip < sourceMip + lc) {
        [_cb rdna4Fail:@"blit texture range copy: source and destination overlap"]; return;
    }
    for (NSUInteger level = 0; level < lc; ++level) {
        MTLSize size = MTLSizeMake(MAX((NSUInteger)1, a.width >> (sl + level)),
            MAX((NSUInteger)1, a.height >> (sl + level)), MAX((NSUInteger)1, a.depth >> (sl + level)));
        for (NSUInteger slice = 0; slice < sc; ++slice)
            [self copyFromTexture:a sourceSlice:ss + slice sourceLevel:sl + level sourceOrigin:MTLOriginMake(0,0,0) sourceSize:size
                toTexture:b destinationSlice:ds + slice destinationLevel:dl + level destinationOrigin:MTLOriginMake(0,0,0)];
    }
}
- (void)copyFromTexture:(id)src toTexture:(id)dst {
    if (![src isKindOfClass:[RDNA4Texture class]] || ![dst isKindOfClass:[RDNA4Texture class]]) {
        [_cb rdna4Fail:@"blit copyFromTexture:toTexture: bad classes"]; return;
    }
    RDNA4Texture *a = src, *b = dst;
    NSUInteger sl = 0, dl = 0;
    BOOL matched = NO;
    for (NSUInteger level = 0; level < a.mipmapLevelCount; ++level) {
        if (MAX((NSUInteger)1, a.width >> level) == b.width && MAX((NSUInteger)1, a.height >> level) == b.height &&
            MAX((NSUInteger)1, a.depth >> level) == b.depth) { sl = level; matched = YES; break; }
    }
    if (!matched) for (NSUInteger level = 1; level < b.mipmapLevelCount; ++level) {
        if (MAX((NSUInteger)1, b.width >> level) == a.width && MAX((NSUInteger)1, b.height >> level) == a.height &&
            MAX((NSUInteger)1, b.depth >> level) == a.depth) { dl = level; matched = YES; break; }
    }
    if (!matched) { [_cb rdna4Fail:@"blit copyFromTexture:toTexture: no mip matches the smaller texture"]; return; }
    [self copyFromTexture:a sourceSlice:0 sourceLevel:sl toTexture:b destinationSlice:0 destinationLevel:dl
        sliceCount:MIN([a rdna4Layers], [b rdna4Layers]) levelCount:MIN(a.mipmapLevelCount - sl, b.mipmapLevelCount - dl)];
}
// the MTLBlitOption variants: option 0 (none) = the plain call; PVRTC row-linear options are not supported
- (void)copyFromTexture:(id)src sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl sourceOrigin:(MTLOrigin)so sourceSize:(MTLSize)sz
               toBuffer:(id)dst destinationOffset:(NSUInteger)doff destinationBytesPerRow:(NSUInteger)bpr destinationBytesPerImage:(NSUInteger)bpi options:(NSUInteger)o {
    if (o) { [_cb rdna4Fail:@"blit copyFromTexture:toBuffer:options: blit options are not supported"]; return; }
    [self copyFromTexture:src sourceSlice:ss sourceLevel:sl sourceOrigin:so sourceSize:sz toBuffer:dst destinationOffset:doff destinationBytesPerRow:bpr destinationBytesPerImage:bpi];
}
- (void)copyFromBuffer:(id)src sourceOffset:(NSUInteger)so sourceBytesPerRow:(NSUInteger)bpr sourceBytesPerImage:(NSUInteger)bpi sourceSize:(MTLSize)sz
             toTexture:(id)dst destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl destinationOrigin:(MTLOrigin)dorg options:(NSUInteger)o {
    if (o) { [_cb rdna4Fail:@"blit copyFromBuffer:toTexture:options: blit options are not supported"]; return; }
    [self copyFromBuffer:src sourceOffset:so sourceBytesPerRow:bpr sourceBytesPerImage:bpi sourceSize:sz toTexture:dst destinationSlice:ds destinationLevel:dl destinationOrigin:dorg];
}
- (void)copyFromTexture:(id)src sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl sourceOrigin:(MTLOrigin)so sourceSize:(MTLSize)sz
              toTexture:(id)dst destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl destinationOrigin:(MTLOrigin)dorg options:(NSUInteger)o {
    if (o) { [_cb rdna4Fail:@"blit copyFromTexture:toTexture:options: blit options are not supported"]; return; }
    [self copyFromTexture:src sourceSlice:ss sourceLevel:sl sourceOrigin:so sourceSize:sz toTexture:dst destinationSlice:ds destinationLevel:dl destinationOrigin:dorg];
}
- (void)synchronizeResource:(id)r {
    if (_ended || [_cb rdna4Error]) return;
    if ([r isKindOfClass:[RDNA4Texture class]] && [(RDNA4Texture *)r rdna4VramSurfCached]) {
        RDNA4Texture *t = [(RDNA4Texture *)r rootResource];
        (void)[_cb rdna4IOFirstTouch:t];
        if ([t layout] != VK_IMAGE_LAYOUT_UNDEFINED) {
            rdna4_full_barrier([_cb vk]);
            if (rdna4_ios_writeback_ex([_cb vk], t, YES)) {
                [_cb rdna4NoteHostWriteback:t];
                [_cb rdna4IOUnwrite:t];
            }
            rdna4_full_barrier([_cb vk]);
        }
    }
}
- (void)synchronizeTexture:(id)t slice:(NSUInteger)s level:(NSUInteger)l { (void)s; (void)l; [self synchronizeResource:t]; }
- (void)pushDebugGroup:(NSString *)s { (void)s; }
- (void)popDebugGroup {}
- (void)endEncoding {
    if (_ended) return;
    _ended = YES;
    if (_nCsbEnd) {
        rdna4_full_barrier([_cb vk]);
        BOOL cbOk = ![_cb rdna4Error];
        for (uint32_t k = 0; k < _nCsbEnd; k++) {
            if (cbOk && _csbEnd[k] && _csbEndIdx[k] != MTLCounterDontSample)
                [_csbEnd[k] rdna4SampleOnCommandBuffer:_cb vkCommandBuffer:[_cb vk]
                                           sampleIndex:_csbEndIdx[k] barrier:NO
                                            stageFlags:VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT
                                                   who:"blit-pass-end"];
            _csbEnd[k] = nil;
        }
        _nCsbEnd = 0u;
    } else {
        rdna4_full_barrier([_cb rdna4BlitVK]);
    }
    [_cb rdna4ArgumentCheckpoint];
    RDNA4LOG("RDNA4BlitEncoder endEncoding");
    [super endEncoding];
}
@end

