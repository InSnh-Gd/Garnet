// ---------------------------------------------------------------------------------------------------------------
// RDNA4ComputePipelineState (11e R2): SPIR-V + meta sidecar from spvcache; ThreadsDynamic modules take the local size as spec ids 0..2.
// ---------------------------------------------------------------------------------------------------------------
#define RDNA4_COMPUTE_ICB_ENABLED 1

static _Atomic uint64_t rdna4_icb_compute_pso_seq;
static pthread_mutex_t rdna4_icb_compute_reg_mu = PTHREAD_MUTEX_INITIALIZER;
static NSHashTable *rdna4_icb_compute_reg;

static void rdna4_icb_compute_reg_add(id pso) {
    if (!pso) return;
    pthread_mutex_lock(&rdna4_icb_compute_reg_mu);
    if (!rdna4_icb_compute_reg) rdna4_icb_compute_reg = [NSHashTable weakObjectsHashTable];
    [rdna4_icb_compute_reg addObject:pso];
    pthread_mutex_unlock(&rdna4_icb_compute_reg_mu);
}

static NSArray *rdna4_icb_compute_reg_for_device(id dev) {
    if (!dev) return @[];
    NSMutableArray *out = [NSMutableArray array];
    pthread_mutex_lock(&rdna4_icb_compute_reg_mu);
    for (id obj in rdna4_icb_compute_reg) {
        if ([obj device] == dev && [(id<MTLComputePipelineState>)obj supportIndirectCommandBuffers]) {
            [out addObject:obj];
            if (out.count >= RDNA4_ICB_MAX_CANDIDATE_PIPELINES) break;
        }
    }
    pthread_mutex_unlock(&rdna4_icb_compute_reg_mu);
    return out;
}

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
    _Atomic uint64_t _uid;
    BOOL _icb, _icbGpuReady, _icbAllowFallback;
    uint64_t _icbToken;
    NSData *_icbSpv;
    VkDescriptorSetLayout _icbDsl;
    VkPipelineLayout _icbPl;
    RDNA4PB *_icbPb;
    uint32_t _icbNpb;
    NSMutableDictionary *_icbVariants;
    uint32_t _compiledLocals[32][3];
    uint32_t _nCompiledLocals;
    uint64_t _vftOwnerToken;
    rdna4vft_reflection_t _vft;
    uint64_t _iftOwnerToken;
    rdna4rtr_reflection_t _rt;
}
- (instancetype)initWithDevice:(id)dev function:(id)fn error:(NSError **)err;
- (instancetype)initWithDevice:(id)dev function:(id)fn spv:(NSData *)spv meta:(NSDictionary *)meta fname:(NSString *)fname error:(NSError **)err;
- (instancetype)initWithDevice:(id)dev descriptor:(MTLComputePipelineDescriptor *)descriptor options:(NSUInteger)options error:(NSError **)err;
- (instancetype)initPlaceholderWithDevice:(id)dev function:(id)fn;   // #12 R1: valid state whose dispatches encode nothing (hot-swapped when the .spv + .meta.json appear)
- (RDNA4ComputePipelineState *)rdna4Real;
- (BOOL)rdna4HotRebuild;
- (BOOL)noop;
- (VkPipeline)pipelineForLocal:(const uint32_t *)l;
- (VkPipeline)pipelineForLocal:(const uint32_t *)l indirect:(BOOL)indirect;
- (VkPipeline)pipelineForLocal:(const uint32_t *)l indirect:(BOOL)indirect threadgroupBytes:(NSUInteger)bytes;
- (BOOL)rdna4PrepareICBGPUVariant:(NSError **)err;
- (VkPipeline)icbPipelineForLocal:(const uint32_t *)l;
- (uint64_t)rdna4ICBToken;
- (BOOL)rdna4ICBAllowFallback;
- (VkPipelineLayout)icbLayout;
- (VkDescriptorSetLayout)icbDsl;
- (const RDNA4PB *)icbPb;
- (uint32_t)icbNpb;
- (uint32_t)rdna4CopyCompiledLocals:(uint32_t (*)[3])outLocals maxCount:(uint32_t)maxCount;
- (BOOL)rdna4LumaWorkgroup;
- (VkPipelineLayout)layout; - (VkDescriptorSetLayout)dsl; - (const RDNA4PB *)pb; - (uint32_t)npb;
- (const RDNA4ArgumentTables *)argumentTables;
- (const rdna4_argument_scan_reflection_plan *)scanPlan;
- (const uint32_t *)nominalLocal; - (int)mode; - (uint32_t)pcOffset; - (uint32_t)pcSize;
- (id)reflection;
- (void)rdna4SetReflection:(MTLComputePipelineReflection *)reflection;
- (const rdna4rtr_as_reflection_t *)rdna4RtAsRefl;
- (id<MTLFunctionHandle>)functionHandleWithFunction:(id<MTLFunction>)function;
- (id<MTLVisibleFunctionTable>)newVisibleFunctionTableWithDescriptor:(MTLVisibleFunctionTableDescriptor *)descriptor;
- (id<MTLIntersectionFunctionTable>)newIntersectionFunctionTableWithDescriptor:(MTLIntersectionFunctionTableDescriptor *)descriptor;
@end

@implementation RDNA4ComputePipelineState
- (instancetype)initPlaceholderWithDevice:(id)dev function:(id)fn {
    self = [super init]; if (!self) return nil;
    _dev = dev; _lock = [NSLock new]; _variants = [NSMutableDictionary dictionary]; _icbVariants = [NSMutableDictionary dictionary]; _owned = [NSMutableArray array]; _noop = YES;
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
    r->_icb = _icb;
    r->_icbToken = _icbToken;
    if (r->_icb) rdna4_icb_compute_reg_add(r);
    uintptr_t v = (uintptr_t)CFBridgingRetain(r);
    if (!rdna4hs_install(&_realp, v)) { CFRelease((CFTypeRef)v); return NO; }
    return YES;
}
- (void)rdna4EnableICBForDescriptor:(MTLComputePipelineDescriptor *)descriptor {
    if (!descriptor || ![descriptor respondsToSelector:@selector(supportIndirectCommandBuffers)] ||
        !descriptor.supportIndirectCommandBuffers)
        return;
    _icb = YES;
    if (!_icbToken) {
        uint64_t seq = (atomic_fetch_add(&rdna4_icb_compute_pso_seq, 1) % UINT64_C(0x7FFFFFFF)) + 1ull;
        _icbToken = RDNA4_ICB_PSO_TOKEN_TAG | RDNA4_ICB_COMPUTE_PSO_BIT | seq;
    }
    if (!_icbVariants) _icbVariants = [NSMutableDictionary dictionary];
    rdna4_icb_compute_reg_add(self);
}
- (BOOL)noop { return _noop; }
- (BOOL)rdna4LumaWorkgroup { return _lumaWorkgroup; }
- (instancetype)initWithDevice:(id)dev function:(id)fn error:(NSError **)err {
    // #12 R1: the cache lookup comes first, so a miss (placeholder case) needs no Vulkan device.
    NSString *fname = nil; NSDictionary *meta = nil; NSError *e = nil;
    NSData *spv = rdna4_spv_lookup(fn, "kernel", &e, &fname, &meta);
    if (!spv) { if (err) *err = e; return nil; }
    return [self initWithDevice:dev function:fn spv:spv meta:meta fname:fname error:err];
}
- (instancetype)initWithDevice:(id)dev descriptor:(MTLComputePipelineDescriptor *)descriptor options:(NSUInteger)options error:(NSError **)err {
    if (!descriptor || !descriptor.computeFunction) {
        if (err) *err = rdna4_err(61, @"compute pipeline descriptor has no computeFunction");
        return nil;
    }
    id fn = descriptor.computeFunction;
    BOOL installedLk = NO;
    if (rdna4_lk_for(fn) == nil) {
        rdna4x_linkage_compute(descriptor);
        installedLk = (rdna4_lk_for(fn) != nil);
    }
    NSError *lkErr = rdna4_lk_error(fn);
    if (lkErr) {
        if (installedLk) rdna4_lk_install(2, nil);
        if (err) *err = lkErr;
        return nil;
    }
#ifdef RDNA4_BINARY_ARCHIVE_IMPL_INC
    NSData *baSpv = nil, *baVk = nil;
    NSDictionary *baMeta = nil;
    rdna4ba_fingerprint_t baFp = {0};
    BOOL baHit = NO;
    NSError *baErr = nil;
    if (!rdna4_ba_lookup_compute(dev, descriptor, options, &baSpv, &baMeta, &baVk, &baFp, &baHit, &baErr)) {
        if (installedLk) rdna4_lk_install(2, nil);
        if (err) *err = baErr;
        return nil;
    }
    if (baHit) {
        if (!rdna4_radv_open(err)) {
            if (installedLk) rdna4_lk_install(2, nil);
            return nil;
        }
        NSString *fname = [fn respondsToSelector:@selector(name)] ? [fn name] : @"?";
        rdna4_ba_vkcache_scope_t vcScope;
        rdna4_ba_vkcache_scope_begin(&vcScope, baVk, &baFp);
        self = [self initWithDevice:dev function:fn spv:baSpv meta:baMeta fname:fname error:err];
        rdna4_ba_vkcache_scope_end(&vcScope);
        if (installedLk) rdna4_lk_install(2, nil);
        if (self) {
            if (descriptor.label) _lbl = [descriptor.label copy];
            [self rdna4EnableICBForDescriptor:descriptor];
        }
        return self;
    }
#else
    if (options & ((NSUInteger)1u << 2)) {
        if (installedLk) rdna4_lk_install(2, nil);
        if (err) *err = [NSError errorWithDomain:MTLBinaryArchiveDomain
            code:(NSInteger)MTLBinaryArchiveErrorCompilationFailure
            userInfo:@{ NSLocalizedDescriptionKey: @"Compute pipeline missed binary archive (MTLPipelineOptionFailOnBinaryArchiveMiss)" }];
        return nil;
    }
#endif
    self = [self initWithDevice:dev function:fn error:err];
    if (installedLk) rdna4_lk_install(2, nil);
    if (self) {
        if (descriptor.label) _lbl = [descriptor.label copy];
        [self rdna4EnableICBForDescriptor:descriptor];
    }
    return self;
}
- (instancetype)initWithDevice:(id)dev function:(id)fn spv:(NSData *)spv meta:(NSDictionary *)meta fname:(NSString *)fname error:(NSError **)err {
    (void)fn;
    self = [super init]; if (!self) return nil;
    _dev = dev; _lock = [NSLock new]; _variants = [NSMutableDictionary dictionary]; _icbVariants = [NSMutableDictionary dictionary]; _owned = [NSMutableArray array];
    if (!spv) { if (err) *err = rdna4_err(41, @"compute function SPIR-V is nil"); return nil; }
    if (!fname) fname = @"?";
    NSError *e = nil;
    if (!rdna4_radv_open(err)) return nil;
    const uint64_t deadline = rdna4_now() + (uint64_t)RDNA4G_SYNC_WAIT_APP_MS * 1000000ull;
    NSDictionary *preMeta = meta;
    spv = rdna4_gpu_argument_spv(spv, &meta, deadline, err);
    if (!spv) return nil;
    _vftOwnerToken = rdna4_vft_next_owner_token();
    NSDictionary *vftMeta = (meta && meta[@"visible_function_tables"]) ? meta : preMeta;
    if (!rdna4x_parse_vft_from_meta(vftMeta, &_vft, err)) return nil;
    _iftOwnerToken = rdna4_vft_next_owner_token();
    NSDictionary *rtMeta = (meta && (meta[@"acceleration_structures"] || meta[@"intersection_function_tables"])) ? meta : preMeta;
    if (!rdna4_rt_parse_meta(rtMeta, &_rt, err)) return nil;
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
        VkPipelineCache pc = RDNA4R.pc;
#ifdef RDNA4_BINARY_ARCHIVE_IMPL_INC
        pc = rdna4_ba_vkcache();
#endif
        VkPipeline p = VK_NULL_HANDLE; uint64_t t_ = rdna4_now(); VkResult r = vkCreateComputePipelines(RDNA4R.dev, pc, 1, &ci, NULL, &p); rdna4t_add(&T1.cmp, rdna4_now() - t_); atomic_fetch_add(&rdna4_pipes_new, 1);
        if (module != _sm) vkDestroyShaderModule(RDNA4R.dev, module, NULL);
        if (r != VK_SUCCESS) RDNA4LOG("vkCreateComputePipelines(local %u,%u,%u) = %d", l[0], l[1], l[2], r);
        else {
            v = [NSValue valueWithPointer:(void *)p];
            _variants[k] = v;
            if (_nCompiledLocals < 32u) {
                BOOL seen = NO;
                for (uint32_t ci = 0; ci < _nCompiledLocals; ci++) {
                    if (_compiledLocals[ci][0] == l[0] && _compiledLocals[ci][1] == l[1] && _compiledLocals[ci][2] == l[2]) {
                        seen = YES;
                        break;
                    }
                }
                if (!seen) {
                    _compiledLocals[_nCompiledLocals][0] = l[0];
                    _compiledLocals[_nCompiledLocals][1] = l[1];
                    _compiledLocals[_nCompiledLocals][2] = l[2];
                    _nCompiledLocals++;
                }
            }
        }
    }
    [_lock unlock];
    return v ? (VkPipeline)v.pointerValue : VK_NULL_HANDLE;
}
- (void)rdna4DestroyICBGPUState {
    if (RDNA4R.ok) {
        for (NSValue *v in _icbVariants.allValues) vkDestroyPipeline(RDNA4R.dev, (VkPipeline)v.pointerValue, NULL);
        if (_icbPl) vkDestroyPipelineLayout(RDNA4R.dev, _icbPl, NULL);
        if (_icbDsl) vkDestroyDescriptorSetLayout(RDNA4R.dev, _icbDsl, NULL);
    }
    [_icbVariants removeAllObjects];
    _icbPl = VK_NULL_HANDLE;
    _icbDsl = VK_NULL_HANDLE;
    free(_icbPb); _icbPb = NULL; _icbNpb = 0;
    _icbSpv = nil;
    _icbGpuReady = NO;
    _icbAllowFallback = NO;
}
- (BOOL)rdna4PrepareICBGPUVariant:(NSError **)err {
    [_lock lock];
    if (_icbGpuReady) { [_lock unlock]; return YES; }
    if (!_icb || _noop || !_spv) {
        [_lock unlock];
        if (err) *err = rdna4_err(49, @"compute pipeline does not support GPU indirect command buffers");
        return NO;
    }
    if (!RDNA4R.bufferAddress) {
        [_lock unlock];
        if (err) *err = rdna4_err(49, @"GPU compute indirect command buffers require Vulkan bufferDeviceAddress");
        return NO;
    }
    const rdna4x_xlib *xl = rdna4x_ensure_xlib(err);
    if (!xl || !xl->lower_icb_stage) {
        if (xl && err && !*err) *err = rdna4_err(42, @"translator library lacks garnetx_lower_icb_stage");
        [_lock unlock];
        return NO;
    }
    uint8_t *cspv = NULL; size_t clen = 0;
    RDNA4sMod mc = {0}; BOOL mcOpen = NO;
    RDNA4sRefl *rcRefl = NULL;
    BOOL ok = NO;
    do {
        uint32_t ctbl = 0, cvsp = 0; char xerr[256] = "";
        int rc = xl->lower_icb_stage((const uint8_t *)_spv.bytes, _spv.length, 3u, 0u,
                                     NULL, 0u, &cspv, &clen, &ctbl, &cvsp, xerr, sizeof xerr);
        if (rc != 0 || !cspv || clen < 20 || (clen & 3)) {
            if (err) *err = rdna4_err(42, [NSString stringWithFormat:@"icb lower compute stage failed (%d): %s", rc, xerr]);
            break;
        }
        if (ctbl != RDNA4_ICB_TABLE_BINDING || cvsp != 0u) {
            if (err) *err = rdna4_err(42, [NSString stringWithFormat:@"icb lower compute stage returned unexpected bindings tbl=%u vsp=%u", ctbl, cvsp]);
            break;
        }
        char why[256] = "";
        rcRefl = calloc(1, sizeof *rcRefl);
        if (!rcRefl) { if (err) *err = rdna4_err(49, @"out of memory reflecting ICB compute stage"); break; }
        if (rdna4s_parse((const uint32_t *)cspv, clen / 4, &mc, why, sizeof why) ||
            !mc.has_ep || mc.ep_model != 5u || rdna4s_reflect(&mc, 2, rcRefl)) {
            if (mc.id) mcOpen = YES;
            if (err) *err = rdna4_err(64, [NSString stringWithFormat:@"ICB lowered compute reflection failed: %s",
                                           why[0] ? why : rcRefl->why[0] ? rcRefl->why : "wrong entry point"]);
            break;
        }
        mcOpen = YES;
        if (!rdna4_icb_build_stage_dsl(_pb, _npb, rcRefl, 0, VK_SHADER_STAGE_COMPUTE_BIT, NO,
                                       &_icbDsl, &_icbPb, &_icbNpb, err))
            break;
        VkPushConstantRange pcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, _pcSize };
        VkDescriptorSetLayout layouts[5] = { _icbDsl, _emptySet, _argumentTables.dsl[0], _argumentTables.dsl[1], _argumentTables.dsl[2] };
        VkPipelineLayoutCreateInfo plc = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = _argumentTables.active ? 5u : 1u,
            .pSetLayouts = layouts,
            .pushConstantRangeCount = _pcSize ? 1u : 0u,
            .pPushConstantRanges = &pcr,
        };
        VkResult pr = vkCreatePipelineLayout(RDNA4R.dev, &plc, NULL, &_icbPl);
        if (pr != VK_SUCCESS) {
            if (err) *err = rdna4_err(46, [NSString stringWithFormat:@"vkCreatePipelineLayout(icb compute) = %d", pr]);
            break;
        }
        int allowFallback = 0;
        rdna4s_icb_compute_inspect((const uint32_t *)cspv, clen / 4, NULL, &allowFallback);
        _icbAllowFallback = allowFallback ? YES : NO;
        _icbSpv = [NSData dataWithBytes:cspv length:clen];
        if (!_icbSpv) {
            if (err) *err = rdna4_err(49, @"out of memory copying ICB lowered compute SPIR-V");
            break;
        }
        _icbGpuReady = YES;
        ok = YES;
    } while (0);
    if (mcOpen) rdna4s_free(&mc);
    free(rcRefl);
    if (cspv) xl->freep(cspv, clen);
    if (!ok) [self rdna4DestroyICBGPUState];
    [_lock unlock];
    return ok;
}
- (VkPipeline)icbPipelineForLocal:(const uint32_t *)l {
    if (!l) return VK_NULL_HANDLE;
    rdna4dc_caps caps = rdna4_device_caps();
    if (!rdna4dc_workgroup_ok(&caps, l[0], l[1], l[2])) return VK_NULL_HANDLE;
    if (!_mode && (l[0] != _local[0] || l[1] != _local[1] || l[2] != _local[2])) return VK_NULL_HANDLE;
    uint64_t key = _mode ? ((uint64_t)l[0] | ((uint64_t)l[1] << 20) | ((uint64_t)l[2] << 40)) : 0u;
    [_lock lock];
    if (!_icbGpuReady || !_icbSpv) { [_lock unlock]; return VK_NULL_HANDLE; }
    NSValue *v = _icbVariants[@(key)];
    if (!v) {
        uint32_t *words = NULL; size_t count = 0; char why[256] = "";
        if (rdna4s_icb_compute_indirect((const uint32_t *)_icbSpv.bytes, _icbSpv.length / 4,
                                        _pcOff, _mode, l, &words, &count, why, sizeof why)) {
            RDNA4LOG("icb compute indirect transformation: %s", why);
            [_lock unlock];
            return VK_NULL_HANDLE;
        }
        VkShaderModule module = rdna4_spv_module(words, count * 4, "icb-compute", NULL);
        free(words);
        if (!module) { [_lock unlock]; return VK_NULL_HANDLE; }
        VkSpecializationMapEntry me[3] = { { 0, 0, 4 }, { 1, 4, 4 }, { 2, 8, 4 } };
        uint32_t data[3] = { l[0], l[1], l[2] };
        VkSpecializationInfo si = { 3u, me, sizeof data, data };
        VkComputePipelineCreateInfo ci = {
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .layout = _icbPl,
            .stage = {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                .module = module,
                .pName = _ep,
                .pSpecializationInfo = _mode ? &si : NULL,
            },
        };
        VkPipelineCache pc = RDNA4R.pc;
#ifdef RDNA4_BINARY_ARCHIVE_IMPL_INC
        pc = rdna4_ba_vkcache();
#endif
        VkPipeline p = VK_NULL_HANDLE;
        uint64_t t_ = rdna4_now();
        VkResult r = vkCreateComputePipelines(RDNA4R.dev, pc, 1, &ci, NULL, &p);
        rdna4t_add(&T1.cmp, rdna4_now() - t_);
        atomic_fetch_add(&rdna4_pipes_new, 1);
        vkDestroyShaderModule(RDNA4R.dev, module, NULL);
        if (r != VK_SUCCESS) RDNA4LOG("vkCreateComputePipelines(icb local %u,%u,%u) = %d", l[0], l[1], l[2], r);
        else { v = [NSValue valueWithPointer:(void *)p]; _icbVariants[@(key)] = v; }
    }
    [_lock unlock];
    return v ? (VkPipeline)v.pointerValue : VK_NULL_HANDLE;
}
- (void)dealloc {
    [self rdna4DestroyICBGPUState];
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
    rdna4vft_refl_destroy(&_vft);
    rdna4rtr_refl_destroy(&_rt);
    free(_pb);
    uintptr_t rp = rdna4hs_load(&_realp); if (rp) CFRelease((CFTypeRef)rp);
}
RDNA4_DNR(RDNA4ComputePipelineState)
- (VkPipelineLayout)layout { return _pl; } - (VkDescriptorSetLayout)dsl { return _dsl; } - (const RDNA4PB *)pb { return _pb; } - (uint32_t)npb { return _npb; }
- (uint64_t)rdna4ICBToken { return _icbToken; }
- (BOOL)rdna4ICBAllowFallback { return _icbAllowFallback; }
- (VkPipelineLayout)icbLayout { return _icbPl; }
- (VkDescriptorSetLayout)icbDsl { return _icbDsl; }
- (const RDNA4PB *)icbPb { return _icbPb; }
- (uint32_t)icbNpb { return _icbNpb; }
- (uint32_t)rdna4CopyCompiledLocals:(uint32_t (*)[3])outLocals maxCount:(uint32_t)maxCount {
    if (!outLocals || !maxCount) return 0u;
    [_lock lock];
    uint32_t n = _nCompiledLocals < maxCount ? _nCompiledLocals : maxCount;
    for (uint32_t i = 0; i < n; i++) {
        outLocals[i][0] = _compiledLocals[i][0];
        outLocals[i][1] = _compiledLocals[i][1];
        outLocals[i][2] = _compiledLocals[i][2];
    }
    [_lock unlock];
    return n;
}
- (const RDNA4ArgumentTables *)argumentTables { return &_argumentTables; }
- (const rdna4_argument_scan_reflection_plan *)scanPlan { return &_scan; }
- (const uint32_t *)nominalLocal { return _local; } - (int)mode { return _mode; } - (uint32_t)pcOffset { return _pcOff; } - (uint32_t)pcSize { return _pcSize; }
- (id)reflection { return _reflection; }
- (void)rdna4SetReflection:(MTLComputePipelineReflection *)reflection { _reflection = reflection; }
- (id)device { return _dev; }
- (NSString *)label { return _lbl; }
- (void)setLabel:(NSString *)l { _lbl = [l copy]; }
- (NSUInteger)maxTotalThreadsPerThreadgroup { return rdna4_device_caps().invocations; }
- (NSUInteger)threadExecutionWidth {
    rdna4dc_caps caps = rdna4_device_caps();
    return rdna4dc_thread_execution_width(&caps);
}
- (NSUInteger)staticThreadgroupMemoryLength { return 0; }
- (NSUInteger)imageblockMemoryLengthForDimensions:(MTLSize)d { (void)d; return 0; }
- (BOOL)supportIndirectCommandBuffers { return _icb; }
- (const rdna4rtr_as_reflection_t *)rdna4RtAsRefl { return &_rt.as; }
- (id<MTLFunctionHandle>)functionHandleWithFunction:(id<MTLFunction>)function {
    RDNA4ComputePipelineState *real = [self rdna4Real];
    if (real) return [real functionHandleWithFunction:function];
    if (function && [function respondsToSelector:@selector(functionType)] &&
        [function functionType] == MTLFunctionTypeIntersection)
        return rdna4_rt_pipeline_function_handle(self, _dev, _iftOwnerToken, &_rt.ift, function, 0);
    return rdna4_vft_pipeline_handle(self, _dev, _vftOwnerToken, &_vft, function, 0);
}
- (id<MTLVisibleFunctionTable>)newVisibleFunctionTableWithDescriptor:(MTLVisibleFunctionTableDescriptor *)descriptor {
    RDNA4ComputePipelineState *real = [self rdna4Real];
    if (real) return [real newVisibleFunctionTableWithDescriptor:descriptor];
    return rdna4_vft_pipeline_new_table(self, _dev, _vftOwnerToken, &_vft, descriptor, 0);
}
- (id<MTLIntersectionFunctionTable>)newIntersectionFunctionTableWithDescriptor:(MTLIntersectionFunctionTableDescriptor *)descriptor {
    RDNA4ComputePipelineState *real = [self rdna4Real];
    if (real) return [real newIntersectionFunctionTableWithDescriptor:descriptor];
    return rdna4_rt_pipeline_new_ift(self, _dev, _iftOwnerToken, &_rt.ift, descriptor, 0);
}
- (uint64_t)uniqueIdentifier { return rdna4_resource_uid(&_uid); }
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 130000
- (MTLResourceID)gpuResourceID API_AVAILABLE(macos(13.0)) {
    MTLResourceID identifier;
    identifier._impl = _icbToken ? _icbToken : self.uniqueIdentifier;
    return identifier;
}
#endif
@end
#ifdef RDNA4_BINARY_ARCHIVE_IMPL_INC
static id rdna4_ba_compile_compute_state(id dev, id fn, NSData *spv, NSDictionary *meta, NSString *fname, NSError **err) {
    return [[RDNA4ComputePipelineState alloc] initWithDevice:dev function:fn spv:spv meta:meta fname:fname error:err];
}
#endif

