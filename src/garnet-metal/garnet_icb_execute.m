// garnet_icb_execute.m - GarnetMetal modular compilation unit: indirect command execution on encoders.
#if !defined(GARNET_INTERNAL) && !defined(GARNET_METAL_MODULAR_BUILD)
#define GARNET_INTERNAL static
#endif
#ifdef GARNET_METAL_MODULAR_BUILD
#include "garnet_metal_internal.h"
#endif
#include "garnet_icb_internal.h"

#ifndef RDNA4_ICB_EXECUTE_INC
#define RDNA4_ICB_EXECUTE_INC

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
#ifdef VK_VERSION_1_0
#define RDNA4_ICB_STAGE_DECL() RDNA4RenderScratch *const icbSc = [self rdna4EnsureScratch]
#define RDNA4_ICB_SV (icbSc->sv)
#define RDNA4_ICB_SF (icbSc->sf)
#else
#define RDNA4_ICB_STAGE_DECL() ((void)0)
#define RDNA4_ICB_SV (_sv)
#define RDNA4_ICB_SF (_sf)
#endif
- (BOOL)rdna4ICBInheritedBuffersSatisfyPipeline:(id)pipeline {
    if (![pipeline isKindOfClass:[RDNA4RenderPipelineState class]]) return NO;
    RDNA4RenderPipelineState *use = [(RDNA4RenderPipelineState *)pipeline rdna4Real] ?: pipeline;
    id device = [self rdna4ICBDevice];
    RDNA4_ICB_STAGE_DECL();
#ifdef VK_VERSION_1_0
    if ([use rdna4IsTessellation]) {
        const rdna4_tess_cfg_t *cfg = [use tessCfg];
        for (uint32_t i = 0; i < cfg->num_inputs; i++) {
            const rdna4_tess_input_t *in = &cfg->inputs[i];
            if (!in->has_attr) continue;
            RDNA4Buffer *vb = RDNA4_ICB_SV.buf[in->buf_idx].b;
            NSUInteger vo = RDNA4_ICB_SV.buf[in->buf_idx].off;
            if (!vb || ![vb isKindOfClass:[RDNA4Buffer class]] || [vb device] != device ||
                vo > [vb length] || ![vb gpuAddress] || vo > UINT64_MAX - [vb gpuAddress])
                return NO;
        }
    } else
#endif
    {
        const uint32_t *needed = [use needBindings];
        uint32_t count = [use nNeed];
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t index = needed[i];
            if (index == 31) continue;
            if (index >= 31 || !RDNA4_ICB_SV.buf[index].b || RDNA4_ICB_SV.buf[index].off > [RDNA4_ICB_SV.buf[index].b length]) return NO;
        }
    }
    return rdna4icb_stage_buffers_ok([use pbV], [use npbV], &RDNA4_ICB_SV, device) &&
           rdna4icb_stage_buffers_ok([use pbF], [use npbF], &RDNA4_ICB_SF, device);
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
        rdna4_stage_set_buf(&vertex, index, bind.buffer, bind.offset);
    }
    for (NSNumber *slot in record.fragmentBinds) {
        NSUInteger index = slot.unsignedIntegerValue;
        RDNA4ICBBind *bind = record.fragmentBinds[slot];
        if (index >= 31 || !bind.buffer || [bind.buffer device] != device ||
            bind.offset > bind.buffer.length)
            return NO;
        rdna4_stage_set_buf(&fragment, index, bind.buffer, bind.offset);
    }
#ifdef VK_VERSION_1_0
    if ([use rdna4IsTessellation]) {
        const rdna4_tess_cfg_t *cfg = [use tessCfg];
        for (uint32_t i = 0; i < cfg->num_inputs; i++) {
            const rdna4_tess_input_t *in = &cfg->inputs[i];
            if (!in->has_attr) continue;
            RDNA4Buffer *vb = vertex.buf[in->buf_idx].b;
            NSUInteger vo = vertex.buf[in->buf_idx].off;
            if (!vb || ![vb isKindOfClass:[RDNA4Buffer class]] || [vb device] != device ||
                vo > [vb length] || ![vb gpuAddress] || vo > UINT64_MAX - [vb gpuAddress])
                return NO;
        }
    } else
#endif
    {
        const uint32_t *needed = [use needBindings];
        uint32_t count = [use nNeed];
        for (uint32_t i = 0; i < count; ++i)
            if (needed[i] != 31 && !record.vertexBinds[@(needed[i])]) return NO;
    }
    return rdna4icb_stage_buffers_ok([use pbV], [use npbV], &vertex, device) &&
           rdna4icb_stage_buffers_ok([use pbF], [use npbF], &fragment, device);
}
- (void)rdna4ICBClearExplicitBuffers {
    RDNA4_ICB_STAGE_DECL();
    for (NSUInteger i = 0; i < 31; ++i) {
        rdna4_stage_set_buf(&RDNA4_ICB_SV, i, nil, 0);
        rdna4_stage_set_buf(&RDNA4_ICB_SF, i, nil, 0);
    }
}
- (void)rdna4ICBRetainResource:(id)resource {
#ifdef VK_VERSION_1_0
    [_cb rdna4RetainEncoded:resource];
#else
    [_cb rdna4Retain:resource];
#endif
}
#ifdef VK_VERSION_1_0
- (void)rdna4ExecuteICBGPU:(RDNA4IndirectCommandBuffer *)icb
               directRange:(NSRange)range
            indirectBuffer:(id)indirectBuf
            indirectOffset:(NSUInteger)indirectOff
               useIndirect:(BOOL)useIndirect {
    if (!vkCmdDrawIndirect || !RDNA4R.indirectFirstInstance || !RDNA4R.bufferAddress) {
        [_cb rdna4Fail:@"GPU ICB execution requires indirect drawing and bufferDeviceAddress"];
        return;
    }
    id dev = [_cb device];
    if ([icb device] != dev) {
        [_cb rdna4Fail:@"indirect command buffer belongs to a different device"];
        return;
    }
    if (icb->_isCompute) {
        [_cb rdna4Fail:@"cannot execute a compute indirect command buffer on a render encoder"];
        return;
    }
    if (useIndirect) {
        if (![indirectBuf isKindOfClass:[RDNA4Buffer class]] || [(RDNA4Buffer *)indirectBuf device] != dev ||
            (indirectOff & 3) || indirectOff > [(RDNA4Buffer *)indirectBuf length] ||
            8 > [(RDNA4Buffer *)indirectBuf length] - indirectOff) {
            [_cb rdna4Fail:@"GPU ICB indirect range buffer, alignment or range is invalid"];
            return;
        }
        if (!rdna4nc_vk_offset_ok([(RDNA4Buffer *)indirectBuf rdna4VkOffset] + indirectOff, 4, RDNA4NC_ALIGN_INDIRECT)) {
            [_cb rdna4Fail:@"GPU ICB indirect range Vulkan offset must be a multiple of 4"];
            return;
        }
    } else {
        NSString *rangeProblem = rdna4icb_range_problem(range, icb->_commandCount, YES);
        if (rangeProblem) {
            [_cb rdna4Fail:[NSString stringWithFormat:@"executeCommandsInBuffer [%lu,+%lu): %@",
                            (unsigned long)range.location, (unsigned long)range.length, rangeProblem]];
            return;
        }
        if (!range.length) {
            [_cb rdna4RetainEncoded:icb];
            id bk = [icb rdna4GPUBacking];
            if (bk) [_cb rdna4RetainEncoded:bk];
            return;
        }
    }
    if (![icb rdna4EnsureGPUBackingAddress]) {
        [_cb rdna4Fail:@"cannot allocate GPU backing for indirect command buffer"];
        return;
    }
    NSError *syncErr = nil;
    if (![icb rdna4SyncCPUWritesToGPU:&syncErr]) {
        [_cb rdna4Fail:syncErr.localizedDescription ?: @"cannot synchronize ICB CPU writes to GPU"];
        return;
    }
    pthread_mutex_lock(&icb->_mu);
    NSString *fatal = icb->_fatalError;
    RDNA4Buffer *icbBacking = icb->_gpuBacking;
    NSUInteger maxCommands = icb->_commandCount;
    BOOL inheritPipe = icb->_inheritPipelineState;
    BOOL inheritBufs = icb->_inheritBuffers;
    MTLIndirectCommandType cmdTypes = icb->_commandTypes;
    NSArray<RDNA4RenderPipelineState *> *rawCands = inheritPipe ? nil : [icb->_candidatePipelines copy];
    NSArray *retainedRes = [icb->_gpuRetained allObjects];
    pthread_mutex_unlock(&icb->_mu);
    if (fatal) { [_cb rdna4Fail:fatal]; return; }
    if (!icbBacking) { [_cb rdna4Fail:@"indirect command buffer lacks GPU backing"]; return; }

    [_cb rdna4RetainEncoded:icb];
    [_cb rdna4RetainEncoded:icbBacking];
    for (id res in retainedRes) [_cb rdna4RetainEncoded:res];
    if (useIndirect) [_cb rdna4RetainEncoded:indirectBuf];

    uint32_t maxOutSlots = useIndirect ? (uint32_t)MIN(maxCommands, (NSUInteger)RDNA4ICB_MAX_EXECUTION_RANGE) : (uint32_t)range.length;
    if (!maxOutSlots) return;

    NSMutableArray<RDNA4RenderPipelineState *> *cands = [NSMutableArray array];
    uint64_t parentToken = 0;
    if (inheritPipe) {
        if (!_pso) { [_cb rdna4Fail:@"executeCommandsInBuffer with inheritPipelineState requires a bound render pipeline"]; return; }
        RDNA4RenderPipelineState *use = [_pso rdna4Real] ?: _pso;
        if (![use supportIndirectCommandBuffers] || ![self rdna4ICBCompatiblePipeline:use]) {
            [_cb rdna4Fail:@"inherited pipeline is not ICB-capable or incompatible with the render pass"];
            return;
        }
        parentToken = [use rdna4ICBToken];
        if (!parentToken) { [_cb rdna4Fail:@"inherited pipeline has no ICB token"]; return; }
        [cands addObject:use];
    } else {
        for (RDNA4RenderPipelineState *raw in rawCands) {
            RDNA4RenderPipelineState *use = [raw rdna4Real] ?: raw;
            if (![use supportIndirectCommandBuffers] || ![self rdna4ICBCompatiblePipeline:use] || ![use rdna4ICBToken]) {
                [_cb rdna4Fail:@"candidate pipeline is not ICB-capable or incompatible with the render pass"];
                return;
            }
            [cands addObject:use];
        }
        if (!cands.count) return;
    }

    BOOL icbFetch = [cands[0] fetch];
    for (RDNA4RenderPipelineState *P in cands) {
        if ([P fetch] != icbFetch) {
            [_cb rdna4Fail:@"mixing framebuffer-fetch and non-fetch pipelines in one ICB execution is not supported"];
            return;
        }
        if ([P fetch] && [P fetchMax] >= _na) {
            [_cb rdna4Fail:[NSString stringWithFormat:@"ICB pipeline reads [[color(%u)]] but pass has %u attachment(s)", [P fetchMax], _na]];
            return;
        }
        [_cb rdna4RetainEncoded:P];
    }

    VkCullModeFlags cull = _cull == MTLCullModeFront ? VK_CULL_MODE_FRONT_BIT : _cull == MTLCullModeBack ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    VkFrontFace front = _wind == MTLWindingCounterClockwise ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    rdna4_icb_expand_cfg_t cfg = {0};
    uint32_t bucketCand[RDNA4_ICB_MAX_EXPAND_BUCKETS];
    VkPipeline bucketVkPipe[RDNA4_ICB_MAX_EXPAND_BUCKETS];
    uint32_t nb = 0;

    for (uint32_t ci = 0; ci < (uint32_t)cands.count; ci++) {
        RDNA4RenderPipelineState *P = cands[ci];
        NSError *prepErr = nil;
        if (![P rdna4PrepareICBGPUVariant:&prepErr]) {
            [_cb rdna4Fail:prepErr.localizedDescription ?: @"cannot prepare GPU ICB pipeline variant"];
            return;
        }
        uint32_t dk = [P rdna4DSKey:&_dsi];
        uint64_t tok = [P rdna4ICBToken];
        if ([P rdna4IsTessellation]) {
            if (!(cmdTypes & (RDNA4ICB_TYPE_DRAW_PATCHES | RDNA4ICB_TYPE_DRAW_INDEXED_PATCHES))) continue;
            VkPipeline vkp = [P icbPipelineForTopology:VK_PRIMITIVE_TOPOLOGY_PATCH_LIST cull:cull front:front ds:dk];
            if (!vkp) { [_cb rdna4Fail:@"cannot build GPU ICB tessellation pipeline variant"]; return; }
            if (nb >= RDNA4_ICB_MAX_EXPAND_BUCKETS) { [_cb rdna4Fail:@"GPU ICB bucket overflow"]; return; }
            cfg.buckets[nb] = (rdna4_icb_bucket_t){ .token = tok, .is_tess = 1u, .prim_type = 0u };
            bucketCand[nb] = ci; bucketVkPipe[nb] = vkp; nb++;
        } else {
            if (!(cmdTypes & (MTLIndirectCommandTypeDraw | MTLIndirectCommandTypeDrawIndexed))) continue;
            for (uint32_t pt = 0; pt < 5; pt++) {
                VkPipeline vkp = [P icbPipelineForTopology:rdna4_topo((MTLPrimitiveType)pt) cull:cull front:front ds:dk];
                if (!vkp) { [_cb rdna4Fail:@"cannot build GPU ICB render pipeline variant"]; return; }
                if (nb >= RDNA4_ICB_MAX_EXPAND_BUCKETS) { [_cb rdna4Fail:@"GPU ICB bucket overflow"]; return; }
                cfg.buckets[nb] = (rdna4_icb_bucket_t){ .token = tok, .is_tess = 0u, .prim_type = pt };
                bucketCand[nb] = ci; bucketVkPipe[nb] = vkp; nb++;
            }
        }
    }
    if (!nb) return;
    if ((uint64_t)maxOutSlots * (uint64_t)nb > RDNA4_ICB_MAX_GPU_EXPANDED_DRAWS) {
        [_cb rdna4Fail:@"GPU ICB expanded draw count exceeds limit"];
        return;
    }

    cfg.direct_location = useIndirect ? 0u : (uint32_t)range.location;
    cfg.direct_length = useIndirect ? 0u : (uint32_t)range.length;
    cfg.use_indirect_range = useIndirect ? 1u : 0u;
    cfg.max_output_slots = maxOutSlots;
    cfg.inherit_pipeline = inheritPipe ? 1u : 0u;
    cfg.inherit_buffers = inheritBufs ? 1u : 0u;
    cfg.num_buckets = nb;
    cfg.parent_pipeline_token = parentToken;
    cfg.parent_tess_factor_scale = _tessFactorScale;
    if ([_tessFactorBuf isKindOfClass:[RDNA4Buffer class]] && [(RDNA4Buffer *)_tessFactorBuf device] == dev &&
        _tessFactorOff <= [(RDNA4Buffer *)_tessFactorBuf length] && [(RDNA4Buffer *)_tessFactorBuf gpuAddress]) {
        cfg.parent_tess_factor_bda = [(RDNA4Buffer *)_tessFactorBuf gpuAddress] + _tessFactorOff;
        cfg.parent_tess_factor_size = [(RDNA4Buffer *)_tessFactorBuf length] - _tessFactorOff;
        cfg.parent_tess_factor_stride = (uint32_t)_tessFactorStride;
        [_cb rdna4RetainEncoded:_tessFactorBuf];
    }
    RDNA4RenderScratch *icbSc = [self rdna4EnsureScratch];
    for (uint32_t j = 0; j < 31; j++) {
        if (icbSc->sv.buf[j].b && [icbSc->sv.buf[j].b gpuAddress]) {
            if ([icbSc->sv.buf[j].b device] != dev ||
                !rdna4_buffer_table_set(&cfg.parent_v_table, j, [icbSc->sv.buf[j].b gpuAddress],
                                        icbSc->sv.buf[j].b.length, icbSc->sv.buf[j].off, [icbSc->sv.buf[j].b rdna4HostReadOnly])) {
                [_cb rdna4Fail:@"parent vertex buffer slot is invalid for ICB execution"];
                return;
            }
            cfg.parent_vb_bda[j] = [icbSc->sv.buf[j].b gpuAddress] + icbSc->sv.buf[j].off;
            cfg.parent_vb_size[j] = icbSc->sv.buf[j].b.length - icbSc->sv.buf[j].off;
            [_cb rdna4RetainEncoded:icbSc->sv.buf[j].b];
        }
        if (icbSc->sf.buf[j].b && [icbSc->sf.buf[j].b gpuAddress]) {
            if ([icbSc->sf.buf[j].b device] != dev ||
                !rdna4_buffer_table_set(&cfg.parent_f_table, j, [icbSc->sf.buf[j].b gpuAddress],
                                        icbSc->sf.buf[j].b.length, icbSc->sf.buf[j].off, [icbSc->sf.buf[j].b rdna4HostReadOnly])) {
                [_cb rdna4Fail:@"parent fragment buffer slot is invalid for ICB execution"];
                return;
            }
            [_cb rdna4RetainEncoded:icbSc->sf.buf[j].b];
        }
    }

    RDNA4Buffer *cfgBuf = nil; NSUInteger cfgOff = 0;
    if (![_cb rdna4Bytes:&cfg length:sizeof cfg buffer:&cfgBuf offset:&cfgOff]) return;
    RDNA4Buffer *dummyRangeBuf = nil; NSUInteger dummyRangeOff = 0;
    if (!useIndirect) {
        uint32_t zeroRange[2] = { 0u, 0u };
        if (![_cb rdna4Bytes:zeroRange length:sizeof zeroRange buffer:&dummyRangeBuf offset:&dummyRangeOff]) return;
    }

    NSUInteger drawsBytes = (NSUInteger)maxOutSlots * (NSUInteger)nb * sizeof(VkDrawIndirectCommand);
    NSUInteger vsBytes = (NSUInteger)maxOutSlots * RDNA4_ICB_VS_PARAMS_STRIDE;
    NSUInteger tessBytes = (NSUInteger)maxOutSlots * RDNA4_ICB_TESS_PARAMS_STRIDE;
    NSUInteger tblBytes = (NSUInteger)maxOutSlots * RDNA4_ICB_TABLE_V2_STRIDE;
    MTLResourceOptions privOpts = MTLResourceStorageModePrivate | (RDNA4P.on ? RDNA4_OPT_NOZERO : 0);
    NSError *bErr = nil;
    RDNA4Buffer *outDraws = [[RDNA4Buffer alloc] initWithDevice:dev length:drawsBytes options:privOpts error:&bErr];
    RDNA4Buffer *outVsParams = [[RDNA4Buffer alloc] initWithDevice:dev length:vsBytes options:privOpts error:&bErr];
    RDNA4Buffer *outTessParams = [[RDNA4Buffer alloc] initWithDevice:dev length:tessBytes options:privOpts error:&bErr];
    RDNA4Buffer *outVTable = [[RDNA4Buffer alloc] initWithDevice:dev length:tblBytes options:privOpts error:&bErr];
    RDNA4Buffer *outFTable = [[RDNA4Buffer alloc] initWithDevice:dev length:tblBytes options:privOpts error:&bErr];
    if (!outDraws || !outVsParams || !outTessParams || !outVTable || !outFTable) {
        [_cb rdna4Fail:bErr.localizedDescription ?: @"cannot allocate GPU ICB expansion buffers"];
        return;
    }
    [_cb rdna4RetainEncoded:cfgBuf];
    if (dummyRangeBuf) [_cb rdna4RetainEncoded:dummyRangeBuf];
    [_cb rdna4RetainEncoded:outDraws];
    [_cb rdna4RetainEncoded:outVsParams];
    [_cb rdna4RetainEncoded:outTessParams];
    [_cb rdna4RetainEncoded:outVTable];
    [_cb rdna4RetainEncoded:outFTable];

    VkDescriptorSetLayout expDsl = VK_NULL_HANDLE;
    VkPipelineLayout expPl = VK_NULL_HANDLE;
    VkPipeline expPipe = VK_NULL_HANDLE;
    NSError *expErr = nil;
    if (!rdna4_icb_get_expander(&expDsl, &expPl, &expPipe, &expErr)) {
        [_cb rdna4Fail:expErr.localizedDescription ?: @"cannot build GPU ICB expander pipeline"];
        return;
    }

    if ([_cb rdna4HasArgumentWrites]) {
        if (_inPass) [self rdna4EndPass];
        [_cb rdna4ArgumentCheckpoint];
        if ([_cb rdna4Error]) return;
    }
    [self rdna4SyncResourceSegment];
    [self rdna4SnapshotResources];
    if (_inPass) [self rdna4EndPass];

    VkCommandBuffer cmd = [_cb vk];
    rdna4_full_barrier(cmd);
    VkDescriptorSet expSet = [_cb rdna4AllocSet:expDsl];
    if (!expSet) return;
    RDNA4Buffer *rangeSrc = useIndirect ? (RDNA4Buffer *)indirectBuf : dummyRangeBuf;
    NSUInteger rangeSrcOff = useIndirect ? indirectOff : dummyRangeOff;
    VkDescriptorBufferInfo expBi[8] = {
        { [icbBacking vkBuffer], [icbBacking rdna4VkOffset], rdna4_icb_backing_bytes((uint32_t)maxCommands) },
        { [cfgBuf vkBuffer], [cfgBuf rdna4VkOffset] + cfgOff, sizeof cfg },
        { [rangeSrc vkBuffer], [rangeSrc rdna4VkOffset] + rangeSrcOff, 8u },
        { [outDraws vkBuffer], [outDraws rdna4VkOffset], drawsBytes },
        { [outVsParams vkBuffer], [outVsParams rdna4VkOffset], vsBytes },
        { [outTessParams vkBuffer], [outTessParams rdna4VkOffset], tessBytes },
        { [outVTable vkBuffer], [outVTable rdna4VkOffset], tblBytes },
        { [outFTable vkBuffer], [outFTable rdna4VkOffset], tblBytes },
    };
    VkWriteDescriptorSet expW[8];
    for (uint32_t i = 0; i < 8; i++) {
        expW[i] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = expSet, .dstBinding = i, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &expBi[i] };
    }
    vkUpdateDescriptorSets(RDNA4R.dev, 8, expW, 0, NULL);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, expPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, expPl, 0, 1, &expSet, 0, NULL);
    vkCmdDispatch(cmd, (maxOutSlots + 63u) / 64u, 1, 1);
    rdna4_full_barrier(cmd);

    void (^needTex)(RDNA4Texture *, VkImageLayout, BOOL) = ^(RDNA4Texture *t, VkImageLayout l, BOOL writable) {
        if ([t rdna4IsIOS]) (void)rdna4_ios_touch(self->_cb, t, YES, writable, ^{
            if (self->_inPass) [self rdna4EndPass]; });
        [self->_cb rdna4CBRead:t];
        if ([t rdna4DispImgOnly]) (void)[self->_cb rdna4IOFirstTouch:t];
        if (([t rdna4IsIOS] || [t rdna4DispImgOnly]) && writable) {
            [self->_cb rdna4IOWritten:t];
            [self->_cb rdna4DispNote:t bits:RDNA4DF_W_COMPUTE pso:nil];
        }
        [self rdna4Need:t layout:l];
    };

    VkDescriptorSet candSet0[RDNA4_ICB_MAX_CANDIDATE_PIPELINES] = {0};
    VkDescriptorSet candSet1[RDNA4_ICB_MAX_CANDIDATE_PIPELINES] = {0};
    VkDescriptorSet candTessSet[RDNA4_ICB_MAX_CANDIDATE_PIPELINES] = {0};
    VkDescriptorSet candArgSets[RDNA4_ICB_MAX_CANDIDATE_PIPELINES][3] = {{0}};
    BOOL candHasArgs[RDNA4_ICB_MAX_CANDIDATE_PIPELINES] = {NO};

    for (uint32_t ci = 0; ci < (uint32_t)cands.count; ci++) {
        RDNA4RenderPipelineState *P = cands[ci];
        MTLRenderPipelineReflection *reflection = [P reflection] ?: _roReflection;
        BOOL pHasFrag = [P rdna4HasFragment];
        rdna4_each_tex([P icbPbV], [P icbNpbV], &icbSc->sv, dev, needTex);
        if (pHasFrag) rdna4_each_tex([P icbPbF], [P icbNpbF], &icbSc->sf, dev, needTex);

        NSMutableSet *argumentResources = [_argumentResources mutableCopy] ?: [NSMutableSet set];
        NSMutableSet *argumentWritable = [_argumentWritable mutableCopy] ?: [NSMutableSet set];
        rdna4_argument_stage_resources(argumentResources, argumentWritable, [P icbPbV], [P icbNpbV], &icbSc->sv,
                                       rdna4nc_stage_arguments(reflection, RDNA4NC_VERTEX));
        if (pHasFrag)
            rdna4_argument_stage_resources(argumentResources, argumentWritable, [P icbPbF], [P icbNpbF], &icbSc->sf,
                                           rdna4nc_stage_arguments(reflection, RDNA4NC_FRAGMENT));
        RDNA4ArgumentScanPrepared scan = {0};
        const RDNA4StageState *scanStages[] = { &icbSc->sv, &icbSc->sf };
        NSString *scanError = nil;
        if (!rdna4_argument_scan_prepare(&scan, scanStages, pHasFrag ? 2 : 1, argumentWritable, dev, &scanError)) {
            [_cb rdna4Fail:scanError ?: @"argument scan preparation failed"];
            return;
        }
        BOOL argumentsReady = [_cb rdna4PrepareArguments:[P argumentTables]
                                               resources:argumentResources
                                                writable:argumentWritable
                                                    scan:&scan
                                                textures:needTex];
        rdna4_argument_scan_prepared_destroy(&scan);
        if (!argumentsReady) return;
        if ([P argumentTables]->active) {
            candHasArgs[ci] = YES;
            [_cb rdna4CopyArgumentSets:candArgSets[ci]];
        }

        candSet0[ci] = [_cb rdna4AllocSet:[P icbDsl:0]];
        if (!candSet0[ci]) return;
        if ([P icbNpbV] && ![_cb rdna4FillSet:candSet0[ci] bindings:[P icbPbV] count:[P icbNpbV] state:&icbSc->sv stage:"icb-vertex"]) return;
        VkDescriptorBufferInfo dynBi0[2];
        VkWriteDescriptorSet dynW0[2];
        uint32_t nDyn0 = 0;
        dynBi0[nDyn0] = (VkDescriptorBufferInfo){ [outVTable vkBuffer], [outVTable rdna4VkOffset], sizeof(rdna4_buffer_table) };
        dynW0[nDyn0] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = candSet0[ci], .dstBinding = RDNA4_ICB_TABLE_BINDING, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, .pBufferInfo = &dynBi0[nDyn0] };
        nDyn0++;
        if ([P rdna4ICBHasVsParams]) {
            dynBi0[nDyn0] = (VkDescriptorBufferInfo){ [outVsParams vkBuffer], [outVsParams rdna4VkOffset], sizeof(rdna4_icb_vs_params_t) };
            dynW0[nDyn0] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = candSet0[ci], .dstBinding = RDNA4_ICB_VS_PARAMS_BINDING, .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, .pBufferInfo = &dynBi0[nDyn0] };
            nDyn0++;
        }
        vkUpdateDescriptorSets(RDNA4R.dev, nDyn0, dynW0, 0, NULL);

        if (pHasFrag) {
            candSet1[ci] = [_cb rdna4AllocSet:[P icbDsl:1]];
            if (!candSet1[ci]) return;
            if ([P icbNpbF] && ![_cb rdna4FillSet:candSet1[ci] bindings:[P icbPbF] count:[P icbNpbF] state:&icbSc->sf stage:"icb-fragment"]) return;
            VkDescriptorBufferInfo fbi = { [outFTable vkBuffer], [outFTable rdna4VkOffset], sizeof(rdna4_buffer_table) };
            VkWriteDescriptorSet fw = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = candSet1[ci], .dstBinding = RDNA4_ICB_TABLE_BINDING, .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, .pBufferInfo = &fbi };
            vkUpdateDescriptorSets(RDNA4R.dev, 1, &fw, 0, NULL);
        }

        if ([P rdna4IsTessellation]) {
            candTessSet[ci] = [_cb rdna4AllocSet:[P icbTessDsl]];
            if (!candTessSet[ci]) return;
            VkDescriptorBufferInfo tbi = { [outTessParams vkBuffer], [outTessParams rdna4VkOffset], sizeof(rdna4_tess_draw_params_t) };
            VkWriteDescriptorSet tw = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = candTessSet[ci], .dstBinding = 0, .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, .pBufferInfo = &tbi };
            vkUpdateDescriptorSets(RDNA4R.dev, 1, &tw, 0, NULL);
        }
    }

    if (icbFetch != _fetch) _fetch = icbFetch;
    [self rdna4BeginPass];
    if ([_cb rdna4Error]) return;
    [self rdna4OccDraw];
    VkViewport vp = { (float)_vp.originX, (float)(_vp.originY + _vp.height), (float)_vp.width, -(float)_vp.height, (float)_vp.znear, (float)_vp.zfar };
    VkRect2D sc = { { (int32_t)_sr.x, (int32_t)_sr.y }, { (uint32_t)_sr.width, (uint32_t)_sr.height } };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdSetBlendConstants(cmd, _blend);
    if ([cands[0] rdna4HasDS]) {
        vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_BIT, _dsi.fRead);
        vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_BACK_BIT, _dsi.bRead);
        vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_BIT, _dsi.fWrite);
        vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_BACK_BIT, _dsi.bWrite);
        vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_BIT, _sref[0]);
        vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_BACK_BIT, _sref[1]);
        if ([cands[0] rdna4HasDepth]) vkCmdSetDepthBias(cmd, _bias[0], _bias[1], _bias[2]);
    }

    VkPipeline curPipe = VK_NULL_HANDLE;
    uint32_t curArgCand = UINT32_MAX;
    for (uint32_t k = 0; k < maxOutSlots; k++) {
        if (_fetch) {
            VkMemoryBarrier fmb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT };
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 VK_DEPENDENCY_BY_REGION_BIT, 1, &fmb, 0, NULL, 0, NULL);
        }
        for (uint32_t b = 0; b < nb; b++) {
            uint32_t ci = bucketCand[b];
            RDNA4RenderPipelineState *P = cands[ci];
            VkPipeline vkp = bucketVkPipe[b];
            if (vkp != curPipe) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkp);
                curPipe = vkp;
                curArgCand = UINT32_MAX;
            }
            VkPipelineLayout pl = [P icbLayout];
            if (candHasArgs[ci] && curArgCand != ci) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 2, 3, candArgSets[ci], 0, NULL);
                curArgCand = ci;
            }
            uint32_t dyn0[2] = { k * RDNA4_ICB_TABLE_V2_STRIDE, k * RDNA4_ICB_VS_PARAMS_STRIDE };
            uint32_t ndyn0 = [P rdna4ICBHasVsParams] ? 2u : 1u;
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &candSet0[ci], ndyn0, dyn0);
            if ([P rdna4HasFragment]) {
                uint32_t fdyn = k * RDNA4_ICB_TABLE_V2_STRIDE;
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 1, 1, &candSet1[ci], 1, &fdyn);
            }
            if ([P rdna4IsTessellation]) {
                uint32_t tdyn = k * RDNA4_ICB_TESS_PARAMS_STRIDE;
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, [P tessSet], 1, &candTessSet[ci], 1, &tdyn);
            }
            VkDeviceSize cmdOff = [outDraws rdna4VkOffset] + ((VkDeviceSize)k * (VkDeviceSize)nb + (VkDeviceSize)b) * sizeof(VkDrawIndirectCommand);
            vkCmdDrawIndirect(cmd, [outDraws vkBuffer], cmdOff, 1, sizeof(VkDrawIndirectCommand));
        }
    }
    if (_dcOn) rdna4dc_enc_reset_bound(&icbSc->dc);
    _drawDirty = GARNET_DRAW_DIRTY_ALL;
    _cleanFastOk = NO;
    _vbDirtyMask = ~0u;
}
#endif
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
    if (icb->_isCompute) {
        [_cb rdna4Fail:@"cannot execute a compute indirect command buffer on a render encoder"];
        return;
    }
#ifdef VK_VERSION_1_0
    pthread_mutex_lock(&icb->_mu);
    BOOL useGPU = (icb->_gpuProduced || icb->_gpuBacking != nil);
    pthread_mutex_unlock(&icb->_mu);
    if (useGPU) {
        [self rdna4ExecuteICBGPU:icb directRange:executionRange indirectBuffer:nil indirectOffset:0 useIndirect:NO];
        return;
    }
#endif
    NSString *problem = nil;
    RDNA4_ICB_STAGE_DECL();
    RDNA4RenderPipelineState *parentPipeline = _pso;
    MTLRenderPipelineReflection *parentReflection = _roReflection;
    RDNA4StageState parentVertex = RDNA4_ICB_SV;
    RDNA4StageState parentFragment = RDNA4_ICB_SF;
#ifdef VK_VERSION_1_0
    id parentTessBuf = _tessFactorBuf;
    NSUInteger parentTessOff = _tessFactorOff;
    NSUInteger parentTessStride = _tessFactorStride;
    [_cb rdna4RetainEncoded:icb];
#else
    [_cb rdna4Retain:icb];
#endif
    NSUInteger replayed = [icb rdna4ExecuteOnEncoder:self range:executionRange reason:&problem];
    _pso = parentPipeline;
    _roReflection = parentReflection;
    RDNA4_ICB_SV = parentVertex;
    RDNA4_ICB_SF = parentFragment;
#ifdef VK_VERSION_1_0
    if (parentPipeline) {
        [parentPipeline rdna4FillFast:&_psoFast];
        [self rdna4OwnResource:parentPipeline kind:RDNA4_RESOURCE_PIPELINE fragment:NO index:0];
    }
    for (uint32_t bi = 0; bi < 31; bi++) {
        if (parentVertex.buf[bi].b) [self rdna4OwnResource:parentVertex.buf[bi].b kind:RDNA4_RESOURCE_BUFFER fragment:NO index:bi];
        if (parentFragment.buf[bi].b) [self rdna4OwnResource:parentFragment.buf[bi].b kind:RDNA4_RESOURCE_BUFFER fragment:YES index:bi];
    }
    _drawDirty = GARNET_DRAW_DIRTY_ALL;
    _cleanFastOk = NO;
    _vbDirtyMask = ~0u;
    _tessFactorBuf = parentTessBuf;
    _tessFactorOff = parentTessOff;
    _tessFactorStride = parentTessStride;
#endif
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
    if (_hostWriteDropped) return;
    if (_ended || [_cb rdna4Error]) {
        [_cb rdna4Fail:@"executeCommandsInBuffer:indirectBuffer: after endEncoding or after an earlier encode error"];
        return;
    }
    if (![indirectCommandBuffer isKindOfClass:[RDNA4IndirectCommandBuffer class]]) {
        [_cb rdna4Fail:@"executeCommandsInBuffer:indirectBuffer: requires an RDNA4IndirectCommandBuffer"];
        return;
    }
    RDNA4IndirectCommandBuffer *icb = (RDNA4IndirectCommandBuffer *)indirectCommandBuffer;
    if (icb->_isCompute) {
        [_cb rdna4Fail:@"cannot execute a compute indirect command buffer on a render encoder"];
        return;
    }
#ifdef VK_VERSION_1_0
    [self rdna4ExecuteICBGPU:icb
                 directRange:NSMakeRange(0, 0)
              indirectBuffer:indirectRangeBuffer
              indirectOffset:indirectBufferOffset
                 useIndirect:YES];
#else
    (void)indirectRangeBuffer; (void)indirectBufferOffset;
    [_cb rdna4Fail:@"GPU-read indirect execution ranges require the Vulkan runtime"];
#endif
}
@end

#ifdef VK_VERSION_1_0
static void rdna4_icb_add_local(const rdna4dc_caps *caps, uint32_t (*locals)[3], uint32_t *nLoc,
                                uint32_t maxLoc, uint32_t lx, uint32_t ly, uint32_t lz) {
    if (*nLoc >= maxLoc || *nLoc >= 48u) return;
    if (!rdna4dc_workgroup_ok(caps, lx, ly, lz)) return;
    for (uint32_t q = 0; q < *nLoc; q++) {
        if (locals[q][0] == lx && locals[q][1] == ly && locals[q][2] == lz) return;
    }
    locals[*nLoc][0] = lx;
    locals[*nLoc][1] = ly;
    locals[*nLoc][2] = lz;
    (*nLoc)++;
}

@implementation RDNA4ComputeEncoder (RDNA4IndirectCommands)
- (id)rdna4ICBDevice { return [_cb device]; }
- (RDNA4CommandBuffer *)rdna4ICBCommandBuffer { return _cb; }
- (RDNA4ComputePipelineState *)rdna4ICBComputePipeline { return _pso; }
- (BOOL)rdna4ICBInheritedComputeBuffersSatisfyPipeline:(id)pipeline {
    if (![pipeline isKindOfClass:[RDNA4ComputePipelineState class]]) return NO;
    RDNA4ComputePipelineState *use = [(RDNA4ComputePipelineState *)pipeline rdna4Real] ?: (RDNA4ComputePipelineState *)pipeline;
    return rdna4icb_stage_buffers_ok([use pb], [use npb], &_st, [self rdna4ICBDevice]);
}
- (BOOL)rdna4ICBExplicitComputeBuffersSatisfyRecord:(RDNA4ICBRecord *)record pipeline:(id)pipeline {
    if (![pipeline isKindOfClass:[RDNA4ComputePipelineState class]]) return NO;
    RDNA4ComputePipelineState *use = [(RDNA4ComputePipelineState *)pipeline rdna4Real] ?: (RDNA4ComputePipelineState *)pipeline;
    id device = [self rdna4ICBDevice];
    RDNA4StageState kernel = {0};
    for (NSNumber *slot in record.kernelBinds) {
        NSUInteger index = slot.unsignedIntegerValue;
        RDNA4ICBBind *bind = record.kernelBinds[slot];
        if (index >= 31 || !bind.buffer || [bind.buffer device] != device ||
            bind.offset > bind.buffer.length)
            return NO;
        rdna4_stage_set_buf(&kernel, index, bind.buffer, bind.offset);
    }
    return rdna4icb_stage_buffers_ok([use pb], [use npb], &kernel, device);
}
- (void)rdna4ICBClearExplicitComputeBuffers {
    for (NSUInteger i = 0; i < 31; ++i) {
        _st.buf[i].b = nil;
        _st.buf[i].off = 0;
        [self rdna4OwnResource:nil kind:RDNA4_RESOURCE_BUFFER fragment:NO index:i];
    }
    _st.roBufMask = 0;
}
- (void)rdna4ICBResetPerSlotComputeState {
    _threadgroupBytes = 0;
    memset(_tgMemLengths, 0, sizeof _tgMemLengths);
    _stageInRegion = MTLRegionMake3D(0, 0, 0, 0, 0, 0);
    _hasStageInRegion = NO;
}
- (void)rdna4ICBRetainResource:(id)resource {
    [_cb rdna4RetainEncoded:resource];
}
- (void)rdna4ExecuteComputeICBGPU:(RDNA4IndirectCommandBuffer *)icb
                      directRange:(NSRange)range
                   indirectBuffer:(id)indirectBuf
                   indirectOffset:(NSUInteger)indirectOff
                      useIndirect:(BOOL)useIndirect {
    if (!vkCmdDispatchIndirect || !RDNA4R.bufferAddress) {
        [_cb rdna4Fail:@"GPU compute ICB execution requires vkCmdDispatchIndirect and bufferDeviceAddress"];
        return;
    }
    id dev = [_cb device];
    if ([icb device] != dev) {
        [_cb rdna4Fail:@"indirect command buffer belongs to a different device"];
        return;
    }
    if (!icb->_isCompute) {
        [_cb rdna4Fail:@"cannot execute a render indirect command buffer on a compute encoder"];
        return;
    }
    if (useIndirect) {
        if (![indirectBuf isKindOfClass:[RDNA4Buffer class]] || [(RDNA4Buffer *)indirectBuf device] != dev ||
            (indirectOff & 3) || indirectOff > [(RDNA4Buffer *)indirectBuf length] ||
            8 > [(RDNA4Buffer *)indirectBuf length] - indirectOff) {
            [_cb rdna4Fail:@"GPU compute ICB indirect range buffer, alignment or range is invalid"];
            return;
        }
        if (!rdna4nc_vk_offset_ok([(RDNA4Buffer *)indirectBuf rdna4VkOffset] + indirectOff, 4, RDNA4NC_ALIGN_INDIRECT)) {
            [_cb rdna4Fail:@"GPU compute ICB indirect range Vulkan offset must be a multiple of 4"];
            return;
        }
    } else {
        NSString *rangeProblem = rdna4icb_range_problem(range, icb->_commandCount, YES);
        if (rangeProblem) {
            [_cb rdna4Fail:[NSString stringWithFormat:@"compute executeCommandsInBuffer [%lu,+%lu): %@",
                            (unsigned long)range.location, (unsigned long)range.length, rangeProblem]];
            return;
        }
        if (!range.length) {
            [_cb rdna4RetainEncoded:icb];
            id bk = [icb rdna4GPUBacking];
            if (bk) [_cb rdna4RetainEncoded:bk];
            return;
        }
    }
    if (![icb rdna4EnsureGPUBackingAddress]) {
        [_cb rdna4Fail:@"cannot allocate GPU backing for compute indirect command buffer"];
        return;
    }
    NSError *syncErr = nil;
    if (![icb rdna4SyncCPUWritesToGPU:&syncErr]) {
        [_cb rdna4Fail:syncErr.localizedDescription ?: @"cannot synchronize compute ICB CPU writes to GPU"];
        return;
    }

    uint32_t cpuTgs[32][3];
    uint32_t nCpuTgs = 0;
    pthread_mutex_lock(&icb->_mu);
    NSString *fatal = icb->_fatalError;
    RDNA4Buffer *icbBacking = icb->_gpuBacking;
    NSUInteger maxCommands = icb->_commandCount;
    BOOL inheritPipe = icb->_inheritPipelineState;
    BOOL inheritBufs = icb->_inheritBuffers;
    NSArray *rawCands = inheritPipe ? nil : [icb->_candidatePipelines copy];
    NSArray *retainedRes = [icb->_gpuRetained allObjects];
    for (RDNA4ICBRecord *rec in icb->_records.allValues) {
        if ((rec.kind == RDNA4ICBCommandConcurrentDispatch || rec.kind == RDNA4ICBCommandConcurrentDispatchThreads) &&
            rec.threadgroupSize.width && rec.threadgroupSize.height && rec.threadgroupSize.depth && nCpuTgs < 32u) {
            uint32_t lx = (uint32_t)rec.threadgroupSize.width;
            uint32_t ly = (uint32_t)rec.threadgroupSize.height;
            uint32_t lz = (uint32_t)rec.threadgroupSize.depth;
            BOOL seen = NO;
            for (uint32_t t = 0; t < nCpuTgs; t++) {
                if (cpuTgs[t][0] == lx && cpuTgs[t][1] == ly && cpuTgs[t][2] == lz) { seen = YES; break; }
            }
            if (!seen) {
                cpuTgs[nCpuTgs][0] = lx; cpuTgs[nCpuTgs][1] = ly; cpuTgs[nCpuTgs][2] = lz;
                nCpuTgs++;
            }
        }
    }
    pthread_mutex_unlock(&icb->_mu);

    if (fatal) { [_cb rdna4Fail:fatal]; return; }
    if (!icbBacking) { [_cb rdna4Fail:@"compute indirect command buffer lacks GPU backing"]; return; }

    [_cb rdna4RetainEncoded:icb];
    [_cb rdna4RetainEncoded:icbBacking];
    for (id res in retainedRes) [_cb rdna4RetainEncoded:res];
    if (useIndirect) [_cb rdna4RetainEncoded:indirectBuf];

    uint32_t maxOutSlots = useIndirect ? (uint32_t)MIN(maxCommands, (NSUInteger)RDNA4ICB_MAX_EXECUTION_RANGE) : (uint32_t)range.length;
    if (!maxOutSlots) return;

    NSMutableArray<RDNA4ComputePipelineState *> *cands = [NSMutableArray array];
    uint64_t parentToken = 0;
    BOOL fromExplicitCands = YES;
    if (inheritPipe) {
        if (!_pso) { [_cb rdna4Fail:@"compute executeCommandsInBuffer with inheritPipelineState requires a bound compute pipeline"]; return; }
        RDNA4ComputePipelineState *use = [_pso rdna4Real] ?: _pso;
        if (![use supportIndirectCommandBuffers] || ![use rdna4ICBToken]) {
            [_cb rdna4Fail:@"inherited compute pipeline does not support indirect command buffers"];
            return;
        }
        parentToken = [use rdna4ICBToken];
        [cands addObject:use];
    } else {
        if (!rawCands.count) {
            rawCands = rdna4_icb_compute_reg_for_device(dev);
            fromExplicitCands = NO;
        }
        for (id raw in rawCands) {
            if (![raw isKindOfClass:[RDNA4ComputePipelineState class]]) {
                if (fromExplicitCands) {
                    [_cb rdna4Fail:@"candidate pipeline is not a compute pipeline"];
                    return;
                }
                continue;
            }
            RDNA4ComputePipelineState *use = [(RDNA4ComputePipelineState *)raw rdna4Real] ?: (RDNA4ComputePipelineState *)raw;
            if (![use supportIndirectCommandBuffers] || ![use rdna4ICBToken] || [use noop]) {
                if (fromExplicitCands) {
                    [_cb rdna4Fail:@"candidate compute pipeline does not support indirect command buffers"];
                    return;
                }
                continue;
            }
            BOOL dup = NO;
            uint64_t tok = [use rdna4ICBToken];
            for (RDNA4ComputePipelineState *ex in cands) {
                if ([ex rdna4ICBToken] == tok) { dup = YES; break; }
            }
            if (!dup && cands.count < RDNA4_ICB_MAX_CANDIDATE_PIPELINES) [cands addObject:use];
        }
        if (!cands.count) return;
    }

    for (RDNA4ComputePipelineState *P in cands) [_cb rdna4RetainEncoded:P];

    rdna4dc_caps caps = rdna4_device_caps();
    rdna4_icb_compute_expand_cfg_t cfg = {0};
    uint32_t bucketCand[RDNA4_ICB_MAX_COMPUTE_BUCKETS];
    VkPipeline bucketVkPipe[RDNA4_ICB_MAX_COMPUTE_BUCKETS];
    uint32_t nb = 0;

    static const uint32_t stdLocalsFull[][3] = {
        {1,1,1}, {4,1,1}, {8,1,1}, {16,1,1}, {32,1,1}, {64,1,1}, {128,1,1}, {256,1,1},
        {8,8,1}, {16,16,1}
    };
    static const uint32_t stdLocalsCompact[][3] = {
        {1,1,1}, {32,1,1}, {64,1,1}, {128,1,1}, {256,1,1}, {8,8,1}
    };
    uint64_t maxBucketsByDispatches = RDNA4_ICB_MAX_GPU_EXPANDED_DISPATCHES / (uint64_t)maxOutSlots;
    if (maxBucketsByDispatches > RDNA4_ICB_MAX_COMPUTE_BUCKETS) maxBucketsByDispatches = RDNA4_ICB_MAX_COMPUTE_BUCKETS;
    if (maxBucketsByDispatches < (uint64_t)cands.count) {
        [_cb rdna4Fail:@"GPU compute ICB expanded dispatch count exceeds limit"];
        return;
    }
    uint32_t maxPerCand = (uint32_t)(maxBucketsByDispatches / (uint64_t)cands.count);
    if (!maxPerCand) maxPerCand = 1u;

    for (uint32_t ci = 0; ci < (uint32_t)cands.count; ci++) {
        RDNA4ComputePipelineState *P = cands[ci];
        NSError *prepErr = nil;
        if (![P rdna4PrepareICBGPUVariant:&prepErr]) {
            if (fromExplicitCands) {
                [_cb rdna4Fail:prepErr.localizedDescription ?: @"cannot prepare GPU compute ICB pipeline variant"];
                return;
            }
            continue;
        }
        uint64_t tok = [P rdna4ICBToken];
        const uint32_t *nom = [P nominalLocal];
        uint32_t candLocals[48][3];
        uint32_t nLoc = 0;
        rdna4_icb_add_local(&caps, candLocals, &nLoc, maxPerCand, nom[0], nom[1], nom[2]);
        if ([P mode] == 1) {
            for (uint32_t t = 0; t < nCpuTgs; t++)
                rdna4_icb_add_local(&caps, candLocals, &nLoc, maxPerCand, cpuTgs[t][0], cpuTgs[t][1], cpuTgs[t][2]);
            uint32_t compiled[32][3];
            uint32_t nComp = [P rdna4CopyCompiledLocals:compiled maxCount:32u];
            for (uint32_t t = 0; t < nComp; t++)
                rdna4_icb_add_local(&caps, candLocals, &nLoc, maxPerCand, compiled[t][0], compiled[t][1], compiled[t][2]);
            if (cands.count <= 16u) {
                for (size_t s = 0; s < sizeof(stdLocalsFull) / sizeof(stdLocalsFull[0]); s++)
                    rdna4_icb_add_local(&caps, candLocals, &nLoc, maxPerCand, stdLocalsFull[s][0], stdLocalsFull[s][1], stdLocalsFull[s][2]);
            } else {
                for (size_t s = 0; s < sizeof(stdLocalsCompact) / sizeof(stdLocalsCompact[0]); s++)
                    rdna4_icb_add_local(&caps, candLocals, &nLoc, maxPerCand, stdLocalsCompact[s][0], stdLocalsCompact[s][1], stdLocalsCompact[s][2]);
            }
        }
        for (uint32_t li = 0; li < nLoc; li++) {
            VkPipeline vkp = [P icbPipelineForLocal:candLocals[li]];
            if (!vkp) {
                if (li == 0 && fromExplicitCands) {
                    [_cb rdna4Fail:@"cannot build GPU compute ICB pipeline variant"];
                    return;
                }
                continue;
            }
            if (nb >= RDNA4_ICB_MAX_COMPUTE_BUCKETS) {
                [_cb rdna4Fail:@"GPU compute ICB bucket overflow"];
                return;
            }
            uint32_t bflags = (li == 0 && [P rdna4ICBAllowFallback]) ? RDNA4_ICB_COMPUTE_BUCKET_ALLOW_FALLBACK : 0u;
            cfg.buckets[nb] = (rdna4_icb_compute_bucket_t){
                .token = tok,
                .local_x = candLocals[li][0],
                .local_y = candLocals[li][1],
                .local_z = candLocals[li][2],
                .flags = bflags,
            };
            bucketCand[nb] = ci;
            bucketVkPipe[nb] = vkp;
            nb++;
        }
    }
    if (!nb) return;
    if ((uint64_t)maxOutSlots * (uint64_t)nb > RDNA4_ICB_MAX_GPU_EXPANDED_DISPATCHES) {
        [_cb rdna4Fail:@"GPU compute ICB expanded dispatch count exceeds limit"];
        return;
    }

    cfg.direct_location = useIndirect ? 0u : (uint32_t)range.location;
    cfg.direct_length = useIndirect ? 0u : (uint32_t)range.length;
    cfg.use_indirect_range = useIndirect ? 1u : 0u;
    cfg.max_output_slots = maxOutSlots;
    cfg.inherit_pipeline = inheritPipe ? 1u : 0u;
    cfg.inherit_buffers = inheritBufs ? 1u : 0u;
    cfg.num_buckets = nb;
    cfg.parent_pipeline_token = parentToken;
    if (inheritBufs) {
        for (uint32_t j = 0; j < 31; j++) {
            if (_st.buf[j].b && [_st.buf[j].b gpuAddress]) {
                if ([_st.buf[j].b device] != dev ||
                    !rdna4_buffer_table_set(&cfg.parent_k_table, j, [_st.buf[j].b gpuAddress],
                                            _st.buf[j].b.length, _st.buf[j].off, [_st.buf[j].b rdna4HostReadOnly])) {
                    [_cb rdna4Fail:@"parent compute buffer slot is invalid for ICB execution"];
                    return;
                }
                [_cb rdna4RetainEncoded:_st.buf[j].b];
            }
        }
    }

    RDNA4Buffer *cfgBuf = nil; NSUInteger cfgOff = 0;
    if (![_cb rdna4Bytes:&cfg length:sizeof cfg buffer:&cfgBuf offset:&cfgOff]) return;
    RDNA4Buffer *dummyRangeBuf = nil; NSUInteger dummyRangeOff = 0;
    if (!useIndirect) {
        uint32_t zeroRange[2] = { 0u, 0u };
        if (![_cb rdna4Bytes:zeroRange length:sizeof zeroRange buffer:&dummyRangeBuf offset:&dummyRangeOff]) return;
    }

    NSUInteger dispatchesBytes = (NSUInteger)maxOutSlots * (NSUInteger)nb * sizeof(VkDispatchIndirectCommand);
    NSUInteger tblBytes = (NSUInteger)maxOutSlots * RDNA4_ICB_TABLE_V2_STRIDE;
    MTLResourceOptions privOpts = MTLResourceStorageModePrivate | (RDNA4P.on ? RDNA4_OPT_NOZERO : 0);
    NSError *bErr = nil;
    RDNA4Buffer *outDispatches = [[RDNA4Buffer alloc] initWithDevice:dev length:dispatchesBytes options:privOpts error:&bErr];
    RDNA4Buffer *outKTable = [[RDNA4Buffer alloc] initWithDevice:dev length:tblBytes options:privOpts error:&bErr];
    if (!outDispatches || !outKTable) {
        [_cb rdna4Fail:bErr.localizedDescription ?: @"cannot allocate GPU compute ICB expansion buffers"];
        return;
    }
    [_cb rdna4RetainEncoded:cfgBuf];
    if (dummyRangeBuf) [_cb rdna4RetainEncoded:dummyRangeBuf];
    [_cb rdna4RetainEncoded:outDispatches];
    [_cb rdna4RetainEncoded:outKTable];

    VkDescriptorSetLayout expDsl = VK_NULL_HANDLE;
    VkPipelineLayout expPl = VK_NULL_HANDLE;
    VkPipeline expPipe = VK_NULL_HANDLE;
    NSError *expErr = nil;
    if (!rdna4_icb_get_compute_expander(&expDsl, &expPl, &expPipe, &expErr)) {
        [_cb rdna4Fail:expErr.localizedDescription ?: @"cannot build GPU compute ICB expander pipeline"];
        return;
    }

    if ([_cb rdna4HasArgumentWrites]) {
        [_cb rdna4ArgumentCheckpoint];
        if ([_cb rdna4Error]) return;
    }
    [self rdna4SnapshotResources];

    VkCommandBuffer cmd = [_cb vk];
    rdna4_full_barrier(cmd);
    VkDescriptorSet expSet = [_cb rdna4AllocSet:expDsl];
    if (!expSet) return;
    RDNA4Buffer *rangeSrc = useIndirect ? (RDNA4Buffer *)indirectBuf : dummyRangeBuf;
    NSUInteger rangeSrcOff = useIndirect ? indirectOff : dummyRangeOff;
    VkDescriptorBufferInfo expBi[5] = {
        { [icbBacking vkBuffer], [icbBacking rdna4VkOffset], rdna4_icb_backing_bytes((uint32_t)maxCommands) },
        { [cfgBuf vkBuffer], [cfgBuf rdna4VkOffset] + cfgOff, sizeof cfg },
        { [rangeSrc vkBuffer], [rangeSrc rdna4VkOffset] + rangeSrcOff, 8u },
        { [outDispatches vkBuffer], [outDispatches rdna4VkOffset], dispatchesBytes },
        { [outKTable vkBuffer], [outKTable rdna4VkOffset], tblBytes },
    };
    VkWriteDescriptorSet expW[5];
    for (uint32_t i = 0; i < 5; i++) {
        expW[i] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = expSet,
            .dstBinding = i,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &expBi[i],
        };
    }
    vkUpdateDescriptorSets(RDNA4R.dev, 5, expW, 0, NULL);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, expPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, expPl, 0, 1, &expSet, 0, NULL);
    vkCmdDispatch(cmd, (maxOutSlots + 63u) / 64u, 1, 1);
    rdna4_full_barrier(cmd);

    void (^needTex)(RDNA4Texture *, VkImageLayout, BOOL) = ^(RDNA4Texture *t, VkImageLayout l, BOOL writable) {
        rdna4_ios_usek(self->_cb, t, YES, writable, RDNA4DF_W_COMPUTE);
        rdna4_tex_to([self->_cb vk], t, l);
    };

    VkDescriptorSet candSet0[RDNA4_ICB_MAX_CANDIDATE_PIPELINES] = {0};
    VkDescriptorSet candArgSets[RDNA4_ICB_MAX_CANDIDATE_PIPELINES][3] = {{0}};
    BOOL candHasArgs[RDNA4_ICB_MAX_CANDIDATE_PIPELINES] = {NO};

    for (uint32_t ci = 0; ci < (uint32_t)cands.count; ci++) {
        RDNA4ComputePipelineState *P = cands[ci];
        if (![P icbDsl]) continue;
        MTLComputePipelineReflection *reflection = [P reflection] ?: _roReflection;
        rdna4_each_tex([P icbPb], [P icbNpb], &_st, dev, needTex);

        NSMutableSet *argumentResources = [_argumentResources mutableCopy] ?: [NSMutableSet set];
        NSMutableSet *argumentWritable = [_argumentWritable mutableCopy] ?: [NSMutableSet set];
        rdna4_argument_stage_resources(argumentResources, argumentWritable, [P icbPb], [P icbNpb], &_st,
                                       rdna4nc_stage_arguments(reflection, RDNA4NC_COMPUTE));
        RDNA4ArgumentScanPrepared scan = {0};
        const RDNA4StageState *scanStages[] = { &_st };
        NSString *scanError = nil;
        if (!rdna4_argument_scan_prepare(&scan, scanStages, 1, argumentWritable, dev, &scanError)) {
            [_cb rdna4Fail:scanError ?: @"argument scan preparation failed"];
            return;
        }
        BOOL argumentsReady = [_cb rdna4PrepareArguments:[P argumentTables]
                                               resources:argumentResources
                                                writable:argumentWritable
                                                    scan:&scan
                                                textures:needTex];
        rdna4_argument_scan_prepared_destroy(&scan);
        if (!argumentsReady) return;
        if ([P argumentTables]->active) {
            candHasArgs[ci] = YES;
            [_cb rdna4CopyArgumentSets:candArgSets[ci]];
        }

        candSet0[ci] = [_cb rdna4AllocSet:[P icbDsl]];
        if (!candSet0[ci]) return;
        if ([P icbNpb] && ![_cb rdna4FillSet:candSet0[ci] bindings:[P icbPb] count:[P icbNpb] state:&_st stage:"icb-compute"]) return;
        VkDescriptorBufferInfo dynBi = { [outKTable vkBuffer], [outKTable rdna4VkOffset], sizeof(rdna4_buffer_table) };
        VkWriteDescriptorSet dynW = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = candSet0[ci],
            .dstBinding = RDNA4_ICB_TABLE_BINDING,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,
            .pBufferInfo = &dynBi,
        };
        vkUpdateDescriptorSets(RDNA4R.dev, 1, &dynW, 0, NULL);
    }

    VkPipeline curPipe = VK_NULL_HANDLE;
    uint32_t curArgCand = UINT32_MAX;
    for (uint32_t k = 0; k < maxOutSlots; k++) {
        if (k > 0) [self rdna4ICBComputeBarrier];
        for (uint32_t b = 0; b < nb; b++) {
            uint32_t ci = bucketCand[b];
            RDNA4ComputePipelineState *P = cands[ci];
            VkPipeline vkp = bucketVkPipe[b];
            if (vkp != curPipe) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vkp);
                curPipe = vkp;
                curArgCand = UINT32_MAX;
            }
            VkPipelineLayout pl = [P icbLayout];
            if (candHasArgs[ci] && curArgCand != ci) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 2, 3, candArgSets[ci], 0, NULL);
                curArgCand = ci;
            }
            uint32_t dyn0 = k * RDNA4_ICB_TABLE_V2_STRIDE;
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &candSet0[ci], 1, &dyn0);
            VkDeviceSize cmdOff = [outDispatches rdna4VkOffset] + ((VkDeviceSize)k * (VkDeviceSize)nb + (VkDeviceSize)b) * sizeof(VkDispatchIndirectCommand);
            vkCmdDispatchIndirect(cmd, [outDispatches vkBuffer], cmdOff);
        }
    }
    rdna4_full_barrier(cmd);
    [_cb rdna4ArgumentCheckpoint];
}
@end
#endif

#ifndef GARNET_METAL_INTERNAL_H
@interface RDNA4BlitEncoder (RDNA4IndirectCommands)
@end
#endif

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
#ifdef VK_VERSION_1_0
    [_cb rdna4RetainEncoded:buffer];
#else
    [_cb rdna4Retain:buffer];
#endif
    NSString *problem = nil;
    if (![(RDNA4IndirectCommandBuffer *)buffer rdna4ValidateRange:range reason:&problem])
        [_cb rdna4Fail:[NSString stringWithFormat:@"optimizeIndirectCommandBuffer [%lu,+%lu): %@",
                        (unsigned long)range.location, (unsigned long)range.length, problem]];
    // The CPU representation has no driver command stream to reorder.
}
@end

#ifndef GARNET_METAL_INTERNAL_H
@interface GarnetDevice (RDNA4IndirectCommandBuffers)
- (id<MTLIndirectCommandBuffer>)newIndirectCommandBufferWithDescriptor:(MTLIndirectCommandBufferDescriptor *)descriptor
                                                       maxCommandCount:(NSUInteger)maxCommandCount
                                                                options:(MTLResourceOptions)options;
@end
#endif

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
        RDNA4LOG_WARN("newIndirectCommandBufferWithDescriptor: refused: %s", error.localizedDescription.UTF8String);
        return nil;
    }
    if ([buffer rdna4IsComputeICB]) {
        NSUInteger maxKernelBinds = 0;
        (void)rdna4icb_read_uint(descriptor, @selector(maxKernelBufferBindCount), &maxKernelBinds);
        RDNA4LOG("newIndirectCommandBufferWithDescriptor: %lu compute command(s), %lu bytes, commandTypes 0x%lx inheritPipeline %d inheritBuffers %d kernelBindCount %lu",
                 (unsigned long)maxCommandCount, (unsigned long)buffer.size,
                 (unsigned long)descriptor.commandTypes, descriptor.inheritPipelineState,
                 descriptor.inheritBuffers, (unsigned long)maxKernelBinds);
    } else {
        RDNA4LOG("newIndirectCommandBufferWithDescriptor: %lu command(s), %lu bytes, commandTypes 0x%lx inheritPipeline %d inheritBuffers %d bind counts %lu/%lu",
                 (unsigned long)maxCommandCount, (unsigned long)buffer.size,
                 (unsigned long)descriptor.commandTypes, descriptor.inheritPipelineState,
                 descriptor.inheritBuffers, (unsigned long)descriptor.maxVertexBufferBindCount,
                 (unsigned long)descriptor.maxFragmentBufferBindCount);
    }
    return (id<MTLIndirectCommandBuffer>)buffer;
}
@end


#endif
