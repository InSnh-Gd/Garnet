// ---------------------------------------------------------------------------------------------------------------
// RDNA4ComputeEncoder (11e R2)
// ---------------------------------------------------------------------------------------------------------------
@interface RDNA4ComputeEncoder : _MTLCommandEncoder { RDNA4CommandBuffer *_cb; BOOL _ended; NSString *_lbl; RDNA4ComputePipelineState *_pso; MTLComputePipelineReflection *_roReflection; RDNA4StageState _st; uint32_t _roWarn; NSMutableSet *_argumentResources, *_argumentWritable, *_samplerBindings; RDNA4ResourceBindings *_resourceBindings; NSUInteger _threadgroupBytes; }
- (instancetype)initWithCommandBuffer:(id)cb compute:(int)dummy;
@end

@implementation RDNA4ComputeEncoder
- (instancetype)initWithCommandBuffer:(id)cb compute:(int)dummy {
    (void)dummy;
    self = [super initWithCommandBuffer:cb]; if (!self) return nil;
    _cb = cb;
    _resourceBindings = [RDNA4ResourceBindings new];
    RDNA4LOGR("RDNA4ComputeEncoder %p", (__bridge void *)self);
    return self;
}
RDNA4_DNR(RDNA4ComputeEncoder)
- (void)rdna4OwnResource:(id)object kind:(uint32_t)kind
                fragment:(BOOL)fragment index:(NSUInteger)index {
    [_resourceBindings setObject:object
                            slot:rdna4_resource_slot(kind, fragment, index)];
}
- (void)rdna4SnapshotResources {
    [_resourceBindings snapshotToCommandBuffer:_cb];
}
- (NSString *)label { return _lbl; }
- (void)setLabel:(NSString *)l { _lbl = [l copy]; }
- (NSUInteger)getType { return 2; }
- (NSUInteger)dispatchType { return 0; }
- (void)setComputePipelineState:(id)p {
    if (![p isKindOfClass:[RDNA4ComputePipelineState class]]) { [_cb rdna4Fail:@"setComputePipelineState: not an RDNA4ComputePipelineState"]; return; }
    _pso = p; _roReflection = [_pso reflection];
    _st.scanPlan = [_pso scanPlan];
    [self rdna4OwnResource:p kind:RDNA4_RESOURCE_PIPELINE
                  fragment:NO index:0];
}
- (void)setBuffer:(id)b offset:(NSUInteger)o atIndex:(NSUInteger)i {
    if (i >= 31 || (b && ![b isKindOfClass:[RDNA4Buffer class]])) { [_cb rdna4Fail:@"compute setBuffer: bad index or class"]; return; }
    _st.buf[i].b = b; _st.buf[i].off = o;
    [self rdna4OwnResource:b kind:RDNA4_RESOURCE_BUFFER fragment:NO index:i];
}
- (void)setBufferOffset:(NSUInteger)o atIndex:(NSUInteger)i { if (i < 31) _st.buf[i].off = o; }
- (void)setBuffers:(const id __unsafe_unretained *)b offsets:(const NSUInteger *)o withRange:(NSRange)r { for (NSUInteger k = 0; k < r.length; k++) [self setBuffer:b[k] offset:o[k] atIndex:r.location + k]; }
- (void)setBytes:(const void *)p length:(NSUInteger)n atIndex:(NSUInteger)i {
    if (i >= 31 || !p) { [_cb rdna4Fail:@"compute setBytes: bad index/bytes"]; return; }
    RDNA4Buffer *b = nil; NSUInteger off = 0;
    if ([_cb rdna4Bytes:p length:n buffer:&b offset:&off]) [self setBuffer:b offset:off atIndex:i];
}
- (void)setTexture:(id)t atIndex:(NSUInteger)i {
    if (i >= 128 || (t && ![t isKindOfClass:[RDNA4Texture class]])) { [_cb rdna4Fail:@"compute setTexture: bad index or class"]; return; }
    _st.tex[i] = t;
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
    if (index == 0) _threadgroupBytes = length;
    else RDNA4LOG("compute setThreadgroupMemoryLength at index %lu is unsupported", (unsigned long)index);
}
- (void)memoryBarrierWithScope:(NSUInteger)s { (void)s; rdna4_full_barrier([_cb vk]); }
- (void)memoryBarrierWithResources:(const id __unsafe_unretained *)r count:(NSUInteger)n { (void)r; (void)n; rdna4_full_barrier([_cb vk]); }
- (void)updateFence:(id)f {
    if (_ended) { [_cb rdna4Fail:@"compute updateFence after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    [_cb rdna4UpdateFence:f];
}
- (void)waitForFence:(id)f {
    if (_ended) { [_cb rdna4Fail:@"compute waitForFence after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    [_cb rdna4WaitForFence:f];
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
          [self rdna4OwnResource:r kind:RDNA4_RESOURCE_PIPELINE
                        fragment:NO index:0];
          RDNA4LOGR("HOT-SWAP: compute encoder now uses the real pipeline %p", (__bridge void *)r);
      } }
    MTLComputePipelineReflection *reflection = [_pso reflection] ?: _roReflection;
    [self rdna4SnapshotResources];
    if (rdna4nc_stage_writes(&_st, rdna4nc_stage_arguments(reflection, RDNA4NC_COMPUTE), RDNA4NC_COMPUTE, "compute dispatch", &_roWarn)) {
        [_cb rdna4Fail:@"compute dispatch writes a read-only no-copy buffer"];
        return;
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
    NSMutableSet *argumentResources = [_argumentResources mutableCopy] ?: [NSMutableSet set];
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
    BOOL argumentsReady = [_cb rdna4PrepareArguments:[_pso argumentTables]
        resources:argumentResources writable:argumentWritable scan:&scan
        textures:^(RDNA4Texture *t, VkImageLayout layout, BOOL writable) {
        rdna4_ios_usek(self->_cb, t, YES, writable, RDNA4DF_W_COMPUTE);
        rdna4_tex_to([self->_cb vk], t, layout);
    }];
    rdna4_argument_scan_prepared_destroy(&scan);
    if (!argumentsReady) return;
    rdna4_each_tex([_pso pb], [_pso npb], &_st, dev, ^(RDNA4Texture *t, VkImageLayout l, BOOL writable) { rdna4_ios_usek(self->_cb, t, YES, writable, RDNA4DF_W_COMPUTE); rdna4_tex_to([self->_cb vk], t, l); });
    VkCommandBuffer cmd = [_cb vk];
    VkDescriptorSet s = VK_NULL_HANDLE;
    if ([_pso npb]) { s = [_cb rdna4AllocSet:[_pso dsl]]; if (!s || ![_cb rdna4FillSet:s bindings:[_pso pb] count:[_pso npb] state:&_st stage:"compute"]) return; }
    [_cb rdna4BindArguments:[_pso argumentTables] point:VK_PIPELINE_BIND_POINT_COMPUTE layout:[_pso layout]];
    if (indirect || [_pso mode] == 0) {   // Whole workgroups, with CPU or GPU dispatch counts.
        if (exact && (g[0] % nom[0] || g[1] % nom[1] || g[2] % nom[2])) { [_cb rdna4Fail:@"dispatchThreads with a partial boundary threadgroup needs a ThreadsDynamic translation (this module is Workgroups)"]; return; }
        VkPipeline pipeline = [_pso pipelineForLocal:nom indirect:indirect != nil threadgroupBytes:threadgroupBytes];
        if (!pipeline) { [_cb rdna4Fail:@"cannot build a compute pipeline variant"]; return; }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        if (s) vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, [_pso layout], 0, 1, &s, 0, NULL);
        if (indirect) {
            rdna4_full_barrier(cmd);
            vkCmdDispatchIndirect(cmd, [indirect vkBuffer], [indirect rdna4VkOffset] + offset);
        } else vkCmdDispatch(cmd, exact ? g[0] / nom[0] : g[0], exact ? g[1] / nom[1] : g[1], exact ? g[2] / nom[2] : g[2]);
    } else {                  // ThreadsDynamic: the metal2vulkan region plan, one dispatch per region
        RDNA4Region reg[8]; uint32_t threads[3], tgpg[3];
        for (int d = 0; d < 3; d++) { threads[d] = exact ? g[d] : g[d] * nom[d]; tgpg[d] = (threads[d] + nom[d] - 1) / nom[d]; }
        int nr = rdna4_plan(threads, nom, reg);
        if (s) vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, [_pso layout], 0, 1, &s, 0, NULL);
        for (int i = 0; i < nr; i++) {
            VkPipeline p = [_pso pipelineForLocal:reg[i].local indirect:NO threadgroupBytes:threadgroupBytes]; if (!p) { [_cb rdna4Fail:@"cannot build a compute pipeline variant"]; return; }
            uint32_t words[12] = { threads[0], threads[1], threads[2], reg[i].tbase[0], reg[i].tbase[1], reg[i].tbase[2], reg[i].gbase[0], reg[i].gbase[1], reg[i].gbase[2], tgpg[0], tgpg[1], tgpg[2] };
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p);
            vkCmdPushConstants(cmd, [_pso layout], VK_SHADER_STAGE_COMPUTE_BIT, [_pso pcOffset], sizeof words, words);
            vkCmdDispatch(cmd, reg[i].groups[0], reg[i].groups[1], reg[i].groups[2]);
        }
        RDNA4LOGR("compute dispatch: threads %u,%u,%u local %u,%u,%u -> %d region(s)", threads[0], threads[1], threads[2], nom[0], nom[1], nom[2], nr);
    }
    rdna4_full_barrier(cmd);
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
- (void)endEncoding {
    if (_ended) return;
    _ended = YES;
    rdna4_full_barrier([_cb vk]);
    [_cb rdna4ArgumentCheckpoint];
    RDNA4LOGR("RDNA4ComputeEncoder endEncoding");
    [super endEncoding];
}
@end

