// ---------------------------------------------------------------------------------------------------------------
// RDNA4ComputePipelineState (11e R2): SPIR-V + meta sidecar from spvcache; ThreadsDynamic modules take the local size as spec ids 0..2.
// ---------------------------------------------------------------------------------------------------------------
@interface RDNA4ComputePipelineState : NSObject {
    id _dev; NSString *_lbl; VkShaderModule _sm; VkPipelineLayout _pl; VkDescriptorSetLayout _dsl; RDNA4PB *_pb; uint32_t _npb; uint32_t _pcSize;
    uint32_t _local[3]; int _mode; uint32_t _pcOff; char _ep[128]; NSMutableDictionary *_variants; NSLock *_lock; NSMutableArray *_owned; BOOL _noop;
    id _hsFn; _Atomic(uintptr_t) _realp;   // hot-swap (placeholder only): the kernel function to rebuild from; the published real object (+1, written once)
    RDNA4LinkCtx *_hsLk;   // build 18 (P3): the kernel's link context at creation (nil = none); the rebuild on the watcher thread installs it
    MTLComputePipelineReflection *_reflection;
    NSData *_spv;
    BOOL _lumaWorkgroup;
    RDNA4ArgumentTables _argumentTables;
    rdna4_argument_scan_reflection_plan _scan;
    VkDescriptorSetLayout _emptySet;
}
- (instancetype)initWithDevice:(id)dev function:(id)fn error:(NSError **)err;
- (instancetype)initPlaceholderWithDevice:(id)dev function:(id)fn;   // #12 R1: valid state whose dispatches encode nothing (hot-swapped when the .spv + .meta.json appear)
- (RDNA4ComputePipelineState *)rdna4Real;
- (BOOL)rdna4HotRebuild;
- (BOOL)noop;
- (VkPipeline)pipelineForLocal:(const uint32_t *)l;
- (VkPipeline)pipelineForLocal:(const uint32_t *)l indirect:(BOOL)indirect;
- (VkPipeline)pipelineForLocal:(const uint32_t *)l indirect:(BOOL)indirect threadgroupBytes:(NSUInteger)bytes;
- (BOOL)rdna4LumaWorkgroup;
- (VkPipelineLayout)layout; - (VkDescriptorSetLayout)dsl; - (const RDNA4PB *)pb; - (uint32_t)npb;
- (const RDNA4ArgumentTables *)argumentTables;
- (const rdna4_argument_scan_reflection_plan *)scanPlan;
- (const uint32_t *)nominalLocal; - (int)mode; - (uint32_t)pcOffset; - (uint32_t)pcSize;
- (id)reflection;
- (void)rdna4SetReflection:(MTLComputePipelineReflection *)reflection;
@end

@implementation RDNA4ComputePipelineState
- (instancetype)initPlaceholderWithDevice:(id)dev function:(id)fn {
    self = [super init]; if (!self) return nil;
    _dev = dev; _lock = [NSLock new]; _variants = [NSMutableDictionary dictionary]; _owned = [NSMutableArray array]; _noop = YES;
    NSString *sha = rdna4_fn_sha(fn);
    if (sha) {
        _hsFn = fn; _hsLk = rdna4_lk_for(fn);
        rdna4hs_register(self, @[ sha ], YES, [NSString stringWithFormat:@"kernel %@", [fn respondsToSelector:@selector(name)] ? [fn name] : @"?"], ^BOOL(id o) { return [(RDNA4ComputePipelineState *)o rdna4HotRebuild]; });
    }
    return self;
}
- (RDNA4ComputePipelineState *)rdna4Real { uintptr_t p = rdna4hs_load(&_realp); return p ? (__bridge RDNA4ComputePipelineState *)(void *)p : nil; }
// Runs on the watcher queue: a NEW pipeline object through the normal init (a miss or an unusable .meta.json fails it, nothing is published).
- (BOOL)rdna4HotRebuild {
    if (!_hsFn) return NO;
    NSError *e = nil;
    RDNA4_LK_SCOPE; if (_hsLk) rdna4_lk_install(2, _hsLk);   // build 18 (P3)
    RDNA4ComputePipelineState *r = [[RDNA4ComputePipelineState alloc] initWithDevice:_dev function:_hsFn error:&e];
    if (!r) { RDNA4LOG("HOT-SWAP kernel rebuild failed: %s", e.localizedDescription.UTF8String); return NO; }
    r->_lbl = _lbl;
    uintptr_t v = (uintptr_t)CFBridgingRetain(r);
    if (!rdna4hs_install(&_realp, v)) { CFRelease((CFTypeRef)v); return NO; }
    return YES;
}
- (BOOL)noop { return _noop; }
- (BOOL)rdna4LumaWorkgroup { return _lumaWorkgroup; }
- (instancetype)initWithDevice:(id)dev function:(id)fn error:(NSError **)err {
    self = [super init]; if (!self) return nil;
    _dev = dev; _lock = [NSLock new]; _variants = [NSMutableDictionary dictionary]; _owned = [NSMutableArray array];
    // #12 R1: the cache lookup comes first, so a miss (placeholder case) needs no Vulkan device.
    NSString *fname = nil; NSDictionary *meta = nil; NSError *e = nil;
    NSData *spv = rdna4_spv_lookup(fn, "kernel", &e, &fname, &meta);
    if (!spv) { if (err) *err = e; return nil; }
    if (!rdna4_radv_open(err)) return nil;
    const uint64_t deadline = rdna4_now() + (uint64_t)RDNA4G_SYNC_WAIT_APP_MS * 1000000ull;
    NSDictionary *preMeta = meta;
    spv = rdna4_gpu_argument_spv(spv, &meta, deadline, err);
    if (!spv) return nil;
    if (!meta || !meta[@"kernel_dispatch"] || ![meta[@"local_size"] isKindOfClass:[NSArray class]] || [meta[@"local_size"] count] != 3) {
        if (err) *err = rdna4_err(62, [NSString stringWithFormat:@"compute function '%@': spvcache has no .meta.json with kernel_dispatch/local_size (run add-air.py)", fname]); return nil; }
    id kd = meta[@"kernel_dispatch"];
    if ([kd isEqual:@"Workgroups"]) _mode = 0;
    else if ([kd isKindOfClass:[NSDictionary class]] && kd[@"ThreadsDynamic"]) { _mode = 1; _pcOff = (uint32_t)[kd[@"ThreadsDynamic"][@"offset"] unsignedIntValue]; }
    else { if (err) *err = rdna4_err(63, [NSString stringWithFormat:@"compute function '%@': kernel_dispatch %@ not supported (ThreadsFixed)", fname, kd]); return nil; }
    for (int i = 0; i < 3; i++) _local[i] = (uint32_t)[meta[@"local_size"][i] unsignedIntValue];
    RDNA4sRefl *r = calloc(1, sizeof *r); RDNA4sMod m = {0}; char why[256] = "";
    if (rdna4s_parse(spv.bytes, spv.length / 4, &m, why, sizeof why) || !m.has_ep || m.ep_model != 5 || rdna4s_reflect(&m, 2, r)) {
        if (err) *err = rdna4_err(64, [NSString stringWithFormat:@"kernel SPIR-V reflection: %s", why[0] ? why : r->why[0] ? r->why : "no/wrong entry point"]); rdna4s_free(&m); free(r); return nil; }
    strlcpy(_ep, m.ep_name, sizeof _ep);
    uint32_t *lumaWords = NULL;
    if (_mode && rdna4_luma_workgroup(fname.UTF8String, &m, spv.bytes, spv.length / 4, &lumaWords)) {
        spv = [NSData dataWithBytesNoCopy:lumaWords length:spv.length + 8 * sizeof(*lumaWords)];
        _lumaWorkgroup = YES;
    }
    rdna4s_free(&m);
    _pcSize = r->pc_size;
    BOOL ok = rdna4_stage_dsl(r, 0, VK_SHADER_STAGE_COMPUTE_BIT, meta, &_dsl, &_pb, &_npb, _owned, &e);
    if (ok) {
        _argumentTables.comparisonSamplers = [meta[@"argument_resource_tables"][@"comparison_sampler"] boolValue];
        _argumentTables.samplerCoordinates = [meta[@"normalized_sampler_coordinates"] boolValue];
        ok = rdna4_argument_table_metadata_apply(&_argumentTables, r, 0, meta, &e) &&
            rdna4_argument_table_layouts(&_argumentTables, r, NULL, YES, &e) &&
            rdna4_argument_stage_remap(&_argumentTables, _pb, _npb, &e);
    }
    free(r);
    if (!ok) { if (err) *err = e; return nil; }
    if (!rdna4_argument_scan_plan_build(&_scan, preMeta, meta, "compute", &e)) {
        if (err) *err = e;
        return nil;
    }
    _sm = rdna4_spv_module(spv.bytes, spv.length, "kernel", err); if (!_sm) return nil;
    _spv = spv;
    VkPushConstantRange pcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, _pcSize };
    if (_argumentTables.active) {
        VkDescriptorSetLayoutCreateInfo empty = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        VkResult result = vkCreateDescriptorSetLayout(RDNA4R.dev, &empty, NULL, &_emptySet);
        if (result != VK_SUCCESS) { if (err) *err = rdna4_err(49, @"cannot create the empty compute descriptor set"); return nil; }
    }
    VkDescriptorSetLayout layouts[5] = {_dsl, _emptySet, _argumentTables.dsl[0], _argumentTables.dsl[1], _argumentTables.dsl[2]};
    VkPipelineLayoutCreateInfo plc = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = _argumentTables.active ? 5 : 1, .pSetLayouts = layouts,
        .pushConstantRangeCount = _pcSize ? 1 : 0, .pPushConstantRanges = &pcr };
    VkResult vr = vkCreatePipelineLayout(RDNA4R.dev, &plc, NULL, &_pl);
    if (vr != VK_SUCCESS) { if (err) *err = rdna4_err(46, [NSString stringWithFormat:@"vkCreatePipelineLayout(compute) = %d", vr]); return nil; }
    VkPipeline p = [self pipelineForLocal:_local];
    if (!p) { if (err) *err = rdna4_err(65, @"vkCreateComputePipelines failed"); return nil; }
    RDNA4LOG("RDNA4ComputePipelineState %p '%s': local %u,%u,%u, %s, %u bindings, push constants %u B", (__bridge void *)self, fname.UTF8String, _local[0], _local[1], _local[2],
           _mode ? "ThreadsDynamic" : "Workgroups", _npb, _pcSize);
    return self;
}
- (VkPipeline)pipelineForLocal:(const uint32_t *)l {
    return [self pipelineForLocal:l indirect:NO];
}
- (VkPipeline)pipelineForLocal:(const uint32_t *)l indirect:(BOOL)indirect {
    return [self pipelineForLocal:l indirect:indirect threadgroupBytes:0];
}
- (VkPipeline)pipelineForLocal:(const uint32_t *)l indirect:(BOOL)indirect threadgroupBytes:(NSUInteger)bytes {
    rdna4dc_caps caps = rdna4_device_caps();
    if (!rdna4dc_workgroup_ok(&caps, l[0], l[1], l[2])) {
        RDNA4LOG("compute pipeline: local size %u,%u,%u exceeds device limits", l[0], l[1], l[2]); return VK_NULL_HANDLE;
    }
    uint64_t key = _mode ? ((uint64_t)l[0] | ((uint64_t)l[1] << 20) | ((uint64_t)l[2] << 40)) : 0;
    if (_mode && indirect) key |= UINT64_C(1) << 63;
    if (bytes && (!_lumaWorkgroup || !rdna4_luma_workgroup_bytes_ok(bytes, caps.shared_memory) ||
        !rdna4_luma_workgroup_fits(bytes, l))) return VK_NULL_HANDLE;
    id<NSCopying> k = bytes ? (id<NSCopying>)@[ @(key), @(bytes) ] : @(key);
    [_lock lock];
    NSValue *v = _variants[k];
    if (!v) {
        VkShaderModule module = _sm;
        if (_mode && indirect) {
            uint32_t *words = NULL; size_t count = 0; char why[256] = "";
            if (rdna4s_indirect_compute(_spv.bytes, _spv.length / 4, _pcOff, l, &words, &count, why, sizeof why)) {
                RDNA4LOG("indirect compute transformation: %s", why); [_lock unlock]; return VK_NULL_HANDLE;
            }
            module = rdna4_spv_module(words, count * 4, "indirect kernel", NULL);
            free(words);
            if (!module) { [_lock unlock]; return VK_NULL_HANDLE; }
        }
        VkSpecializationMapEntry me[] = { { 0, 0, 4 }, { 1, 4, 4 }, { 2, 8, 4 }, { RDNA4_LUMA_WORKGROUP_SPEC, 12, 4 } };
        uint32_t data[] = { l[0], l[1], l[2], (uint32_t)(bytes / RDNA4_LUMA_ELEMENT_BYTES) };
        VkSpecializationInfo si = { bytes ? 4u : 3u, me, bytes ? sizeof data : 3 * sizeof(*data), data };
        VkComputePipelineCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = _pl,
            .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = _ep, .pSpecializationInfo = _mode ? &si : NULL } };
        VkPipeline p = VK_NULL_HANDLE; uint64_t t_ = rdna4_now(); VkResult r = vkCreateComputePipelines(RDNA4R.dev, RDNA4R.pc, 1, &ci, NULL, &p); rdna4t_add(&T1.cmp, rdna4_now() - t_); atomic_fetch_add(&rdna4_pipes_new, 1);
        if (module != _sm) vkDestroyShaderModule(RDNA4R.dev, module, NULL);
        if (r != VK_SUCCESS) RDNA4LOG("vkCreateComputePipelines(local %u,%u,%u) = %d", l[0], l[1], l[2], r);
        else { v = [NSValue valueWithPointer:(void *)p]; _variants[k] = v; }
    }
    [_lock unlock];
    return v ? (VkPipeline)v.pointerValue : VK_NULL_HANDLE;
}
- (void)dealloc {
    if (RDNA4R.ok) {
        for (NSValue *v in _variants.allValues) vkDestroyPipeline(RDNA4R.dev, (VkPipeline)v.pointerValue, NULL);
        if (_pl) vkDestroyPipelineLayout(RDNA4R.dev, _pl, NULL);
        if (_dsl) vkDestroyDescriptorSetLayout(RDNA4R.dev, _dsl, NULL);
        if (_emptySet) vkDestroyDescriptorSetLayout(RDNA4R.dev, _emptySet, NULL);
        rdna4_argument_tables_destroy(&_argumentTables);
        for (NSValue *v in _owned) rdna4_destroy_sampler((VkSampler)v.pointerValue, NULL);
        if (_sm) vkDestroyShaderModule(RDNA4R.dev, _sm, NULL);
    }
    rdna4_argument_scan_reflection_plan_destroy(&_scan);
    free(_pb);
    uintptr_t rp = rdna4hs_load(&_realp); if (rp) CFRelease((CFTypeRef)rp);
}
RDNA4_DNR(RDNA4ComputePipelineState)
- (VkPipelineLayout)layout { return _pl; } - (VkDescriptorSetLayout)dsl { return _dsl; } - (const RDNA4PB *)pb { return _pb; } - (uint32_t)npb { return _npb; }
- (const RDNA4ArgumentTables *)argumentTables { return &_argumentTables; }
- (const rdna4_argument_scan_reflection_plan *)scanPlan { return &_scan; }
- (const uint32_t *)nominalLocal { return _local; } - (int)mode { return _mode; } - (uint32_t)pcOffset { return _pcOff; } - (uint32_t)pcSize { return _pcSize; }
- (id)reflection { return _reflection; }
- (void)rdna4SetReflection:(MTLComputePipelineReflection *)reflection { _reflection = reflection; }
- (id)device { return _dev; }
- (NSString *)label { return _lbl; }
- (void)setLabel:(NSString *)l { _lbl = [l copy]; }
- (NSUInteger)maxTotalThreadsPerThreadgroup { return rdna4_device_caps().invocations; }
- (NSUInteger)threadExecutionWidth { return rdna4_device_caps().subgroup_size; }
- (NSUInteger)staticThreadgroupMemoryLength { return 0; }
- (NSUInteger)imageblockMemoryLengthForDimensions:(MTLSize)d { (void)d; return 0; }
- (BOOL)supportIndirectCommandBuffers { return NO; }
@end

