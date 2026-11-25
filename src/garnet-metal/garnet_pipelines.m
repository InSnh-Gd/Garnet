// garnet_pipelines.m - GarnetMetal modular compilation unit.
#if !defined(GARNET_INTERNAL) && !defined(GARNET_METAL_MODULAR_BUILD)
#define GARNET_INTERNAL static
#endif
#ifdef GARNET_METAL_MODULAR_BUILD
#include "garnet_metal_internal.h"
#endif
#include "garnet_indirect_compute.h"
#include "garnet_luma_workgroup.h"
#include "garnet_fragment_output.h"
#include "garnet_fallback_spv.h"

// ---------------------------------------------------------------------------------------------------------------
// RDNA4RenderPipelineState (10d/11e): SPIR-V from Resources/spvcache/<sha256(bitcodeData)>.spv -> VkPipeline variants.
// ---------------------------------------------------------------------------------------------------------------
#include "garnet_fragment_output.h"

   // bundle 10: nd = 1 when ads[na] is the depth/stencil attachment

// Returns the cached SPIR-V for fn, or nil with *err code 40 (no bitcodeData) / 41 (miss, or RDNA4M_FORCE_FALLBACK).
// *fname gets the function name for logs; *meta the <sha>.meta.json sidecar when present. Reflection reads the entry-point name out of the SPIR-V itself.
static NSData *rdna4_spv_lookup_impl(id fn, const char *role, NSError **err, NSString **fname, NSDictionary **meta) {
    SEL sel = NSSelectorFromString(@"bitcodeData");
    NSData *bc = (fn && [fn respondsToSelector:sel]) ? ((NSData *(*)(id, SEL))objc_msgSend)(fn, sel) : nil;
    if (fname) *fname = (fn && [fn respondsToSelector:@selector(name)]) ? [fn name] : @"?";
    if (!bc.length) {
        RDNA4_ONCE("function '%s' (class %s) has no bitcodeData: a Core Image / stitched library function (or a binary archive function) the translator cannot see; no pipeline is built for it", (fn && [fn respondsToSelector:@selector(name)]) ? [[fn name] UTF8String] : "?", fn ? class_getName(object_getClass(fn)) : "nil");
        if (err) *err = rdna4_err(40, [NSString stringWithFormat:@"%s function has no bitcodeData", role]); return nil; }
    if (rdna4_force_fallback()) { if (err) *err = rdna4_err(41, [NSString stringWithFormat:@"RDNA4M_FORCE_FALLBACK: %s treated as a spvcache miss", role]); return nil; }
    NSString *sha = rdna4_sha256hex(bc);
    NSString *path = nil; NSData *spv = nil;
    RDNA4LinkCtx *lk = rdna4_lk_for(fn);   // build 18 (P3) + Metal 3 Stage 2 linkage
    if (lk && lk->linkageError) {
        if (err) *err = lk->linkageError;
        return nil;
    }
    const BOOL stage2Only = lk && lk->useStage2 && (lk->requireStage2 || rdna4x_stage2_symbols_available());
    NSArray<NSString *> *keys = lk ? (stage2Only ? @[ lk->lsha ] : @[ lk->lsha, sha ]) : @[ sha ];
    for (NSString *cs in keys) {
        for (NSString *dir in rdna4_spv_dirs()) {   // H2: bundle spvcache first, then the side directory
            path = [dir stringByAppendingPathComponent:[cs stringByAppendingString:@".spv"]];
            spv = [NSData dataWithContentsOfFile:path];
            if (spv.length >= 20 && !(spv.length & 3)) break;
            spv = nil;
        }
        if (spv) { sha = cs; break; }
    }
    if (!spv) { if (err) *err = rdna4_err(41, [NSString stringWithFormat:@"spvcache miss for %s: %@", role, [[rdna4_spv_dirs().firstObject stringByAppendingPathComponent:sha] stringByAppendingString:@".spv"]]); return nil; }
    if (meta) *meta = rdna4_meta_for_spv(path);
    rdna4x_touch_hit(path, sha);
    RDNA4LOG("spvcache hit %s: %s (%lu bytes%s)", role, path.UTF8String, (unsigned long)spv.length, (meta && *meta) ? ", meta" : ", no meta");
    return spv;
}

GARNET_INTERNAL NSData *rdna4_spv_lookup(id fn, const char *role, NSError **err, NSString **fname, NSDictionary **meta) {
    uint64_t t_ = rdna4_now(); NSData *d = rdna4_spv_lookup_impl(fn, role, err, fname, meta); rdna4t_add(&T1.spv, rdna4_now() - t_); return d;
}

GARNET_INTERNAL VkShaderModule rdna4_spv_module(const void *code, size_t bytes, const char *role, NSError **err) {
    VkShaderModuleCreateInfo smc = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes, .pCode = code };
    VkShaderModule m = VK_NULL_HANDLE; uint64_t t_ = rdna4_now(); VkResult r = vkCreateShaderModule(RDNA4R.dev, &smc, NULL, &m); rdna4t_add(&T1.shm, rdna4_now() - t_);
    if (r != VK_SUCCESS) { if (err) *err = rdna4_err(42, [NSString stringWithFormat:@"vkCreateShaderModule(%s) = %d", role, r]); return VK_NULL_HANDLE; }
    return m;
}

@implementation RDNA4RenderPipelineState
static _Atomic uint64_t rdna4_icb_pso_seq;
- (instancetype)initWithDevice:(id)dev descriptor:(MTLRenderPipelineDescriptor *)d error:(NSError **)err { return [self initWithDevice:dev descriptor:d options:0 noFallback:NO error:err]; }
- (instancetype)initWithDevice:(id)dev descriptor:(MTLRenderPipelineDescriptor *)d noFallback:(BOOL)nofb error:(NSError **)err { return [self initWithDevice:dev descriptor:d options:0 noFallback:nofb error:err]; }
- (const char *)rdna4VName { return _vnm; }
- (const char *)rdna4FName { return _fnm; }
- (BOOL)rdna4HasFragment { return _hasFrag; }
- (BOOL)rdna4IsRenderFallback { return _fallback; }
- (RDNA4RenderPipelineState *)rdna4Real { uintptr_t p = rdna4hs_load(&_realp); return p ? (__bridge RDNA4RenderPipelineState *)(void *)p : nil; }
// Runs on the watcher queue. Builds a complete NEW pipeline object from the recorded descriptor (no fallback allowed, no AIR dump) and publishes it.
- (BOOL)rdna4HotRebuild {
    if (!_hsDesc) return NO;
    NSError *e = nil;
    RDNA4_LK_SCOPE; rdna4x_linkage_render(_hsDesc);   // build 18 (P3): the rebuild re-reads the descriptor's linkage on the watcher thread, so it looks under the same linked key
    RDNA4RenderPipelineState *r = [[RDNA4RenderPipelineState alloc] initWithDevice:_dev descriptor:_hsDesc noFallback:YES error:&e];
    if (!r) { RDNA4LOG("HOT-SWAP rebuild failed: %s", e.localizedDescription.UTF8String); return NO; }
    r->_lbl = _lbl;
    r->_icbToken = _icbToken;
    r->_maxAmp = _maxAmp;
    uintptr_t v = (uintptr_t)CFBridgingRetain(r);
    if (!rdna4hs_install(&_realp, v)) { CFRelease((CFTypeRef)v); return NO; }   // lost a race (cannot happen: one watcher): keep the first
    return YES;
}
- (instancetype)initWithDevice:(id)dev descriptor:(MTLRenderPipelineDescriptor *)d options:(NSUInteger)options noFallback:(BOOL)nofb error:(NSError **)err {
    self = [super init];
    if (!self) return nil;
    _dev = dev; _lock = [NSLock new]; _variants = [NSMutableDictionary dictionary]; _icbVariants = [NSMutableDictionary dictionary]; _owned = [NSMutableArray array];
    if (d.label) _lbl = [d.label copy];
    _icb = d.supportIndirectCommandBuffers;
    if (_icb) _icbToken = RDNA4_ICB_PSO_TOKEN_TAG | (atomic_fetch_add(&rdna4_icb_pso_seq, 1) + 1);
    const NSUInteger reqMaxAmp = [d respondsToSelector:@selector(maxVertexAmplificationCount)]
        ? d.maxVertexAmplificationCount : 1u;
    const char *ampWhy = NULL;
    if (!rdna4amp_pipeline_count_ok((uint64_t)reqMaxAmp, RDNA4AMP_MAX_SUPPORTED_COUNT, 0, &ampWhy)) {
        if (err) *err = rdna4_err(57, [NSString stringWithFormat:@"maxVertexAmplificationCount %lu: %s",
                                       (unsigned long)reqMaxAmp, ampWhy ?: "unsupported"]);
        return nil;
    }
    _maxAmp = (uint32_t)reqMaxAmp;
    if (!rdna4_vft_validate_render_footnote10(d, NO, NO, NO, err)) return nil;
    NSError *vlkErr = rdna4_lk_error(d.vertexFunction);
    if (vlkErr) { if (err) *err = vlkErr; return nil; }
    NSError *flkErr = d.fragmentFunction ? rdna4_lk_error(d.fragmentFunction) : nil;
    if (flkErr) { if (err) *err = flkErr; return nil; }
    const BOOL hasFrag = d.fragmentFunction != nil;
    _hasFrag = hasFrag;
    _rastOff = !d.isRasterizationEnabled;
    if (!rdna4_radv_open(err)) return nil;
    const uint64_t deadline = rdna4_now() + (uint64_t)RDNA4G_SYNC_WAIT_APP_MS * 1000000ull;
    // 11e: SPIR-V from binaryArchives or spvcache; on a miss in WindowServer (or RDNA4M_FORCE_FALLBACK with RDNA4M_ALLOW) build
    // from the built-in no-write shaders instead of failing (the AIR is dumped by the caller-side rdna4_dump_pipeline).
    NSString *vname = nil, *fname = hasFrag ? nil : @"(none)"; NSError *e1 = nil, *e2 = nil; NSDictionary *mv_ = nil, *mf_ = nil;
    NSData *vsd = nil, *fsd = nil;
    BOOL baHit = NO;
#ifdef RDNA4_BINARY_ARCHIVE_IMPL_INC
    NSData *baVk = nil; rdna4ba_fingerprint_t baFp = {0}; NSError *baErr = nil;
    if (!rdna4_ba_lookup_render(dev, d, options, &vsd, &mv_, &fsd, &mf_, &baVk, &baFp, &baHit, &baErr)) {
        if (err) *err = baErr;
        return nil;
    }
#else
    if (options & ((NSUInteger)1u << 2)) {
        if (err) *err = [NSError errorWithDomain:MTLBinaryArchiveDomain
            code:(NSInteger)MTLBinaryArchiveErrorCompilationFailure
            userInfo:@{ NSLocalizedDescriptionKey: @"Render pipeline missed binary archive (MTLPipelineOptionFailOnBinaryArchiveMiss)" }];
        return nil;
    }
#endif
    if (baHit) {
        vname = (d.vertexFunction && [d.vertexFunction respondsToSelector:@selector(name)]) ? [d.vertexFunction name] : @"?";
        if (hasFrag)
            fname = (d.fragmentFunction && [d.fragmentFunction respondsToSelector:@selector(name)]) ? [d.fragmentFunction name] : @"?";
    } else {
        vsd = rdna4_spv_lookup(d.vertexFunction, "vertex", &e1, &vname, &mv_);
        if (vsd && [mv_[@"stage"] isEqual:@"TessellationEvaluation"] &&
            ![mv_[@"tessellation"] isKindOfClass:[NSDictionary class]] && !nofb && rdna4x_active()) {
            vsd = nil; mv_ = nil;
            e1 = rdna4_err(41, @"cached tessellation sidecar lacks tessellation metadata");
        }
        fsd = hasFrag ? rdna4_spv_lookup(d.fragmentFunction, "fragment", &e2, &fname, &mf_) : nil;
    }
    strlcpy(_vnm, vname.UTF8String ? vname.UTF8String : "?", sizeof _vnm); strlcpy(_fnm, fname.UTF8String ? fname.UTF8String : "(none)", sizeof _fnm);
    BOOL fb = NO;
    NSError *translationFailure = nil;
    if (!vsd || (hasFrag && !fsd)) {
        NSError *e = e1 ? e1 : e2;
        const BOOL inproc = (e.code == 41 && !nofb && rdna4x_active()) ? YES : NO;
        if (inproc) {   // bundle 14: translate in this process first (the AIR dump and the daemon wait below are then skipped); a failure or a timeout falls through to today's fallback + hot-swap
            const uint64_t dl = deadline;
            NSError *vertexFailure = nil, *fragmentFailure = nil;
            const BOOL tv = vsd ? YES : rdna4x_translate_fn_with_error(d.vertexFunction, "vertex", dl, &vertexFailure);
            const BOOL tf = (!hasFrag || fsd) ? YES : rdna4x_translate_fn_with_error(d.fragmentFunction, "fragment", dl, &fragmentFailure);
            translationFailure = vertexFailure ?: fragmentFailure;
            if (tv && tf) {
                e1 = nil; e2 = nil; mv_ = nil; mf_ = nil;
                vsd = rdna4_spv_lookup(d.vertexFunction, "vertex", &e1, &vname, &mv_);
                fsd = hasFrag ? rdna4_spv_lookup(d.fragmentFunction, "fragment", &e2, &fname, &mf_) : nil;
                strlcpy(_vnm, vname.UTF8String ? vname.UTF8String : "?", sizeof _vnm); strlcpy(_fnm, fname.UTF8String ? fname.UTF8String : "(none)", sizeof _fnm);
            }
        }
    }
    if (!vsd || (hasFrag && !fsd)) {
        NSError *e = e1 ? e1 : e2;
        const BOOL inproc = (e.code == 41 && !nofb && rdna4x_active()) ? YES : NO;
        if (e.code == 41 && !nofb) {
            NSError *dumpFailure = !inproc ? rdna4_dump_pipeline(d, !vsd, hasFrag && !fsd) : nil;
            if (!inproc && rdna4g_syncwait_applies(rdna4_is_ws(), atomic_load(&rdna4_app_admitted), rdna4_force_fallback(), rdna4_test_fb_as_ws())) {
                NSMutableArray<NSString *> *want = [NSMutableArray array];
                NSString *sv_ = !vsd ? rdna4_fn_sha(d.vertexFunction) : nil;
                NSString *sf_ = hasFrag && !fsd ? rdna4_fn_sha(d.fragmentFunction) : nil;
                if (sv_) [want addObject:sv_];
                if (sf_) [want addObject:sf_];
                if (rdna4_sync_wait("render", want, NO)) {
                    if (!vsd) vsd = rdna4_spv_lookup(d.vertexFunction, "vertex", &e1, &vname, &mv_);
                    if (hasFrag && !fsd) fsd = rdna4_spv_lookup(d.fragmentFunction, "fragment", &e2, &fname, &mf_);
                    strlcpy(_vnm, vname.UTF8String ? vname.UTF8String : "?", sizeof _vnm); strlcpy(_fnm, fname.UTF8String ? fname.UTF8String : "(none)", sizeof _fnm);
                }
            }
            if (!vsd || (hasFrag && !fsd)) {
                RDNA4LinkCtx *vlk = rdna4_lk_for(d.vertexFunction);
                RDNA4LinkCtx *flk = hasFrag ? rdna4_lk_for(d.fragmentFunction) : nil;
                const BOOL noStage2Fallback = ((vlk && vlk->requireStage2) || (flk && flk->requireStage2)) ? YES : NO;
                if (noStage2Fallback || !rdna4_fallback_ok()) {
                    if (translationFailure) {
                        NSMutableDictionary *info = [e.userInfo mutableCopy];
                        info[NSUnderlyingErrorKey] = translationFailure;
                        info[NSLocalizedDescriptionKey] = [NSString stringWithFormat:@"%@: %@", e.localizedDescription, translationFailure.localizedDescription];
                        e = [NSError errorWithDomain:e.domain code:e.code userInfo:info];
                    }
                    if (err) *err = dumpFailure ?: e;
                    return nil;
                }
                fb = YES; _fallback = YES;
                RDNA4LOG("FALLBACK %s / %s (%s)", vname.UTF8String, fname.UTF8String, e.localizedDescription.UTF8String);
            }
        } else { if (err) *err = e; return nil; }
    }
    RDNA4sRefl *rv = NULL, *rf = NULL; RDNA4sMod mv = {0}, mf = {0}; char vep[128] = "main", fep[128] = "main";
    NSData *fsp = nil; NSDictionary *preV = nil, *preF = nil;
    if (!fb) {
        if (!rdna4_radv_open(err)) return nil;
        preV = mv_; preF = mf_;
        vsd = rdna4_gpu_argument_spv(vsd, &mv_, deadline, err);
        if (hasFrag) fsd = rdna4_gpu_argument_spv(fsd, &mf_, deadline, err);
        if (!vsd || (hasFrag && !fsd)) return nil;
        _vftOwnerToken = rdna4_vft_next_owner_token();
        NSDictionary *vftMetaV = (mv_ && mv_[@"visible_function_tables"]) ? mv_ : preV;
        if (!rdna4x_parse_vft_from_meta(vftMetaV, &_vftV, err)) return nil;
        if (hasFrag) {
            NSDictionary *vftMetaF = (mf_ && mf_[@"visible_function_tables"]) ? mf_ : preF;
            if (!rdna4x_parse_vft_from_meta(vftMetaF, &_vftF, err)) return nil;
        }
        _iftOwnerToken = rdna4_vft_next_owner_token();
        NSDictionary *rtMetaV = (mv_ && (mv_[@"acceleration_structures"] || mv_[@"intersection_function_tables"])) ? mv_ : preV;
        if (!rdna4_rt_parse_meta(rtMetaV, &_rtV, err)) return nil;
        if (hasFrag) {
            NSDictionary *rtMetaF = (mf_ && (mf_[@"acceleration_structures"] || mf_[@"intersection_function_tables"])) ? mf_ : preF;
            if (!rdna4_rt_parse_meta(rtMetaF, &_rtF, err)) return nil;
        }
        BOOL hasVftMeta = (_vftV.present || _vftF.present) ? YES : NO;
        BOOL hasRenderRT = (rdna4rtr_refl_uses_raytracing(&_rtV) || rdna4rtr_refl_uses_raytracing(&_rtF)) ? YES : NO;
        if ((hasVftMeta || hasRenderRT) && !rdna4_vft_validate_render_footnote10(d, NO, hasVftMeta, hasRenderRT, err)) return nil;
        char why[256] = "";
        fsp = hasFrag ? rdna4_spv_set(fsd, 1) : nil;   // fragment descriptors live in set 1 (binding-model comment above)
        rv = calloc(1, sizeof *rv); rf = hasFrag ? calloc(1, sizeof *rf) : NULL;
        BOOL bad = NO;
        if (rdna4s_parse(vsd.bytes, vsd.length / 4, &mv, why, sizeof why) || !mv.has_ep || (mv.ep_model != 0 && mv.ep_model != 2) || rdna4s_reflect(&mv, 0, rv)) {
            if (err) *err = rdna4_err(51, [NSString stringWithFormat:@"vertex SPIR-V reflection: %s", why[0] ? why : rv->why[0] ? rv->why : "no/wrong entry point"]); bad = YES; }
        else if (hasFrag && (rdna4s_parse(fsp.bytes, fsp.length / 4, &mf, why, sizeof why) || !mf.has_ep || mf.ep_model != 4 || rdna4s_reflect(&mf, 1, rf))) {
            if (err) *err = rdna4_err(52, [NSString stringWithFormat:@"fragment SPIR-V reflection: %s", why[0] ? why : rf->why[0] ? rf->why : "no/wrong entry point"]); bad = YES; }
        if (!bad && hasFrag && !RDNA4R.orderedColorAccess) {
            BOOL fetch = NO;
            for (int b = 192; b < 200; b++) fetch |= rf->bind[1][b].used && rf->bind[1][b].type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
            const uint32_t *words = fsp.bytes;
            for (size_t p = 5; fetch && p < fsp.length / 4; p += words[p] >> 16) {
                uint32_t op = words[p] & 0xffff, wc = words[p] >> 16;
                if ((op == 71 && wc >= 4 && words[p + 2] == 11 && words[p + 3] == 7) ||
                    (op == 72 && wc >= 5 && words[p + 3] == 11 && words[p + 4] == 7)) {
                    if (err) *err = rdna4_err(59, @"framebuffer fetch with primitive_id requires rasterization-order attachment access");
                    bad = YES; break;
                }
            }
        }
        if (!bad && hasFrag) {
            size_t count = fsp.length / 4;
            uint32_t *normalized = NULL;
            if (rdna4s_widen_fragment_output(fsp.bytes, &count, &mf, &normalized)) {
                if (err) *err = rdna4_err(52, @"fragment SPIR-V: insufficient memory to widen color outputs");
                bad = YES;
            } else if (normalized) {
                fsp = [NSData dataWithBytesNoCopy:normalized length:count * 4 freeWhenDone:YES];
            }
        }
        if (!bad && mv.ep_model == 2) {
            _isTess = YES;
            memset(&_tessCfg, 0, sizeof _tessCfg);
            if (!RDNA4R.tessellation || !RDNA4R.bufferAddress) {
                if (err) *err = rdna4_err(49, @"tessellation requires RADV tessellationShader and bufferDeviceAddress");
                bad = YES;
            } else if (d.tessellationFactorFormat != MTLTessellationFactorFormatHalf) {
                if (err) *err = rdna4_err(57, @"unsupported tessellationFactorFormat (only Half is supported)");
                bad = YES;
            } else {
                NSDictionary *tessMeta = [mv_[@"tessellation"] isKindOfClass:[NSDictionary class]] ? mv_[@"tessellation"] : nil;
                if (!tessMeta) {
                    if (err) *err = rdna4_err(51, @"tessellation SPIR-V metadata is missing (stale spvcache sidecar)");
                    bad = YES;
                } else if ([tessMeta[@"amplification_id"] isKindOfClass:[NSDictionary class]] ||
                           [tessMeta[@"amplification_count"] isKindOfClass:[NSDictionary class]] ||
                           !rdna4amp_pipeline_count_ok((uint64_t)_maxAmp, RDNA4AMP_MAX_SUPPORTED_COUNT, 1, &ampWhy)) {
                    if (err) *err = rdna4_err(57, @"vertex amplification in tessellation pipelines is not supported");
                    bad = YES;
                }
                if (!bad) {
                    if (tessMeta && tessMeta[@"control_point_count"]) {
                        _tessCfg.control_point_count = [tessMeta[@"control_point_count"] unsignedIntValue];
                    } else {
                        for (uint32_t id = 1; id <= mv.bound; id++) {
                            const RDNA4sId *v = &mv.id[id];
                            if (v->op == 59 && v->sc == 1 && !v->patch && v->builtin < 0 && v->loc >= 0 && v->elem <= mv.bound) {
                                uint32_t pt = mv.id[v->elem].elem;
                                if (pt <= mv.bound && mv.id[pt].op == 28 && mv.id[pt].n <= mv.bound) {
                                    _tessCfg.control_point_count = mv.id[mv.id[pt].n].cval;
                                    break;
                                }
                            }
                        }
                    }
                    NSDictionary *instMeta = (tessMeta && [tessMeta[@"instance_id"] isKindOfClass:[NSDictionary class]]) ? tessMeta[@"instance_id"] : nil;
                    if (instMeta && instMeta[@"location"]) {
                        _tessCfg.has_instance_id = 1;
                        _tessCfg.instance_id_loc = [instMeta[@"location"] unsignedIntValue];
                    }
                    if (d.tessellationPartitionMode > MTLTessellationPartitionModeFractionalEven ||
                        d.tessellationOutputWindingOrder > MTLWindingCounterClockwise ||
                        d.tessellationFactorStepFunction > MTLTessellationFactorStepFunctionPerPatchAndPerInstance ||
                        d.tessellationControlPointIndexType > MTLTessellationControlPointIndexTypeUInt32 ||
                        d.maxTessellationFactor == 0 || d.maxTessellationFactor > RDNA4R.maxTessGenLevel) {
                        if (err) *err = rdna4_err(57, [NSString stringWithFormat:@"tessellation pipeline descriptor parameter out of range (part=%lu wind=%lu step=%lu cpidx=%lu max=%lu)",
                            (unsigned long)d.tessellationPartitionMode, (unsigned long)d.tessellationOutputWindingOrder,
                            (unsigned long)d.tessellationFactorStepFunction, (unsigned long)d.tessellationControlPointIndexType, (unsigned long)d.maxTessellationFactor]);
                        bad = YES;
                    } else {
                        _tessCfg.partition = (uint8_t)d.tessellationPartitionMode;
                        _tessCfg.winding = (uint8_t)d.tessellationOutputWindingOrder;
                        _tessCfg.factor_step = (uint8_t)d.tessellationFactorStepFunction;
                        _tessCfg.cp_idx_type = (uint8_t)d.tessellationControlPointIndexType;
                        _tessCfg.factor_scale_enabled = d.isTessellationFactorScaleEnabled ? 1 : 0;
                        _tessCfg.max_tess_factor = (float)d.maxTessellationFactor;
                        _tessCfg.tess_set = 2u;
                    }
                    if (!bad && !rdna4_tess_inspect_tes(&mv, &_tessCfg, why, sizeof why)) {
                        if (err) *err = rdna4_err(51, [NSString stringWithFormat:@"tessellation SPIR-V: %s", why]);
                        bad = YES;
                    } else if (!bad) {
                        MTLVertexDescriptor *vd = d.vertexDescriptor;
                        for (uint32_t i = 0; i < _tessCfg.num_inputs; i++) {
                            rdna4_tess_input_t *in = &_tessCfg.inputs[i];
                            if (in->location >= 31) {
                                if (err) *err = rdna4_err(57, [NSString stringWithFormat:@"tessellation vertex attribute %u exceeds max attributes (31)", in->location]);
                                bad = YES; break;
                            }
                            MTLVertexAttributeDescriptor *a = vd ? vd.attributes[in->location] : nil;
                            if (!a || a.format == MTLVertexFormatInvalid) continue;
                            VkFormat vf = rdna4_vkvfmt(a.format);
                            rdna4_tess_vfmt_t tf = rdna4_tess_vfmt_from_vk((uint32_t)vf);
                            NSUInteger bi = a.bufferIndex;
                            if (tf == RDNA4_TFMT_INVALID || bi >= RDNA4_TESS_MAX_VBUFS || a.offset > 2047) {
                                if (err) *err = rdna4_err(57, [NSString stringWithFormat:@"tessellation vertex attribute %u: format %lu / bufferIndex %lu / offset %lu not supported", in->location, (unsigned long)a.format, (unsigned long)bi, (unsigned long)a.offset]);
                                bad = YES; break;
                            }
                            MTLVertexBufferLayoutDescriptor *l = vd.layouts[bi];
                            if (!l || l.stride == (NSUInteger)-1 || l.stride > 2048 ||
                                (l.stepFunction != MTLVertexStepFunctionConstant && l.stepFunction != MTLVertexStepFunctionPerPatch && l.stepFunction != MTLVertexStepFunctionPerPatchControlPoint) ||
                                (l.stepFunction != MTLVertexStepFunctionConstant && !l.stepRate)) {
                                if (err) *err = rdna4_err(58, [NSString stringWithFormat:@"tessellation vertex layout %lu: stride %lu stepFunction %lu rate %lu not supported", (unsigned long)bi, (unsigned long)l.stride, (unsigned long)l.stepFunction, (unsigned long)l.stepRate]);
                                bad = YES; break;
                            }
                            in->has_attr = 1;
                            in->vfmt = (uint8_t)tf;
                            in->buf_idx = (uint8_t)bi;
                            in->step_func = (uint8_t)l.stepFunction;
                            in->step_rate = (uint32_t)l.stepRate;
                            in->attr_offset = (uint32_t)a.offset;
                            in->buf_stride = (l.stepFunction == MTLVertexStepFunctionConstant) ? 0u : (uint32_t)l.stride;
                        }
                        if (!bad && !rdna4_tess_validate_cfg(&_tessCfg, RDNA4R.maxTessGenLevel, RDNA4R.maxTessPatchSize, why, sizeof why)) {
                            if (err) *err = rdna4_err(57, [NSString stringWithFormat:@"tessellation config: %s", why]);
                            bad = YES;
                        }
                    }
                }
            }
        }
        if (!bad) { strlcpy(vep, mv.ep_name, sizeof vep); if (hasFrag) strlcpy(fep, mf.ep_name, sizeof fep); }
        rdna4s_free(&mv); if (hasFrag) rdna4s_free(&mf);
        if (bad) { free(rv); free(rf); return nil; }
    }
    strlcpy(_vep, vep, sizeof _vep); strlcpy(_fep, fep, sizeof _fep);
    BOOL ok = NO;
    do {
        if (!_isTess) {
            _vs = fb ? rdna4_spv_module(rdna4_fb_vs_spv, sizeof rdna4_fb_vs_spv, "vertex", err) : rdna4_spv_module(vsd.bytes, vsd.length, "vertex", err);
            if (!_vs) break;
        }
        if (hasFrag) { _fs = fb ? rdna4_spv_module(rdna4_fb_fs_spv, sizeof rdna4_fb_fs_spv, "fragment", err) : rdna4_spv_module(fsp.bytes, fsp.length, "fragment", err); if (!_fs) break; }
        ok = YES;
    } while (0);
    if (!ok) { free(rv); free(rf); return nil; }
    VkAttachmentDescription ads[9]; VkAttachmentReference refs[8]; uint32_t na = 0;
    {   // bundle 10: sample count, dynamic states, depth bias: rdna4pc_config (a colour-only single-sample pipeline gets exactly the old three dynamic states, one sample, no bias)
        const char *pcwhy = NULL;
        if (!rdna4pc_config(d.depthAttachmentPixelFormat != MTLPixelFormatInvalid, d.stencilAttachmentPixelFormat != MTLPixelFormatInvalid, (unsigned long)d.rasterSampleCount, d.alphaToCoverageEnabled ? 1 : 0, rdna4_ms_mask(), &_pc, &pcwhy)) {
            RDNA4LOG("pipeline: rasterSampleCount %lu refused: %s (device mask 0x%x)", (unsigned long)d.rasterSampleCount, pcwhy, rdna4_ms_mask());
            if (err) *err = rdna4_err(44, [NSString stringWithFormat:@"rasterSampleCount %lu: %s", (unsigned long)d.rasterSampleCount, pcwhy]); free(rv); free(rf); return nil; }
    }
    uint32_t dualSrcUsed = 0;
    for (NSUInteger i = 0; i < 8; i++) {
        MTLRenderPipelineColorAttachmentDescriptor *ca = d.colorAttachments[i];
        if (ca.pixelFormat == MTLPixelFormatInvalid) continue;
        const RDNA4Fmt *f = rdna4_fmt(ca.pixelFormat);
        if (!f) { RDNA4LOG("pipeline: colour attachment %lu pixel format %lu has no Vulkan mapping in the format table", (unsigned long)i, (unsigned long)ca.pixelFormat);
                  if (err) *err = rdna4_err(43, [NSString stringWithFormat:@"unsupported colour attachment format %lu", (unsigned long)ca.pixelFormat]); free(rv); free(rf); return nil; }
        if (f->flags & (RDNA4F_A8 | RDNA4F_BC)) { RDNA4LOG("pipeline: colour attachment %lu format %lu cannot be a render target", (unsigned long)i, (unsigned long)ca.pixelFormat);
                  if (err) *err = rdna4_err(43, [NSString stringWithFormat:@"unsupported colour attachment format %lu", (unsigned long)ca.pixelFormat]); free(rv); free(rf); return nil; }
        if (!rdna4_radv_open(err)) { free(rv); free(rf); return nil; }
        { VkFormatFeatureFlags ff = rdna4_fmt_feats(f), nd = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | (ca.isBlendingEnabled ? VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT : 0);
          if ((ff & nd) != nd) { RDNA4LOG("pipeline: colour attachment %lu pixel format %lu (vk %d): RADV features 0x%x lack 0x%x", (unsigned long)i, (unsigned long)ca.pixelFormat, f->vk, ff, nd & ~ff);
              if (err) *err = rdna4_err(43, [NSString stringWithFormat:@"unsupported colour attachment format %lu (RADV lacks attachment%s support)", (unsigned long)ca.pixelFormat, ca.isBlendingEnabled ? "/blend" : ""]); free(rv); free(rf); return nil; } }
        RDNA4LOG("pipeline: colour attachment %lu pixel format %lu (vk %d)", (unsigned long)i, (unsigned long)ca.pixelFormat, f->vk);
        ads[na] = (VkAttachmentDescription){ .format = f->vk, .samples = (VkSampleCountFlagBits)_pc.samples, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        refs[na] = (VkAttachmentReference){ na, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkColorComponentFlags wm = 0; MTLColorWriteMask m = ca.writeMask;
        if (m & MTLColorWriteMaskRed) wm |= VK_COLOR_COMPONENT_R_BIT; if (m & MTLColorWriteMaskGreen) wm |= VK_COLOR_COMPONENT_G_BIT;
        if (m & MTLColorWriteMaskBlue) wm |= VK_COLOR_COMPONENT_B_BIT; if (m & MTLColorWriteMaskAlpha) wm |= VK_COLOR_COMPONENT_A_BIT;
        wm = rdna4if_write_mask(fb, (f->flags & (RDNA4F_UINT | RDNA4F_SINT)) != 0, wm);   // retain the integer-attachment fallback guard
        if (!hasFrag) { wm = 0; RDNA4LOG("pipeline: vertex-only (no fragment stage): attachment %u write mask forced to 0 (contents kept)", na); }
        else if (rf && !(rf->outmask & (1u << na))) { wm = 0; RDNA4LOG("pipeline: fragment stage does not write output %u: attachment write mask forced to 0 (contents kept)", na); }
        _cba[na] = (VkPipelineColorBlendAttachmentState){ .colorWriteMask = wm };
        if (ca.isBlendingEnabled) {   // 11e: blending from the descriptor (enable, factors, ops, write mask)
            BOOL bad = NO;
            BOOL (^isDualFactor)(MTLBlendFactor) = ^(MTLBlendFactor bf) {
                return (BOOL)(bf == MTLBlendFactorSource1Color || bf == MTLBlendFactorOneMinusSource1Color ||
                              bf == MTLBlendFactorSource1Alpha || bf == MTLBlendFactorOneMinusSource1Alpha);
            };
            if (isDualFactor(ca.sourceRGBBlendFactor) || isDualFactor(ca.destinationRGBBlendFactor) ||
                isDualFactor(ca.sourceAlphaBlendFactor) || isDualFactor(ca.destinationAlphaBlendFactor)) {
                dualSrcUsed++;
                if (!RDNA4R.dualSrcBlend || na >= RDNA4R.lim.maxFragmentDualSrcAttachments ||
                    dualSrcUsed > RDNA4R.lim.maxFragmentDualSrcAttachments)
                    bad = YES;
            }
            _cba[na].blendEnable = VK_TRUE;
            _cba[na].srcColorBlendFactor = rdna4_bf(ca.sourceRGBBlendFactor, &bad); _cba[na].dstColorBlendFactor = rdna4_bf(ca.destinationRGBBlendFactor, &bad);
            _cba[na].srcAlphaBlendFactor = rdna4_bf(ca.sourceAlphaBlendFactor, &bad); _cba[na].dstAlphaBlendFactor = rdna4_bf(ca.destinationAlphaBlendFactor, &bad);
            _cba[na].colorBlendOp = rdna4_bo(ca.rgbBlendOperation, &bad); _cba[na].alphaBlendOp = rdna4_bo(ca.alphaBlendOperation, &bad);
            if (bad) { if (err) *err = rdna4_err(56, @"blend factor/operation or dual-source attachment limit not supported"); free(rv); free(rf); return nil; }
            RDNA4LOG("pipeline: attachment %lu blending rgb %lu/%lu/%lu alpha %lu/%lu/%lu writeMask 0x%lx", (unsigned long)i, (unsigned long)ca.rgbBlendOperation,
                   (unsigned long)ca.sourceRGBBlendFactor, (unsigned long)ca.destinationRGBBlendFactor, (unsigned long)ca.alphaBlendOperation,
                   (unsigned long)ca.sourceAlphaBlendFactor, (unsigned long)ca.destinationAlphaBlendFactor, (unsigned long)m);
        }
        na++;
    }
    _na = na;
    _argumentTables.fragmentColorAttachments = na;
    rdna4_argument_descriptor_usage descriptorUsage;
    if (!rdna4_argument_descriptor_scan(rv, rf, na, &descriptorUsage) ||
        !rdna4_argument_descriptor_validate(&descriptorUsage, NO, err)) {
        free(rv); free(rf); return nil;
    }
    // bundle 10: the depth / stencil attachment.  One Vulkan attachment: the descriptor's depth format and stencil format are the same format (a combined one) or only one of them is set.
    uint32_t nd = 0; VkAttachmentReference dref = { 0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    _dpf = d.depthAttachmentPixelFormat; _spf = d.stencilAttachmentPixelFormat; _dsVk = VK_FORMAT_UNDEFINED; _dsAsp = 0;
    if (_dpf != MTLPixelFormatInvalid || _spf != MTLPixelFormatInvalid) {
        MTLPixelFormat dsp = _dpf != MTLPixelFormatInvalid ? _dpf : _spf; rdna4dp_fmt_t df; const char *dwhy = NULL;
        if (_dpf != MTLPixelFormatInvalid && _spf != MTLPixelFormatInvalid && _dpf != _spf) dwhy = "separate depth and stencil attachment formats are not supported (use one combined format such as Depth32Float_Stencil8)";
        else if (!rdna4dp_fmt((unsigned long)dsp, &df)) dwhy = df.why;
        else if (_dpf != MTLPixelFormatInvalid && !(df.aspects & RDNA4DP_ASP_DEPTH)) dwhy = "depthAttachmentPixelFormat is a stencil-only format";
        else if (_spf != MTLPixelFormatInvalid && !(df.aspects & RDNA4DP_ASP_STENCIL)) dwhy = "stencilAttachmentPixelFormat has no stencil aspect";
        const RDNA4Fmt *dsf = dwhy ? NULL : rdna4_fmt_ds(dsp);
        if (!dwhy && !dsf) dwhy = "no Vulkan mapping";
        if (!dwhy && !(rdna4_fmt_feats(dsf) & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) dwhy = "the device lacks depth/stencil attachment support for this format";
        if (dwhy) { RDNA4LOG("pipeline: depth/stencil attachment (depth pf %lu stencil pf %lu): %s", (unsigned long)_dpf, (unsigned long)_spf, dwhy);
                    if (err) *err = rdna4_err(44, [NSString stringWithFormat:@"unsupported depth/stencil attachment (depth pf %lu, stencil pf %lu): %s", (unsigned long)_dpf, (unsigned long)_spf, dwhy]); free(rv); free(rf); return nil; }
        _dsVk = dsf->vk; _dsAsp = df.aspects; nd = 1; dref.attachment = na;
        ads[na] = (VkAttachmentDescription){ .format = dsf->vk, .samples = (VkSampleCountFlagBits)_pc.samples,
            .loadOp = (df.aspects & RDNA4DP_ASP_DEPTH) ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE, .storeOp = (df.aspects & RDNA4DP_ASP_DEPTH) ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .stencilLoadOp = (df.aspects & RDNA4DP_ASP_STENCIL) ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = (df.aspects & RDNA4DP_ASP_STENCIL) ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
        RDNA4LOG("pipeline: depth/stencil attachment pixel format %lu (vk %d, aspects 0x%x), %u sample(s)", (unsigned long)dsp, dsf->vk, df.aspects, _pc.samples);
    }
    // 11e-2: a fragment stage with input attachments (ColorInput, binding 192+n, InputAttachmentIndex n) gets the "fetch" render-pass shape:
    // every colour attachment is also an input attachment (same index), layouts GENERAL; the encoder switches to a compatible pass.
    for (int b = 192; rf && b < 200; b++) if (rf->bind[1][b].used && rf->bind[1][b].type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) { _fetch = YES; _fetchMax = (uint32_t)(b - 192); }
    if (_fetch && _fetchMax >= na) { if (err) *err = rdna4_err(59, [NSString stringWithFormat:@"fragment reads [[color(%u)]] but the pipeline has %u colour attachment(s)", _fetchMax, na]); free(rv); free(rf); return nil; }
    if (_fetch && _pc.samples > 1) { if (err) *err = rdna4_err(59, @"framebuffer fetch ([[color(n)]]) on a multisample pipeline is not supported"); free(rv); free(rf); return nil; }
    VkResult r;
    if (_fetch) {
        for (uint32_t i = 0; i < na; i++) ads[i].initialLayout = ads[i].finalLayout = VK_IMAGE_LAYOUT_GENERAL;
        _rp = rdna4_mk_rp(ads, na, nd, YES);
        if (!_rp) { if (err) *err = rdna4_err(45, @"vkCreateRenderPass (framebuffer fetch)"); free(rv); free(rf); return nil; }
    } else {
    VkSubpassDescription sp = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = na, .pColorAttachments = refs, .pDepthStencilAttachment = nd ? &dref : NULL };
    VkRenderPassCreateInfo rpc = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = na + nd, .pAttachments = ads, .subpassCount = 1, .pSubpasses = &sp };
    r = vkCreateRenderPass(RDNA4R.dev, &rpc, NULL, &_rp);
    if (r != VK_SUCCESS) { if (err) *err = rdna4_err(45, [NSString stringWithFormat:@"vkCreateRenderPass = %d", r]); free(rv); free(rf); return nil; }
    }
    // Layouts: vertex/TES = set 0, fragment = set 1 (both always exist, possibly empty) + push constants when a stage declares a block.
    NSError *le = nil;
    VkShaderStageFlags vsStage = _isTess ? VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT : VK_SHADER_STAGE_VERTEX_BIT;
    if (!rdna4_stage_dsl(rv, 0, vsStage, mv_, &_dsl[0], &_pbV, &_npbV, _owned, &le) ||
        !rdna4_stage_dsl(rf, 1, VK_SHADER_STAGE_FRAGMENT_BIT, mf_, &_dsl[1], &_pbF, &_npbF, _owned, &le)) {
        if (err) *err = le; free(rv); free(rf); return nil; }
    if (!rdna4_argument_table_stages_compatible(mv_, mf_)) {
        if (err) *err = rdna4_err(49,
            @"vertex and fragment GPU argument tables use different ABI tails");
        free(rv); free(rf); return nil;
    }
    _argumentTables.comparisonSamplers = [mv_[@"argument_resource_tables"][@"comparison_sampler"] boolValue] ||
        [mf_[@"argument_resource_tables"][@"comparison_sampler"] boolValue];
    _argumentTables.samplerCoordinates = [mv_[@"normalized_sampler_coordinates"] boolValue] ||
        [mf_[@"normalized_sampler_coordinates"] boolValue];
    if (!rdna4_argument_table_metadata_apply(&_argumentTables, rv, 0, mv_, &le) ||
        !rdna4_argument_table_metadata_apply(&_argumentTables, rf, 1, mf_, &le) ||
        !rdna4_argument_table_layouts(&_argumentTables, rv, rf, NO, &le) ||
        !rdna4_argument_stage_remap(&_argumentTables, _pbV, _npbV, &le) ||
        !rdna4_argument_stage_remap(&_argumentTables, _pbF, _npbF, &le)) {
        if (err) *err = le; free(rv); free(rf); return nil; }
    for (uint32_t i = 0; i < _npbV; i++)
        if (_pbV[i].addressTable && _pbV[i].addressTableVersion == RDNA4_BUFFER_REGISTRY_VERSION) _hasAddressRegistry = YES;
    for (uint32_t i = 0; i < _npbF; i++)
        if (_pbF[i].addressTable && _pbF[i].addressTableVersion == RDNA4_BUFFER_REGISTRY_VERSION) _hasAddressRegistry = YES;
    NSError *scanError = nil;
    if (!rdna4_argument_scan_plan_build(&_scanV, preV, mv_, "vertex", &scanError) ||
        !rdna4_argument_scan_plan_build(&_scanF, preF, mf_, "fragment", &scanError)) {
        if (err) *err = scanError; free(rv); free(rf); return nil;
    }
    if (_isTess) {
        _tessSet = _argumentTables.active ? 5u : 2u;
        _tessCfg.tess_set = _tessSet;
        _tessKey = rdna4_tess_pipeline_cache_key(&_tessCfg, vsd.bytes, vsd.length);
        uint32_t *vsW = NULL, *tcsW = NULL, *tesW = NULL;
        size_t vsNW = 0, tcsNW = 0, tesNW = 0;
        BOOL tessOk = rdna4_tess_emit_vs_spv(&vsW, &vsNW) &&
                      rdna4_tess_emit_tcs_spv(&_tessCfg, &tcsW, &tcsNW) &&
                      rdna4_tess_patch_tes_spv(vsd.bytes, vsd.length / 4, &_tessCfg, &tesW, &tesNW);
        if (!tessOk) {
            free(vsW); free(tcsW); free(tesW);
            if (err) *err = rdna4_err(42, @"failed to emit tessellation adapter SPIR-V");
            free(rv); free(rf); return nil;
        }
        if (_icb && !fb) {
            _icbVsSpv = [NSData dataWithBytes:tesW length:tesNW * 4];
            if (hasFrag) _icbFsSpv = [fsp copy];
        }
        _vs = rdna4_spv_module(vsW, vsNW * 4, "tess-vertex", err);
        _tcs = rdna4_spv_module(tcsW, tcsNW * 4, "tess-control", err);
        _tes = rdna4_spv_module(tesW, tesNW * 4, "tess-eval", err);
        free(vsW); free(tcsW); free(tesW);
        if (!_vs || !_tcs || !_tes) { free(rv); free(rf); return nil; }
        VkDescriptorSetLayoutBinding tb = { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT };
        VkDescriptorSetLayoutCreateInfo td = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &tb };
        r = vkCreateDescriptorSetLayout(RDNA4R.dev, &td, NULL, &_tessDsl);
        if (r != VK_SUCCESS) { if (err) *err = rdna4_err(50, [NSString stringWithFormat:@"vkCreateDescriptorSetLayout (tess) = %d", r]); free(rv); free(rf); return nil; }
    } else if (_icb && !fb) {
        _icbVsSpv = [vsd copy];
        if (hasFrag) _icbFsSpv = [fsp copy];
    }
    uint32_t pcs = 0; VkShaderStageFlags pcf = 0;
    if (rv && rv->pc_size) { pcs = rv->pc_size; pcf |= vsStage; }
    if (rf && rf->pc_size) { if (rf->pc_size > pcs) pcs = rf->pc_size; pcf |= VK_SHADER_STAGE_FRAGMENT_BIT; }
    _pcs = pcs; _pcf = pcf;
    if (pcs > 256) { if (err) *err = rdna4_err(48, [NSString stringWithFormat:@"push constant block %u B > 256", pcs]); free(rv); free(rf); return nil; }
    VkPushConstantRange pcr = { pcf, 0, pcs };
    VkDescriptorSetLayout layouts[6] = {_dsl[0], _dsl[1], _argumentTables.active ? _argumentTables.dsl[0] : _tessDsl, _argumentTables.dsl[1], _argumentTables.dsl[2], _tessDsl};
    uint32_t layoutCount = _argumentTables.active ? (_isTess ? 6u : 5u) : (_isTess ? 3u : 2u);
    VkPipelineLayoutCreateInfo plc = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = layoutCount, .pSetLayouts = layouts,
        .pushConstantRangeCount = pcs ? 1 : 0, .pPushConstantRanges = &pcr };
    r = vkCreatePipelineLayout(RDNA4R.dev, &plc, NULL, &_pl);
    if (r != VK_SUCCESS) { if (err) *err = rdna4_err(46, [NSString stringWithFormat:@"vkCreatePipelineLayout = %d", r]); free(rv); free(rf); return nil; }
    // Vertex input: for every SPIR-V input Location, the descriptor's attribute of that index (format, offset, bufferIndex) and its
    // buffer layout (stride, step function). A Location without a descriptor attribute reads binding 31 (stride 0, a null buffer).
    MTLVertexDescriptor *vd = d.vertexDescriptor; BOOL dummy = NO;
    for (int i = 0; !_isTess && rv && i < rv->nva; i++) {
        uint32_t loc = rv->va[i].location;
        MTLVertexAttributeDescriptor *a = vd ? vd.attributes[loc] : nil;
        if (a && a.format != MTLVertexFormatInvalid) {
            VkFormat vf = rdna4_vkvfmt(a.format); NSUInteger bi = a.bufferIndex;
            if (vf == VK_FORMAT_UNDEFINED || bi >= 31 || a.offset > 2047) {
                if (err) *err = rdna4_err(57, [NSString stringWithFormat:@"vertex attribute %u: format %lu / bufferIndex %lu / offset %lu not supported", loc, (unsigned long)a.format, (unsigned long)bi, (unsigned long)a.offset]);
                free(rv); free(rf); return nil; }
            MTLVertexBufferLayoutDescriptor *l = vd.layouts[bi]; VkVertexInputRate rate = VK_VERTEX_INPUT_RATE_VERTEX; uint32_t stride = (uint32_t)l.stride;
            if (l.stride == (NSUInteger)-1 || l.stride > 2048 || (l.stepFunction != MTLVertexStepFunctionConstant && l.stepFunction != MTLVertexStepFunctionPerVertex && l.stepFunction != MTLVertexStepFunctionPerInstance) ||
                (l.stepFunction == MTLVertexStepFunctionPerInstance && l.stepRate != 1)) {
                if (err) { *err = rdna4_err(58, [NSString stringWithFormat:@"vertex buffer layout %lu: stride %lu step function %lu rate %lu not supported (dynamic stride / step rate > 1 / patch)", (unsigned long)bi,
                                             (unsigned long)l.stride, (unsigned long)l.stepFunction, (unsigned long)l.stepRate]); }
                free(rv); free(rf); return nil; }
            if (l.stepFunction == MTLVertexStepFunctionConstant) stride = 0;
            else if (l.stepFunction == MTLVertexStepFunctionPerInstance) rate = VK_VERTEX_INPUT_RATE_INSTANCE;
            uint32_t k = 0; for (; k < _nvib; k++) if (_vib[k].binding == (uint32_t)bi) break;
            if (k == _nvib) { _vib[_nvib++] = (VkVertexInputBindingDescription){ (uint32_t)bi, stride, rate }; _need[_nneed++] = (uint32_t)bi; }
            _via[_nvia++] = (VkVertexInputAttributeDescription){ loc, (uint32_t)bi, vf, (uint32_t)a.offset };
            if (_icb && _nIcbVAttrs < 32) {
                rdna4_tess_vfmt_t tf = rdna4_tess_vfmt_from_vk((uint32_t)vf);
                if (tf == RDNA4_TFMT_INVALID) {
                    if (err) *err = rdna4_err(57, [NSString stringWithFormat:@"ICB vertex attribute %u format %lu has no BDA decoder", loc, (unsigned long)a.format]);
                    free(rv); free(rf); return nil;
                }
                _icbVAttrs[_nIcbVAttrs++] = (garnetx_icb_vattr_t){
                    .location = loc,
                    .buffer_index = (uint32_t)bi,
                    .offset = (uint32_t)a.offset,
                    .stride = stride,
                    .vfmt = (uint8_t)tf,
                    .step_func = (l.stepFunction == MTLVertexStepFunctionConstant) ? 0u : (l.stepFunction == MTLVertexStepFunctionPerInstance) ? 2u : 1u,
                    .pad = { 0, 0 },
                    .step_rate = (uint32_t)(l.stepRate ? l.stepRate : 1),
                };
            }
        } else {
            RDNA4LOG("pipeline: WARN shader input location %u has no vertex-descriptor attribute: reads a zero-stride null binding", loc);
            if (!dummy) { _vib[_nvib++] = (VkVertexInputBindingDescription){ 31, 0, VK_VERTEX_INPUT_RATE_VERTEX }; _need[_nneed++] = 31; dummy = YES; }
            _via[_nvia++] = (VkVertexInputAttributeDescription){ loc, 31, rv->va[i].format, 0 };
        }
    }
    free(rv); free(rf);
    uint32_t dk0 = [self rdna4DSKey:NULL];   // bundle 10: 0 for a colour-only pipeline (the key below is then exactly the old one)
    VkPrimitiveTopology topo0 = _isTess ? VK_PRIMITIVE_TOPOLOGY_PATCH_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
#ifdef RDNA4_BINARY_ARCHIVE_IMPL_INC
    rdna4_ba_vkcache_scope_t baScope;
    const BOOL useBaCache = baHit && baVk.length > 0;
    if (useBaCache) rdna4_ba_vkcache_scope_begin(&baScope, baVk, &baFp);
#endif
    NSError *ve = nil; VkPipeline p0 = [self rdna4Build:topo0 cull:VK_CULL_MODE_NONE front:VK_FRONT_FACE_CLOCKWISE ds:dk0 error:&ve];
#ifdef RDNA4_BINARY_ARCHIVE_IMPL_INC
    if (useBaCache) rdna4_ba_vkcache_scope_end(&baScope);
#endif
    if (!p0) { if (err) *err = ve; return nil; }
    _variants[@((uint64_t)topo0 | ((uint64_t)VK_CULL_MODE_NONE << 8) | ((uint64_t)VK_FRONT_FACE_CLOCKWISE << 16) | ((uint64_t)dk0 << 24))] = [NSValue valueWithPointer:(void *)p0];
    RDNA4LOG("RDNA4RenderPipelineState %p: pipeline %p (%u colour attachment(s), set0 %u / set1 %u bindings, %u vertex input(s), %u vertex binding(s)%s%s%s)", (__bridge void *)self, (void *)p0, na,
           _npbV, _npbF, _nvia, _nvib, _isTess ? ", TESSELLATION" : "", fb ? ", FALLBACK no-write" : "", _fetch ? ", FRAMEBUFFER FETCH (input attachments)" : "");
    if (fb) {   // hot-swap: remember how to rebuild (descriptor copy: the client may reuse its own) and watch for the .spv files
        NSString *sv = rdna4_fn_sha(d.vertexFunction), *sf = hasFrag ? rdna4_fn_sha(d.fragmentFunction) : nil;
        if (sv && (!hasFrag || sf)) {
            _hsDesc = [d copy];
            NSArray<NSString *> *shas = sf ? @[ sv, sf ] : @[ sv ];
            rdna4hs_register(self, shas, NO, [NSString stringWithFormat:@"%@ / %@", vname, fname], ^BOOL(id o) { return [(RDNA4RenderPipelineState *)o rdna4HotRebuild]; });
        }
    }
    return self;
}
- (uint32_t)rdna4DSKey:(const void *)dsinfo {
    RDNA4DSInfo di = dsinfo ? *(const RDNA4DSInfo *)dsinfo : rdna4_dsinfo_default();
    return rdna4ds_key(_dpf != MTLPixelFormatInvalid, _spf != MTLPixelFormatInvalid, di.cmp, di.write, &di.f, &di.b);
}
- (BOOL)rdna4HasDS { return _dsVk != VK_FORMAT_UNDEFINED; }
- (BOOL)rdna4HasDepth { return _dpf != MTLPixelFormatInvalid; }
- (BOOL)rdna4HasStencil { return _spf != MTLPixelFormatInvalid; }
- (VkFormat)rdna4DSVk { return _dsVk; }
- (unsigned)rdna4SampleCount { return _pc.samples; }
static BOOL rdna4_icb_build_stage_dsl(const RDNA4PB *origPb, uint32_t origN, const RDNA4sRefl *refl, uint32_t set,
                                    VkShaderStageFlags stage, BOOL wantVsParams,
                                    VkDescriptorSetLayout *outDsl, RDNA4PB **outPb, uint32_t *outN, NSError **err) {
    *outDsl = VK_NULL_HANDLE;
    *outPb = NULL;
    *outN = 0;
    if (!refl) {
        VkDescriptorSetLayoutCreateInfo empty = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        VkResult r = vkCreateDescriptorSetLayout(RDNA4R.dev, &empty, NULL, outDsl);
        if (r != VK_SUCCESS && err) *err = rdna4_err(50, [NSString stringWithFormat:@"vkCreateDescriptorSetLayout(icb empty) = %d", r]);
        return r == VK_SUCCESS;
    }
    if (!refl->bind[set][RDNA4_ICB_TABLE_BINDING].used ||
        (wantVsParams && !refl->bind[set][RDNA4_ICB_VS_PARAMS_BINDING].used)) {
        if (err) *err = rdna4_err(49, @"ICB lowered stage missing required dynamic buffer binding 676/677");
        return NO;
    }
    VkDescriptorSetLayoutBinding *bnd = calloc(RDNA4S_MAXBIND, sizeof *bnd);
    RDNA4PB *pb = calloc(RDNA4S_MAXBIND, sizeof *pb);
    if (!bnd || !pb) {
        free(bnd); free(pb);
        if (err) *err = rdna4_err(49, @"out of memory building ICB stage layout");
        return NO;
    }
    uint32_t nbnd = 0, npb = 0;
    for (uint32_t b = 0; b < RDNA4S_MAXBIND; b++) {
        const RDNA4sBind *a = &refl->bind[set][b];
        if (!a->used) continue;
        if (a->runtime) {
            free(bnd); free(pb);
            if (err) *err = rdna4_err(49, @"ICB lowered stage contains unexpected runtime descriptor array");
            return NO;
        }
        if (b == RDNA4_ICB_TABLE_BINDING || (wantVsParams && b == RDNA4_ICB_VS_PARAMS_BINDING)) {
            bnd[nbnd++] = (VkDescriptorSetLayoutBinding){ b, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1, stage, NULL };
            continue;
        }
        const RDNA4PB *match = NULL;
        for (uint32_t j = 0; j < origN; j++) {
            if (origPb[j].binding == b && origPb[j].type == a->type) { match = &origPb[j]; break; }
        }
        if (!match) {
            free(bnd); free(pb);
            if (err) *err = rdna4_err(49, [NSString stringWithFormat:@"ICB lowered set %u binding %u (type %d) missing original metadata", set, b, (int)a->type]);
            return NO;
        }
        bnd[nbnd++] = (VkDescriptorSetLayoutBinding){ b, match->type, match->count, stage, NULL };
        pb[npb++] = *match;
    }
    VkDescriptorSetLayoutCreateInfo dc = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = nbnd, .pBindings = bnd };
    VkResult vr = vkCreateDescriptorSetLayout(RDNA4R.dev, &dc, NULL, outDsl);
    free(bnd);
    if (vr != VK_SUCCESS) {
        free(pb);
        if (err) *err = rdna4_err(50, [NSString stringWithFormat:@"vkCreateDescriptorSetLayout(icb) = %d", vr]);
        return NO;
    }
    if (npb) { *outPb = realloc(pb, npb * sizeof *pb); if (!*outPb) *outPb = pb; *outN = npb; }
    else free(pb);
    return YES;
}
- (void)rdna4DestroyICBGPUState {
    if (RDNA4R.ok) {
        for (NSValue *v in _icbVariants.allValues) vkDestroyPipeline(RDNA4R.dev, (VkPipeline)v.pointerValue, NULL);
        if (_icbPl) vkDestroyPipelineLayout(RDNA4R.dev, _icbPl, NULL);
        for (int i = 0; i < 2; i++) if (_icbDsl[i]) vkDestroyDescriptorSetLayout(RDNA4R.dev, _icbDsl[i], NULL);
        if (_icbTessDsl) vkDestroyDescriptorSetLayout(RDNA4R.dev, _icbTessDsl, NULL);
        if (_icbVs) vkDestroyShaderModule(RDNA4R.dev, _icbVs, NULL);
        if (_icbTes) vkDestroyShaderModule(RDNA4R.dev, _icbTes, NULL);
        if (_icbFs) vkDestroyShaderModule(RDNA4R.dev, _icbFs, NULL);
    }
    [_icbVariants removeAllObjects];
    _icbPl = VK_NULL_HANDLE;
    _icbDsl[0] = _icbDsl[1] = _icbTessDsl = VK_NULL_HANDLE;
    _icbVs = _icbTes = _icbFs = VK_NULL_HANDLE;
    free(_icbPbV); _icbPbV = NULL; _icbNpbV = 0;
    free(_icbPbF); _icbPbF = NULL; _icbNpbF = 0;
    _icbGpuReady = NO;
    _icbHasVsParams = NO;
}
- (BOOL)rdna4PrepareICBGPUVariant:(NSError **)err {
    [_lock lock];
    if (_icbGpuReady) { [_lock unlock]; return YES; }
    if (!_icb || _fallback || !_icbVsSpv) {
        [_lock unlock];
        if (err) *err = rdna4_err(49, @"render pipeline does not support GPU indirect command buffers");
        return NO;
    }
    if (!RDNA4R.bufferAddress) {
        [_lock unlock];
        if (err) *err = rdna4_err(49, @"GPU indirect command buffers require Vulkan bufferDeviceAddress");
        return NO;
    }
    const rdna4x_xlib *xl = rdna4x_ensure_xlib(err);
    if (!xl || !xl->lower_icb_stage) {
        if (xl && err && !*err) *err = rdna4_err(42, @"translator library lacks garnetx_lower_icb_stage");
        [_lock unlock];
        return NO;
    }
    uint8_t *vspv = NULL, *fspv = NULL; size_t vlen = 0, flen = 0;
    RDNA4sMod mv = {0}, mf = {0}; BOOL mvOpen = NO, mfOpen = NO;
    RDNA4sRefl *rv = NULL, *rf = NULL;
    BOOL ok = NO;
    do {
        uint32_t vtbl = 0, vvsp = 0; char xerr[256] = "";
        uint32_t skind = _isTess ? 2u : 0u;
        int rc = xl->lower_icb_stage((const uint8_t *)_icbVsSpv.bytes, _icbVsSpv.length, skind, 0u,
                                     _isTess ? NULL : _icbVAttrs, _isTess ? 0u : _nIcbVAttrs,
                                     &vspv, &vlen, &vtbl, &vvsp, xerr, sizeof xerr);
        if (rc != 0 || !vspv || vlen < 20 || (vlen & 3)) {
            if (err) *err = rdna4_err(42, [NSString stringWithFormat:@"icb lower vertex/tes stage failed (%d): %s", rc, xerr]);
            break;
        }
        if (_hasFrag) {
            if (!_icbFsSpv) { if (err) *err = rdna4_err(42, @"missing cached fragment SPIR-V for ICB lowering"); break; }
            uint32_t ftbl = 0, fvsp = 0; xerr[0] = 0;
            rc = xl->lower_icb_stage((const uint8_t *)_icbFsSpv.bytes, _icbFsSpv.length, 1u, 1u,
                                     NULL, 0u, &fspv, &flen, &ftbl, &fvsp, xerr, sizeof xerr);
            if (rc != 0 || !fspv || flen < 20 || (flen & 3)) {
                if (err) *err = rdna4_err(42, [NSString stringWithFormat:@"icb lower fragment stage failed (%d): %s", rc, xerr]);
                break;
            }
        }
        char why[256] = "";
        rv = calloc(1, sizeof *rv);
        if (!rv) { if (err) *err = rdna4_err(49, @"out of memory reflecting ICB vertex stage"); break; }
        if (rdna4s_parse((const uint32_t *)vspv, vlen / 4, &mv, why, sizeof why) || rdna4s_reflect(&mv, 0, rv)) {
            if (mv.id) mvOpen = YES;
            if (err) *err = rdna4_err(51, [NSString stringWithFormat:@"ICB lowered vertex reflection failed: %s", why[0] ? why : rv->why]);
            break;
        }
        mvOpen = YES;
        if (_hasFrag) {
            rf = calloc(1, sizeof *rf);
            if (!rf) { if (err) *err = rdna4_err(49, @"out of memory reflecting ICB fragment stage"); break; }
            if (rdna4s_parse((const uint32_t *)fspv, flen / 4, &mf, why, sizeof why) || rdna4s_reflect(&mf, 1, rf)) {
                if (mf.id) mfOpen = YES;
                if (err) *err = rdna4_err(52, [NSString stringWithFormat:@"ICB lowered fragment reflection failed: %s", why[0] ? why : rf->why]);
                break;
            }
            mfOpen = YES;
        }
        VkShaderStageFlags vsStage = _isTess ? VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT : VK_SHADER_STAGE_VERTEX_BIT;
        if (!rdna4_icb_build_stage_dsl(_pbV, _npbV, rv, 0, vsStage, !_isTess, &_icbDsl[0], &_icbPbV, &_icbNpbV, err) ||
            !rdna4_icb_build_stage_dsl(_pbF, _npbF, rf, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NO, &_icbDsl[1], &_icbPbF, &_icbNpbF, err))
            break;
        if (_isTess) {
            VkDescriptorSetLayoutBinding tb = { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,
                .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT };
            VkDescriptorSetLayoutCreateInfo td = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &tb };
            VkResult tr = vkCreateDescriptorSetLayout(RDNA4R.dev, &td, NULL, &_icbTessDsl);
            if (tr != VK_SUCCESS) { if (err) *err = rdna4_err(50, [NSString stringWithFormat:@"vkCreateDescriptorSetLayout(icb tess) = %d", tr]); break; }
        }
        VkPushConstantRange pcr = { _pcf, 0, _pcs };
        VkDescriptorSetLayout layouts[6] = { _icbDsl[0], _icbDsl[1],
            _argumentTables.active ? _argumentTables.dsl[0] : _icbTessDsl,
            _argumentTables.dsl[1], _argumentTables.dsl[2], _icbTessDsl };
        uint32_t layoutCount = _argumentTables.active ? (_isTess ? 6u : 5u) : (_isTess ? 3u : 2u);
        VkPipelineLayoutCreateInfo plc = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = layoutCount,
            .pSetLayouts = layouts, .pushConstantRangeCount = _pcs ? 1 : 0, .pPushConstantRanges = &pcr };
        VkResult pr = vkCreatePipelineLayout(RDNA4R.dev, &plc, NULL, &_icbPl);
        if (pr != VK_SUCCESS) { if (err) *err = rdna4_err(46, [NSString stringWithFormat:@"vkCreatePipelineLayout(icb) = %d", pr]); break; }
        if (_isTess) {
            _icbTes = rdna4_spv_module(vspv, vlen, "icb-tess-eval", err);
            if (!_icbTes) break;
        } else {
            _icbVs = rdna4_spv_module(vspv, vlen, "icb-vertex", err);
            if (!_icbVs) break;
        }
        if (_hasFrag) {
            _icbFs = rdna4_spv_module(fspv, flen, "icb-fragment", err);
            if (!_icbFs) break;
        }
        _icbHasVsParams = !_isTess;
        _icbGpuReady = YES;
        ok = YES;
    } while (0);
    if (mvOpen) rdna4s_free(&mv);
    if (mfOpen) rdna4s_free(&mf);
    free(rv); free(rf);
    if (vspv) xl->freep(vspv, vlen);
    if (fspv) xl->freep(fspv, flen);
    if (!ok) [self rdna4DestroyICBGPUState];
    [_lock unlock];
    return ok;
}
- (VkPipeline)rdna4Build:(VkPrimitiveTopology)topo cull:(VkCullModeFlags)cull front:(VkFrontFace)front ds:(uint32_t)dk error:(NSError **)err {
    return [self rdna4Build:topo cull:cull front:front ds:dk rflags:0 icb:NO error:err];
}
- (VkPipeline)rdna4Build:(VkPrimitiveTopology)topo cull:(VkCullModeFlags)cull front:(VkFrontFace)front ds:(uint32_t)dk icb:(BOOL)icb error:(NSError **)err {
    return [self rdna4Build:topo cull:cull front:front ds:dk rflags:0 icb:icb error:err];
}
- (VkPipeline)rdna4Build:(VkPrimitiveTopology)topo cull:(VkCullModeFlags)cull front:(VkFrontFace)front ds:(uint32_t)dk rflags:(uint32_t)rflags icb:(BOOL)icb error:(NSError **)err {
    BOOL fillLine = (rflags & 1u) != 0;
    BOOL depthClamp = (rflags & 2u) != 0;
    BOOL sampleLoc = (rflags & 4u) != 0;
    uint32_t vpCount = ((rflags >> 3) & 0xFu) + 1u;
    if (fillLine && !RDNA4R.fillModeNonSolid) {
        if (err) *err = rdna4_err(47, @"MTLTriangleFillModeLines requires fillModeNonSolid");
        return VK_NULL_HANDLE;
    }
    if (vpCount > (RDNA4R.maxViewports ?: 1u)) {
        if (err) *err = rdna4_err(47, [NSString stringWithFormat:@"viewportCount %u exceeds RADV max %u", vpCount, RDNA4R.maxViewports]);
        return VK_NULL_HANDLE;
    }
    if (sampleLoc && (!RDNA4R.sampleLocationsExt || !(RDNA4R.sampleLocationCounts & _pc.samples))) {
        if (err) *err = rdna4_err(47, @"programmable sample positions unsupported for sample count");
        return VK_NULL_HANDLE;
    }
    VkPipelineShaderStageCreateInfo st[4];
    uint32_t stCount = 0;
    VkShaderModule vsMod = (icb && !_isTess) ? _icbVs : _vs;
    VkShaderModule tesMod = (icb && _isTess) ? _icbTes : _tes;
    VkShaderModule fsMod = (icb && _hasFrag) ? _icbFs : _fs;
    if (_isTess) {
        st[stCount++] = (VkPipelineShaderStageCreateInfo){ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = _vs, .pName = "main" };
        st[stCount++] = (VkPipelineShaderStageCreateInfo){ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, .module = _tcs, .pName = "main" };
        st[stCount++] = (VkPipelineShaderStageCreateInfo){ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, .module = tesMod, .pName = _vep };
        if (fsMod) st[stCount++] = (VkPipelineShaderStageCreateInfo){ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fsMod, .pName = _fep };
    } else {
        st[stCount++] = (VkPipelineShaderStageCreateInfo){ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vsMod, .pName = _vep };
        if (fsMod) st[stCount++] = (VkPipelineShaderStageCreateInfo){ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fsMod, .pName = _fep };
    }
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = icb ? 0 : _nvib, .pVertexBindingDescriptions = icb ? NULL : _vib,
        .vertexAttributeDescriptionCount = icb ? 0 : _nvia, .pVertexAttributeDescriptions = icb ? NULL : _via };
    BOOL strip = !icb && (topo == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP || topo == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP);   // Metal restarts indexed strips at the max index value
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = topo, .primitiveRestartEnable = strip };
    VkPipelineTessellationDomainOriginStateCreateInfo tdo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_DOMAIN_ORIGIN_STATE_CREATE_INFO,
        .domainOrigin = VK_TESSELLATION_DOMAIN_ORIGIN_LOWER_LEFT };
    VkPipelineTessellationStateCreateInfo ts = { .sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO, .pNext = &tdo, .patchControlPoints = 1 };
    VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = vpCount, .scissorCount = vpCount };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable = (depthClamp && RDNA4R.depthClamp) ? VK_TRUE : VK_FALSE,
        .polygonMode = fillLine ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL,
        .rasterizerDiscardEnable = _rastOff ? VK_TRUE : VK_FALSE,
        .cullMode = cull, .frontFace = front, .lineWidth = 1.0f, .depthBiasEnable = _pc.depthBias ? VK_TRUE : VK_FALSE };   // bundle 10: the bias VALUES are dynamic (setDepthBias:), on for depth pipelines only
    VkSampleLocationEXT defSL[8];
    for (uint32_t s = 0; s < _pc.samples && s < 8; s++) defSL[s] = (VkSampleLocationEXT){ 0.5f, 0.5f };
    VkPipelineSampleLocationsStateCreateInfoEXT psli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SAMPLE_LOCATIONS_STATE_CREATE_INFO_EXT,
        .sampleLocationsEnable = VK_TRUE,
        .sampleLocationsInfo = { .sType = VK_STRUCTURE_TYPE_SAMPLE_LOCATIONS_INFO_EXT,
            .sampleLocationsPerPixel = (VkSampleCountFlagBits)_pc.samples,
            .sampleLocationGridSize = { 1, 1 },
            .sampleLocationsCount = _pc.samples, .pSampleLocations = defSL } };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .pNext = sampleLoc ? &psli : NULL,
        .rasterizationSamples = (VkSampleCountFlagBits)_pc.samples, .alphaToCoverageEnable = _pc.alphaToCoverage ? VK_TRUE : VK_FALSE };
    rdna4ds_vk_t dv; rdna4ds_decode(dk, &dv);
    VkPipelineDepthStencilStateCreateInfo dss = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = dv.depthTest ? VK_TRUE : VK_FALSE, .depthWriteEnable = dv.depthWrite ? VK_TRUE : VK_FALSE, .depthCompareOp = (VkCompareOp)dv.depthCompareOp, .stencilTestEnable = dv.stencilTest ? VK_TRUE : VK_FALSE,
        .front = { (VkStencilOp)dv.front.failOp, (VkStencilOp)dv.front.passOp, (VkStencilOp)dv.front.depthFailOp, (VkCompareOp)dv.front.compareOp, 0, 0, 0 },
        .back = { (VkStencilOp)dv.back.failOp, (VkStencilOp)dv.back.passOp, (VkStencilOp)dv.back.depthFailOp, (VkCompareOp)dv.back.compareOp, 0, 0, 0 } };
    VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .flags = _fetch && RDNA4R.orderedColorAccess ? VK_PIPELINE_COLOR_BLEND_STATE_CREATE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_BIT_EXT : 0,
        .attachmentCount = _na, .pAttachments = _cba };
    VkDynamicState dyn[9]; uint32_t ndyn = 0;
    for (unsigned i = 0; i < _pc.ndyn && i < 8; i++) dyn[ndyn++] = (VkDynamicState)_pc.dyn[i];
    if (sampleLoc) dyn[ndyn++] = VK_DYNAMIC_STATE_SAMPLE_LOCATIONS_EXT;
    VkPipelineDynamicStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = ndyn, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo gpc = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = stCount, .pStages = st, .pVertexInputState = &vi,
        .pInputAssemblyState = &ia, .pTessellationState = _isTess ? &ts : NULL, .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = _pc.hasDS ? &dss : NULL, .pColorBlendState = &cb,
        .pDynamicState = &ds, .layout = icb ? _icbPl : _pl, .renderPass = _rp };
    VkPipelineCache pc = RDNA4R.pc;
#ifdef RDNA4_BINARY_ARCHIVE_IMPL_INC
    pc = rdna4_ba_vkcache();
#endif
    VkPipeline p = VK_NULL_HANDLE; uint64_t t_ = rdna4_now(); VkResult r = vkCreateGraphicsPipelines(RDNA4R.dev, pc, 1, &gpc, NULL, &p); rdna4t_add(&T1.gfx, rdna4_now() - t_); atomic_fetch_add(&rdna4_pipes_new, 1);
    if (r != VK_SUCCESS) { if (err) *err = rdna4_err(47, [NSString stringWithFormat:@"vkCreateGraphicsPipelines = %d", r]); return VK_NULL_HANDLE; }
    return p;
}
- (VkPipeline)pipelineForTopology:(VkPrimitiveTopology)topo cull:(VkCullModeFlags)cull front:(VkFrontFace)front { return [self pipelineForTopology:topo cull:cull front:front ds:[self rdna4DSKey:NULL] rflags:0]; }
- (VkPipeline)pipelineForTopology:(VkPrimitiveTopology)topo cull:(VkCullModeFlags)cull front:(VkFrontFace)front ds:(uint32_t)dk { return [self pipelineForTopology:topo cull:cull front:front ds:dk rflags:0]; }
- (VkPipeline)pipelineForTopology:(VkPrimitiveTopology)topo cull:(VkCullModeFlags)cull front:(VkFrontFace)front ds:(uint32_t)dk rflags:(uint32_t)rflags {
    NSNumber *k = @((uint64_t)topo | ((uint64_t)cull << 8) | ((uint64_t)front << 16) | ((uint64_t)dk << 24) | ((uint64_t)(rflags & 0x7Fu) << 56));
    [_lock lock];
    NSValue *v = _variants[k];
    if (!v) {
        NSError *e = nil; VkPipeline p = [self rdna4Build:topo cull:cull front:front ds:dk rflags:rflags icb:NO error:&e];
        if (!p) RDNA4LOG("pipeline variant (topology %d cull %d front %d rflags 0x%x) failed: %s", topo, cull, front, rflags, e.localizedDescription.UTF8String);
        else { v = [NSValue valueWithPointer:(void *)p]; _variants[k] = v; RDNA4LOG("pipeline variant topology %d cull %d front %d rflags 0x%x created", topo, cull, front, rflags); }
    }
    [_lock unlock];
    return v ? (VkPipeline)v.pointerValue : VK_NULL_HANDLE;
}
- (VkPipeline)icbPipelineForTopology:(VkPrimitiveTopology)topo cull:(VkCullModeFlags)cull front:(VkFrontFace)front ds:(uint32_t)dk {
    NSNumber *k = @((uint64_t)topo | ((uint64_t)cull << 8) | ((uint64_t)front << 16) | ((uint64_t)dk << 24));
    [_lock lock];
    if (!_icbGpuReady) { [_lock unlock]; return VK_NULL_HANDLE; }
    NSValue *v = _icbVariants[k];
    if (!v) {
        NSError *e = nil; VkPipeline p = [self rdna4Build:topo cull:cull front:front ds:dk rflags:0 icb:YES error:&e];
        if (!p) RDNA4LOG("icb pipeline variant (topology %d cull %d front %d) failed: %s", topo, cull, front, e.localizedDescription.UTF8String);
        else { v = [NSValue valueWithPointer:(void *)p]; _icbVariants[k] = v; RDNA4LOG("icb pipeline variant topology %d cull %d front %d created", topo, cull, front); }
    }
    [_lock unlock];
    return v ? (VkPipeline)v.pointerValue : VK_NULL_HANDLE;
}
- (void)dealloc {
    [self rdna4DestroyICBGPUState];
    if (RDNA4R.ok) {
        for (NSValue *v in _variants.allValues) vkDestroyPipeline(RDNA4R.dev, (VkPipeline)v.pointerValue, NULL);
        if (_pl) vkDestroyPipelineLayout(RDNA4R.dev, _pl, NULL);
        for (int i = 0; i < 2; i++) if (_dsl[i]) vkDestroyDescriptorSetLayout(RDNA4R.dev, _dsl[i], NULL);
        if (_tessDsl) vkDestroyDescriptorSetLayout(RDNA4R.dev, _tessDsl, NULL);
        rdna4_argument_tables_destroy(&_argumentTables);
        for (NSValue *v in _owned) rdna4_destroy_sampler((VkSampler)v.pointerValue, NULL);
        if (_rp) vkDestroyRenderPass(RDNA4R.dev, _rp, NULL);
        if (_vs) vkDestroyShaderModule(RDNA4R.dev, _vs, NULL);
        if (_tcs) vkDestroyShaderModule(RDNA4R.dev, _tcs, NULL);
        if (_tes) vkDestroyShaderModule(RDNA4R.dev, _tes, NULL);
        if (_fs) vkDestroyShaderModule(RDNA4R.dev, _fs, NULL);
    }
    rdna4_argument_scan_reflection_plan_destroy(&_scanV);
    rdna4_argument_scan_reflection_plan_destroy(&_scanF);
    rdna4vft_refl_destroy(&_vftV);
    rdna4vft_refl_destroy(&_vftF);
    rdna4rtr_refl_destroy(&_rtV);
    rdna4rtr_refl_destroy(&_rtF);
    free(_pbV); free(_pbF);
    uintptr_t rp = rdna4hs_load(&_realp); if (rp) CFRelease((CFTypeRef)rp);   // the published real object
}
RDNA4_DNR(RDNA4RenderPipelineState)
- (VkPipeline)vkPipeline { return [self pipelineForTopology:_isTess ? VK_PRIMITIVE_TOPOLOGY_PATCH_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST cull:VK_CULL_MODE_NONE front:VK_FRONT_FACE_CLOCKWISE]; }
- (id)reflection { return _reflection; }
- (void)rdna4SetReflection:(MTLRenderPipelineReflection *)reflection { _reflection = reflection; }
- (BOOL)rdna4IsTessellation { return _isTess; }
- (const rdna4_tess_cfg_t *)tessCfg { return &_tessCfg; }
- (VkDescriptorSetLayout)tessDsl { return _tessDsl; }
- (uint32_t)tessSet { return _tessSet; }
- (uint64_t)tessCacheKey { return _tessKey; }
- (VkPipelineLayout)layout { return _pl; }
- (VkDescriptorSetLayout)dsl:(int)i { return _dsl[i]; }
- (uint64_t)rdna4ICBToken { return _icbToken; }
- (BOOL)rdna4ICBHasVsParams { return _icbHasVsParams; }
- (VkPipelineLayout)icbLayout { return _icbPl; }
- (VkDescriptorSetLayout)icbDsl:(int)i { return _icbDsl[i]; }
- (VkDescriptorSetLayout)icbTessDsl { return _icbTessDsl; }
- (const RDNA4PB *)icbPbV { return _icbPbV; } - (uint32_t)icbNpbV { return _icbNpbV; } - (const RDNA4PB *)icbPbF { return _icbPbF; } - (uint32_t)icbNpbF { return _icbNpbF; }
- (const RDNA4ArgumentTables *)argumentTables { return &_argumentTables; }
- (const rdna4_argument_scan_reflection_plan *)scanPlanV { return &_scanV; }
- (const rdna4_argument_scan_reflection_plan *)scanPlanF { return &_scanF; }
- (BOOL)rdna4HasAddressRegistry { return _hasAddressRegistry; }
- (BOOL)fetch { return _fetch; } - (uint32_t)fetchMax { return _fetchMax; }
- (const RDNA4PB *)pbV { return _pbV; } - (uint32_t)npbV { return _npbV; } - (const RDNA4PB *)pbF { return _pbF; } - (uint32_t)npbF { return _npbF; }
- (const uint32_t *)needBindings { return _need; } - (uint32_t)nNeed { return _nneed; }
- (id)device { return _dev; }
- (NSString *)label { return _lbl; }
- (void)setLabel:(NSString *)l { _lbl = [l copy]; }
- (NSUInteger)maxTotalThreadsPerThreadgroup { return 0; }
- (NSUInteger)threadgroupSizeMatchesTileSize { return 0; }
- (NSUInteger)imageblockSampleLength { return 0; }
- (BOOL)supportIndirectCommandBuffers { return _icb; }
- (uint32_t)rdna4MaxVertexAmplificationCount { return _maxAmp ? _maxAmp : 1u; }
- (BOOL)rdna4IsMeshPipeline { return NO; }
- (const rdna4rtr_as_reflection_t *)rdna4RtAsReflV { return &_rtV.as; }
- (const rdna4rtr_as_reflection_t *)rdna4RtAsReflF { return &_rtF.as; }
- (id<MTLFunctionHandle>)functionHandleWithFunction:(id<MTLFunction>)function stage:(MTLRenderStages)stage {
    RDNA4RenderPipelineState *real = [self rdna4Real];
    if (real) return [real functionHandleWithFunction:function stage:stage];
    if (function && [function respondsToSelector:@selector(functionType)] &&
        [function functionType] == MTLFunctionTypeIntersection) {
        const rdna4rtr_ift_reflection_t *ir = (stage == MTLRenderStageVertex) ? &_rtV.ift :
            (stage == MTLRenderStageFragment) ? &_rtF.ift : NULL;
        return rdna4_rt_pipeline_function_handle(self, _dev, _iftOwnerToken, ir, function, (NSUInteger)stage);
    }
    const rdna4vft_reflection_t *r = (stage == MTLRenderStageVertex) ? &_vftV :
        (stage == MTLRenderStageFragment) ? &_vftF : NULL;
    return rdna4_vft_pipeline_handle(self, _dev, _vftOwnerToken, r, function, (NSUInteger)stage);
}
- (id<MTLVisibleFunctionTable>)newVisibleFunctionTableWithDescriptor:(MTLVisibleFunctionTableDescriptor *)descriptor stage:(MTLRenderStages)stage {
    RDNA4RenderPipelineState *real = [self rdna4Real];
    if (real) return [real newVisibleFunctionTableWithDescriptor:descriptor stage:stage];
    if (stage != MTLRenderStageVertex && stage != MTLRenderStageFragment) return nil;
    const rdna4vft_reflection_t *r = (stage == MTLRenderStageVertex) ? &_vftV : &_vftF;
    return rdna4_vft_pipeline_new_table(self, _dev, _vftOwnerToken, r, descriptor, (NSUInteger)stage);
}
- (id<MTLIntersectionFunctionTable>)newIntersectionFunctionTableWithDescriptor:(MTLIntersectionFunctionTableDescriptor *)descriptor stage:(MTLRenderStages)stage {
    RDNA4RenderPipelineState *real = [self rdna4Real];
    if (real) return [real newIntersectionFunctionTableWithDescriptor:descriptor stage:stage];
    if (stage != MTLRenderStageVertex && stage != MTLRenderStageFragment) return nil;
    const rdna4rtr_ift_reflection_t *ir = (stage == MTLRenderStageVertex) ? &_rtV.ift : &_rtF.ift;
    return rdna4_rt_pipeline_new_ift(self, _dev, _iftOwnerToken, ir, descriptor, (NSUInteger)stage);
}
@end

// ---------------------------------------------------------------------------------------------------------------
// RDNA4ComputePipelineState (11e R2): SPIR-V + meta sidecar from spvcache; ThreadsDynamic modules take the local size as spec ids 0..2.
// ---------------------------------------------------------------------------------------------------------------

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

GARNET_INTERNAL NSArray *rdna4_icb_compute_reg_for_device(id dev) {
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
    for (uint32_t i = 0; i < _npb; i++)
        if (_pb[i].addressTable && _pb[i].addressTableVersion == RDNA4_BUFFER_REGISTRY_VERSION) _hasAddressRegistry = YES;
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
- (BOOL)rdna4HasAddressRegistry { return _hasAddressRegistry; }
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
GARNET_INTERNAL id rdna4_ba_compile_compute_state(id dev, id fn, NSData *spv, NSDictionary *meta, NSString *fname, NSError **err) {
    return [[RDNA4ComputePipelineState alloc] initWithDevice:dev function:fn spv:spv meta:meta fname:fname error:err];
}
#endif

// ---------------------------------------------------------------------------------------------------------------
// Bound-resource state shared by the render and compute encoders, descriptor-set filling, dummy resources (11e).
// ---------------------------------------------------------------------------------------------------------------

GARNET_INTERNAL VkSampler rdna4_sampler_object_handle(id object, BOOL comparison,
                                             BOOL normalized, NSError **err) {
    if ([object isKindOfClass:[RDNA4SamplerBinding class]])
        return [(RDNA4SamplerBinding *)object rdna4SamplerComparison:comparison
            normalizedCoordinates:normalized error:err];
    if ([object isKindOfClass:[RDNA4SamplerState class]])
        return normalized
            ? [(RDNA4SamplerState *)object rdna4SamplerComparison:comparison
                normalizedCoordinates:YES error:err]
            : [(RDNA4SamplerState *)object rdna4SamplerComparison:comparison error:err];
    if (object && err) *err = rdna4_err(71, @"bound sampler has an invalid class");
    return VK_NULL_HANDLE;
}

GARNET_INTERNAL BOOL rdna4_sampler_object_pixel(id object) {
    if ([object isKindOfClass:[RDNA4SamplerBinding class]])
        return [(RDNA4SamplerBinding *)object rdna4PixelCoordinates];
    if ([object isKindOfClass:[RDNA4SamplerState class]])
        return [(RDNA4SamplerState *)object rdna4PixelCoordinates];
    return NO;
}



GARNET_INTERNAL void rdna4_argument_scan_prepared_destroy(RDNA4ArgumentScanPrepared *prepared) {
    if (!prepared) return;
    for (size_t i = 0; i < prepared->count; ++i)
        free(prepared->stages[i].works);
    memset(prepared, 0, sizeof(*prepared));
}

GARNET_INTERNAL uint8_t rdna4_argument_scan_storage(MTLStorageMode mode) {
    switch (mode) {
    case MTLStorageModeShared: return RDNA4_AS_STORAGE_SHARED;
    case MTLStorageModeManaged: return RDNA4_AS_STORAGE_MANAGED;
    case MTLStorageModePrivate: return RDNA4_AS_STORAGE_PRIVATE;
    case MTLStorageModeMemoryless: return RDNA4_AS_STORAGE_MEMORYLESS;
    default: return RDNA4_AS_STORAGE_UNKNOWN;
    }
}

GARNET_INTERNAL BOOL rdna4_argument_scan_writable(NSSet *writable, RDNA4Buffer *buffer) {
    if (!writable || !buffer) return NO;
    if ([writable containsObject:buffer]) return YES;
    id heap = buffer.heap;
    return heap && [writable containsObject:heap];
}

static void rdna4_argument_scan_bounds(rdna4_as_bound bounds[RDNA4_AS_ROOTS],
                                       const RDNA4StageState *state,
                                       NSSet *writable, id device) {
    memset(bounds, 0, RDNA4_AS_ROOTS * sizeof(*bounds));
    for (uint32_t root = 0; root < RDNA4_AS_ROOTS; ++root) {
        RDNA4Buffer *buffer = state->buf[root].b;
        if (!buffer) continue;
        rdna4_as_bound *bound = &bounds[root];
        bound->present = true;
        bound->length = (uint64_t)buffer.length;
        bound->binding_offset = (uint64_t)state->buf[root].off;
        bound->writable = rdna4_argument_scan_writable(writable, buffer);
        bound->storage = rdna4_argument_scan_storage(buffer.storageMode);
        bound->mapped = buffer.contents != NULL && buffer.device == device &&
            ![buffer isAliasable];
        if ([buffer isAliasable]) {
            bound->mapped = false;
            bound->storage = RDNA4_AS_STORAGE_UNKNOWN;
        }
    }
}

static BOOL rdna4_argument_scan_prepare_stage(RDNA4ArgumentScanStage *prepared,
                                              const RDNA4StageState *state,
                                              NSSet *writable, id device,
                                              NSString **why) {
    if (!prepared || !state || !state->scanPlan ||
        !state->scanPlan->plan.count) return YES;
    size_t capacity = 0;
    rdna4_as_status status = rdna4_as_work_capacity(&state->scanPlan->plan, 0,
                                                     &capacity);
    if (status != RDNA4_AS_OK || !capacity) {
        if (why) *why = [NSString stringWithFormat:
            @"argument scan work capacity: %s",
            rdna4_argument_scan_status_name(status)];
        return NO;
    }
    if (capacity > SIZE_MAX / sizeof(rdna4_as_work)) {
        if (why) *why = @"argument scan work capacity overflows host size";
        return NO;
    }
    rdna4_as_work *works = calloc(capacity, sizeof *works);
    if (!works) {
        if (why) *why = @"argument scan work allocation failed";
        return NO;
    }
    rdna4_as_bound bounds[RDNA4_AS_ROOTS];
    rdna4_argument_scan_bounds(bounds, state, writable, device);
    rdna4_as_work_stats stats;
    size_t count = 0;
    status = rdna4_argument_scan_reflection_plan_materialize(
        state->scanPlan, bounds, NULL, 0, works, capacity, &count, &stats);
    if (status != RDNA4_AS_OK) {
        free(works);
        if (why) *why = [NSString stringWithFormat:
            @"argument scan materialization: %s",
            rdna4_argument_scan_status_name(status)];
        return NO;
    }
    prepared->state = state;
    if (!count) {
        free(works);
        return YES;
    }
    prepared->works = works;
    prepared->count = count;
    prepared->stats = stats;
    RDNA4LOGR("argscan work: %llu work, %llu CPU/%llu GPU/%llu opaque, %llu element, %llu touched B over %llu B, fallback %llu, unknown %llu, missing-root %llu, invalid-span %llu, capacity %llu%s",
        (unsigned long long)stats.works,
        (unsigned long long)stats.cpu_works,
        (unsigned long long)stats.gpu_works,
        (unsigned long long)stats.opaque_works,
        (unsigned long long)stats.elements,
        (unsigned long long)stats.touched_bytes,
        (unsigned long long)stats.extent_bytes,
        (unsigned long long)stats.fallback_works,
        (unsigned long long)stats.unknown_works,
        (unsigned long long)stats.missing_roots,
        (unsigned long long)stats.invalid_spans,
        (unsigned long long)stats.capacity_rejections,
        stats.saturated ? " (saturated)" : "");
    return YES;
}

GARNET_INTERNAL BOOL rdna4_argument_scan_prepare(RDNA4ArgumentScanPrepared *out,
                                        const RDNA4StageState *const stages[],
                                        size_t stageCount, NSSet *writable,
                                        id device, NSString **why) {
    if (!out || (!stages && stageCount) || stageCount > 2) return NO;
    memset(out, 0, sizeof(*out));
    for (size_t i = 0; i < stageCount; ++i) {
        RDNA4ArgumentScanStage stage = {0};
        if (!rdna4_argument_scan_prepare_stage(&stage, stages[i], writable,
                                               device, why)) {
            rdna4_argument_scan_prepared_destroy(out);
            return NO;
        }
        if (stage.state) out->stages[out->count++] = stage;
    }
    return YES;
}



static BOOL rdna4_embedded_token(const RDNA4PB *p, uint32_t element, const RDNA4StageState *st, id device, uint64_t *token) {
    *token = 0;
    if (!p->embedded || element >= p->count || p->embeddedBuffer >= 31) return NO;
    RDNA4BufSlot slot = st->buf[p->embeddedBuffer];
    RDNA4Buffer *buffer = slot.b;
    if (!buffer || buffer.device != device || ![buffer contents] || slot.off > buffer.length || p->embeddedOffset > NSUIntegerMax - slot.off) return NO;
    NSUInteger start = slot.off + p->embeddedOffset;
    if ((start & (sizeof(uint64_t) - 1)) != 0 || element > (NSUIntegerMax - start) / sizeof(uint64_t)) return NO;
    NSUInteger at = start + (NSUInteger)element * sizeof(uint64_t);
    if (at > buffer.length || sizeof(uint64_t) > buffer.length - at) return NO;
    memcpy(token, (const uint8_t *)[buffer contents] + at, sizeof *token);
    return YES;
}

GARNET_INTERNAL id rdna4_embedded_resource(const RDNA4PB *p, uint32_t element, const RDNA4StageState *st, BOOL texture, id device) {
    uint64_t token = 0;
    if (!rdna4_embedded_token(p, element, st, device, &token) || !token) return nil;
    return rdna4_argument_resource_for_token(token, texture, device);
}

GARNET_INTERNAL BOOL rdna4nc_drop_write(id resource, RDNA4NCOp op, const char *operation, const char *encoder) {
    if (![resource rdna4HostReadOnly]) return NO;
    static _Atomic uint64_t dropped[RDNA4NC_OP_COUNT];
    uint64_t n = atomic_fetch_add(&dropped[op], 1);
    if (n < 8 || (n & 1023u) == 0) {
        NSString *label = [resource label];
        RDNA4LOG("-[MTLDebug%s ] validation: %s writes read-only no-copy buffer %p (%s) — command dropped",
                 encoder, operation, (__bridge void *)resource, label ? label.UTF8String : "unlabeled");
    }
    return YES;
}

// Use existing optional Metal argument reflection. Descriptor types alone do not prove write access.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
GARNET_INTERNAL NSArray *rdna4nc_stage_arguments(id reflection, RDNA4NCOp stage) {
    if (stage == RDNA4NC_COMPUTE) return [(MTLComputePipelineReflection *)reflection arguments];
    if (stage == RDNA4NC_VERTEX) return [(MTLRenderPipelineReflection *)reflection vertexArguments];
    return [(MTLRenderPipelineReflection *)reflection fragmentArguments];
}
GARNET_INTERNAL BOOL rdna4nc_stage_writes(const RDNA4StageState *state, NSArray *arguments, RDNA4NCOp op,
                                 const char *stage, uint32_t *warned) {
    if (!rdna4nc_any_readonly_buffer()) return NO;
    BOOL drop = NO;
    for (unsigned i = 0; i < 31; i++) {
        RDNA4Buffer *buffer = state->buf[i].b;
        if (!buffer || ![buffer rdna4HostReadOnly]) continue;
        BOOL known = NO;
        for (MTLArgument *argument in arguments) {
            if (argument.type != MTLArgumentTypeBuffer || argument.index != i) continue;
            known = YES;
            if (argument.access == MTLArgumentAccessWriteOnly || argument.access == MTLArgumentAccessReadWrite)
                drop |= rdna4nc_drop_write(buffer, op, stage, op == RDNA4NC_COMPUTE ? "ComputeCommandEncoder" : "RenderCommandEncoder");
            break;
        }
        if (!known && !(*warned & (1u << i))) {
            *warned |= 1u << i;
            RDNA4LOG("%s: cannot tell if binding %u writes a read-only buffer %p", stage, i, (__bridge void *)buffer);
        }
    }
    return drop;
}

GARNET_INTERNAL void rdna4_argument_stage_resources(NSMutableSet *resources, NSMutableSet *writable,
        const RDNA4PB *bindings, uint32_t count, const RDNA4StageState *state, NSArray *arguments) {
    uint32_t known = 0;
    for (MTLArgument *argument in arguments) {
        if (argument.type != MTLArgumentTypeBuffer || !argument.active || argument.index >= 31) continue;
        uint32_t index = (uint32_t)argument.index;
        known |= 1u << index;
        RDNA4Buffer *buffer = state->buf[index].b;
        if (!buffer) continue;
        [resources addObject:buffer];
        if (argument.access != MTLArgumentAccessReadOnly) [writable addObject:buffer];
    }
    for (uint32_t n = 0; n < count; ++n) {
        const RDNA4PB *binding = &bindings[n];
        uint32_t textureBase = binding->type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ? 32 : 480;
        if (!binding->embedded && (binding->type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || binding->type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) &&
            binding->binding >= textureBase && binding->binding < textureBase + 128) {
            uint32_t first = binding->binding - textureBase;
            for (uint32_t k = 0; k < binding->count && k < 128 - first; ++k)
                if (state->tex[first + k]) [resources addObject:state->tex[first + k]];
        }
        if (binding->type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER && binding->type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) continue;
        uint32_t first = binding->argumentRoot ? binding->embeddedBuffer : binding->binding;
        for (uint32_t k = 0; first < 31 && k < binding->count && k < 31 - first; ++k) {
            uint32_t index = first + k;
            RDNA4Buffer *buffer = state->buf[index].b;
            if (!buffer) continue;
            [resources addObject:buffer];
            if (!(known & (1u << index))) [writable addObject:buffer];
        }
    }
}

#pragma clang diagnostic pop

// Fallbacks for slots a shader reads but the client never bound. Metal reads zeros / a black texel there; a garbage descriptor could
// fault the GPU, so a zero-filled buffer, a 1x1 texel and a default sampler stand in (each use is logged as a WARN).
GARNET_INTERNAL RDNA4Buffer *rdna4_dummy_buffer(id dev) {
    static RDNA4Buffer *b;
    @synchronized ([GarnetDevice class]) {
        if (!b) { NSError *e = nil; b = [[RDNA4Buffer alloc] initWithDevice:dev length:65536 options:MTLResourceStorageModeShared error:&e];
                  if (b) memset([b contents], 0, 65536); else RDNA4LOG("dummy buffer failed: %s", e.localizedDescription.UTF8String); }
    }
    return b;
}
GARNET_INTERNAL RDNA4Texture *rdna4_dummy_texture(id dev) {
    static RDNA4Texture *t;
    static id owner;
    @synchronized ([GarnetDevice class]) {
        if (!t || owner != dev) {
            owner = dev;
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];
            td.usage = MTLTextureUsageShaderRead; td.storageMode = MTLStorageModeShared;
            NSError *e = nil; t = [[RDNA4Texture alloc] initWithDevice:dev descriptor:td error:&e];
            if (t) { uint32_t z = 0; [t replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&z bytesPerRow:4]; }
            else RDNA4LOG("dummy texture failed: %s", e.localizedDescription.UTF8String);
        }
    }
    return t;
}
static const RDNA4Fmt *rdna4_dummy_storage_format(void) {
    static const MTLPixelFormat formats[] = {
        MTLPixelFormatR32Uint, MTLPixelFormatR32Sint, MTLPixelFormatR32Float, MTLPixelFormatRGBA8Unorm
    };
    for (size_t i = 0; i < sizeof formats / sizeof *formats; ++i) {
        const RDNA4Fmt *f = rdna4_fmt(formats[i]);
        if (f && (rdna4_fmt_feats(f) & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) return f;
    }
    return NULL;
}
GARNET_INTERNAL RDNA4Texture *rdna4_dummy_texture_write(id dev) {
    static RDNA4Texture *t;
    static id owner;
    @synchronized ([GarnetDevice class]) {
        if (!t || owner != dev) {
            owner = dev;
            const RDNA4Fmt *f = rdna4_dummy_storage_format();
            if (!f) {
                RDNA4LOG("dummy shader-write texture unavailable: no storage-image format is supported");
                t = nil;
                return t;
            }
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f->mtl width:1 height:1 mipmapped:NO];
            td.usage = MTLTextureUsageShaderWrite; td.storageMode = MTLStorageModePrivate;
            NSError *e = nil; t = [[RDNA4Texture alloc] initWithDevice:dev descriptor:td error:&e];
            if (!t) RDNA4LOG("dummy shader-write texture failed: %s", e.localizedDescription.UTF8String);
        }
    }
    return t;
}

typedef struct {
    MTLTextureType textureType;
    MTLPixelFormat pixelFormat;
    MTLTextureUsage usage;
    MTLStorageMode storageMode;
    NSUInteger sampleCount;
    BOOL depth;
    BOOL storage;
} RDNA4DummyImagePlan;

// The 48 ABI-5 shape descriptors describe float/uint/int, dimension/array, and depth.
// Sample count is a separate axis because the ABI-5 shape word has no MS bit.
static BOOL rdna4_dummy_image_plan_make(uint32_t shape, BOOL storage, uint32_t samples,
                                        RDNA4DummyImagePlan *p, const char **why) {
    if (!p) return NO;
    memset(p, 0, sizeof *p);
    if (why) *why = NULL;
    BOOL depth = NO;
    if (!rdna4_argument_shape(shape, NULL, &depth)) {
        if (why) *why = "not a legal ABI-5 image shape";
        return NO;
    }
    if (!samples) samples = 1;
    if (samples != 1 && samples != 2 && samples != 4 && samples != 8) {
        if (why) *why = "sample count must be 1, 2, 4, or 8";
        return NO;
    }
    uint32_t component = shape / 16, dim = (shape & 7u) >> 1, arrayed = shape & 1u;
    if (depth && dim != 1 && dim != 3) {
        if (why) *why = "depth formats are 2D or cube, including arrays";
        return NO;
    }
    if (storage && depth) {
        if (why) *why = "depth images cannot be storage images";
        return NO;
    }
    if (samples > 1 && dim != 1) {
        if (why) *why = "multisample dummies are 2D, including arrays";
        return NO;
    }
    if (storage && samples > 1) {
        if (why) *why = "storage images cannot be multisampled";
        return NO;
    }
    MTLTextureType type = samples > 1 ? (arrayed ? (MTLTextureType)RDNA4TD_MTL_2DMSARRAY : MTLTextureType2DMultisample) :
        dim == 0 ? (arrayed ? MTLTextureType1DArray : MTLTextureType1D) :
        dim == 1 ? (arrayed ? MTLTextureType2DArray : MTLTextureType2D) :
        dim == 2 ? MTLTextureType3D : (arrayed ? MTLTextureTypeCubeArray : MTLTextureTypeCube);
    MTLPixelFormat pixelFormat = depth ? MTLPixelFormatDepth32Float :
        component == 1 ? MTLPixelFormatR32Uint : component == 2 ? MTLPixelFormatR32Sint : MTLPixelFormatR32Float;
    if (storage) {
        const RDNA4Fmt *f = rdna4_fmt(pixelFormat);
        if (!f || !(rdna4_fmt_feats(f) & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) {
            if (why) *why = "the selected format has no storage-image feature";
            return NO;
        }
    }
    p->textureType = type;
    p->pixelFormat = pixelFormat;
    p->usage = storage ? MTLTextureUsageShaderWrite : MTLTextureUsageShaderRead;
    p->storageMode = (depth || storage || samples > 1) ? MTLStorageModePrivate : MTLStorageModeShared;
    p->sampleCount = samples;
    p->depth = depth;
    p->storage = storage;
    return YES;
}

static void rdna4_dummy_zero_texture(RDNA4Texture *t) {
    if (!t || [t storageMode] != MTLStorageModeShared) return;
    uint32_t z = 0;
    MTLTextureType type = [t textureType];
    if (type == MTLTextureType1D)
        [t replaceRegion:MTLRegionMake1D(0, 1) mipmapLevel:0 withBytes:&z bytesPerRow:4];
    else if (type == MTLTextureType3D)
        [t replaceRegion:MTLRegionMake3D(0, 0, 0, 1, 1, 1) mipmapLevel:0 slice:0 withBytes:&z bytesPerRow:4 bytesPerImage:4];
    else if (type == MTLTextureType1DArray || type == MTLTextureType2DArray ||
             type == MTLTextureTypeCube || type == MTLTextureTypeCubeArray) {
        MTLRegion region = type == MTLTextureType1DArray ? MTLRegionMake1D(0, 1) : MTLRegionMake2D(0, 0, 1, 1);
        for (uint32_t slice = 0; slice < [t rdna4Layers]; ++slice)
            [t replaceRegion:region mipmapLevel:0 slice:slice withBytes:&z bytesPerRow:4 bytesPerImage:4];
    }
    else
        [t replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&z bytesPerRow:4];
}

typedef struct {
    RDNA4Texture *texture;
    uintptr_t owner;
} RDNA4DummyImageSlot;

#define RDNA4_DUMMY_SAMPLE_VARIANTS 4u
static RDNA4DummyImageSlot rdna4_dummy_null_read[RDNA4_ARGUMENT_NULL_IMAGES][RDNA4_DUMMY_SAMPLE_VARIANTS];
static RDNA4DummyImageSlot rdna4_dummy_null_write[RDNA4_ARGUMENT_NULL_IMAGES];

// Shape-specific read and write caches are separate. A null store therefore
// cannot overwrite a shared read dummy. Each cache is a fixed ABI-sized array.
// The shaped descriptor must use [texture vkView]; rdna4AttViewLevel:0 is a
// single-layer view for array storage and does not preserve the array shape.
static RDNA4Texture *rdna4_dummy_null_image_legacy(id dev, uint32_t shape, BOOL writable, uint32_t samples) {
    if (!dev || shape >= RDNA4_ARGUMENT_NULL_IMAGES) return nil;
    if (!samples) samples = 1;
    if (writable && samples != 1) return nil;
    unsigned sampleSlot = samples == 1 ? 0 : samples == 2 ? 1 : samples == 4 ? 2 : samples == 8 ? 3 : UINT32_MAX;
    if (sampleSlot == UINT32_MAX) return nil;
    RDNA4DummyImagePlan plan;
    const char *why = NULL;
    if (!rdna4_dummy_image_plan_make(shape, writable, samples, &plan, &why)) {
        RDNA4LOG("dummy %s texture shape %u samples %u rejected: %s", writable ? "storage" : "sampled",
                 shape, samples, why ?: "invalid descriptor");
        return nil;
    }
    RDNA4DummyImageSlot *slot = writable ? &rdna4_dummy_null_write[shape] : &rdna4_dummy_null_read[shape][sampleSlot];
    uintptr_t owner = (uintptr_t)(__bridge void *)dev;
    @synchronized ([GarnetDevice class]) {
        if (slot->owner != owner) { slot->owner = owner; slot->texture = nil; }
        if (slot->texture) return slot->texture;
        MTLTextureDescriptor *td = [MTLTextureDescriptor new];
        td.textureType = plan.textureType;
        td.pixelFormat = plan.pixelFormat;
        td.width = 1; td.height = 1; td.depth = 1; td.arrayLength = 1;
        td.mipmapLevelCount = 1; td.sampleCount = plan.sampleCount;
        td.usage = plan.usage; td.storageMode = plan.storageMode;
        NSError *e = nil;
        slot->texture = [[RDNA4Texture alloc] initWithDevice:dev descriptor:td error:&e];
        if (!slot->texture) {
            RDNA4LOG("dummy %s texture shape %u samples %u failed: %s", writable ? "storage" : "sampled",
                     shape, samples, e.localizedDescription.UTF8String);
        } else if (!plan.storage && !plan.depth && plan.sampleCount == 1) {
            rdna4_dummy_zero_texture(slot->texture);
        }
    }
    return slot->texture;
}

// Legacy entry point: all 48 ABI-5 values retain their previous acceptance or
// rejection, and every legal single-sample shape follows the old read-only path.
GARNET_INTERNAL RDNA4Texture *rdna4_dummy_null_texture(id dev, uint32_t shape) {
    return rdna4_dummy_null_image_legacy(dev, shape, NO, 1);
}

GARNET_INTERNAL RDNA4Texture *rdna4_dummy_null_image(id dev, const rdna4ns_shape *shape) {
    uint32_t legacy = 0;
    if (rdna4ns_shape_image_legacy(shape, &legacy) != RDNA4NS_OK) return nil;
    if (!shape->multisampled)
        return rdna4_dummy_null_image_legacy(dev, legacy,
            shape->access == RDNA4NS_ACCESS_STORAGE, 1);
    VkSampleCountFlags counts = shape->depth
        ? RDNA4R.lim.sampledImageDepthSampleCounts
        : shape->component == RDNA4NS_COMP_FLOAT
            ? RDNA4R.lim.sampledImageColorSampleCounts
            : RDNA4R.lim.sampledImageIntegerSampleCounts;
    for (uint32_t samples = 2; samples <= 8; samples *= 2) {
        if (!(counts & samples)) continue;
        RDNA4Texture *texture = rdna4_dummy_null_image_legacy(dev, legacy, NO, samples);
        if (texture) return texture;
    }
    return nil;
}

// Texel dummies use real VkBufferView descriptors, never image views.
typedef struct {
    VkBufferCreateInfo buffer;
    VkBufferViewCreateInfo view;
    VkDeviceSize offset;
    VkDeviceSize range;
} RDNA4DummyTexelPlan;

static inline uint32_t rdna4_dummy_texel_bytes(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT: case VK_FORMAT_R32_SFLOAT: return 4;
    default: return 0;
    }
}

static inline BOOL rdna4_dummy_texel_plan(BOOL storage, VkFormat format,
                                          VkFormatFeatureFlags bufferFeatures, BOOL featureEnabled,
                                          VkDeviceSize offset, VkDeviceSize range,
                                          VkDeviceSize offsetAlignment, uint64_t maxElements,
                                          RDNA4DummyTexelPlan *plan, const char **why) {
    if (why) *why = NULL;
    uint32_t texel = rdna4_dummy_texel_bytes(format);
    if (!texel || !plan) {
        if (why) *why = "texel dummies support R32 UINT/SINT/SFLOAT";
        return NO;
    }
    if (!featureEnabled) {
        if (why) *why = storage ? "storageTexelBuffer is not enabled" : "uniformTexelBuffer is not enabled";
        return NO;
    }
    VkFormatFeatureFlagBits required = storage ? VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT
                                               : VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT;
    if (!(bufferFeatures & required)) {
        if (why) *why = storage ? "format lacks VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT"
                                : "format lacks VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT";
        return NO;
    }
    if (!offsetAlignment || (offsetAlignment & (offsetAlignment - 1))) {
        if (why) *why = "minTexelBufferOffsetAlignment is not a power of two";
        return NO;
    }
    if (offset % offsetAlignment) {
        if (why) *why = "buffer view offset violates minTexelBufferOffsetAlignment";
        return NO;
    }
    if (!maxElements) {
        if (why) *why = "maxTexelBufferElements is zero";
        return NO;
    }
    if (range == VK_WHOLE_SIZE) range = texel;
    if (!range || (range % texel)) {
        if (why) *why = "texel buffer view range must be a nonzero whole number of texels";
        return NO;
    }
    if (range / texel > maxElements) {
        if (why) *why = "texel buffer view exceeds maxTexelBufferElements";
        return NO;
    }
    if (offset > UINT64_MAX - range) {
        if (why) *why = "texel buffer size overflows VkDeviceSize";
        return NO;
    }
    VkBufferUsageFlags usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
        (storage ? VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT : VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT);
    *plan = (RDNA4DummyTexelPlan){
        .buffer = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = offset + range, .usage = usage },
        .view = { .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
                  .buffer = VK_NULL_HANDLE, .format = format, .offset = offset, .range = range },
        .offset = offset, .range = range
    };
    return YES;
}

typedef struct {
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkBufferView view;
} RDNA4DummyTexelSlot;

// One device owns RADV for the process; bounded caches have process lifetime,
// like the image dummies. Failed construction never publishes a partial slot.
static RDNA4DummyTexelSlot rdna4_dummy_texels[2][RDNA4NS_TEXEL_SHAPES];

GARNET_INTERNAL VkBufferView rdna4_dummy_null_texel(id dev, const rdna4ns_shape *shape,
                                          const char **why) {
    if (why) *why = NULL;
    if (!dev || !shape || !rdna4ns_shape_valid(shape) ||
        !rdna4ns_access_is_texel(shape->access)) {
        if (why) *why = "invalid null texel shape";
        return VK_NULL_HANDLE;
    }
    BOOL storage = shape->access == RDNA4NS_ACCESS_STORAGE_TEXEL;
    const VkFormat formats[] = {VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32_UINT, VK_FORMAT_R32_SINT};
    RDNA4DummyTexelSlot *slot = &rdna4_dummy_texels[storage][shape->component];
    @synchronized ([GarnetDevice class]) {
        if (slot->view) return slot->view;
        VkFormatProperties properties = {0};
        vkGetPhysicalDeviceFormatProperties(RDNA4R.pd, formats[shape->component], &properties);
        RDNA4DummyTexelPlan plan;
        if (!rdna4_dummy_texel_plan(storage, formats[shape->component], properties.bufferFeatures,
                storage ? RDNA4R.storageTexelBuffers : RDNA4R.uniformTexelBuffers,
                0, 4, RDNA4R.lim.minTexelBufferOffsetAlignment,
                RDNA4R.lim.maxTexelBufferElements, &plan, why)) return VK_NULL_HANDLE;
        RDNA4DummyTexelSlot built = {0};
        VkResult result = vkCreateBuffer(RDNA4R.dev, &plan.buffer, NULL, &built.buffer);
        if (result == VK_SUCCESS) {
            VkMemoryRequirements requirements = {0};
            vkGetBufferMemoryRequirements(RDNA4R.dev, built.buffer, &requirements);
            uint32_t memoryType = UINT32_MAX;
            VkMemoryPropertyFlags flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            for (uint32_t i = 0; i < RDNA4R.mp.memoryTypeCount; ++i)
                if ((requirements.memoryTypeBits & (1u << i)) &&
                    (RDNA4R.mp.memoryTypes[i].propertyFlags & flags) == flags) {
                    memoryType = i;
                    break;
                }
            if (memoryType == UINT32_MAX) result = VK_ERROR_FEATURE_NOT_PRESENT;
            else {
                VkMemoryAllocateInfo info = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                    .allocationSize = requirements.size, .memoryTypeIndex = memoryType};
                result = vkAllocateMemory(RDNA4R.dev, &info, NULL, &built.memory);
            }
        }
        if (result == VK_SUCCESS)
            result = vkBindBufferMemory(RDNA4R.dev, built.buffer, built.memory, 0);
        if (result == VK_SUCCESS) {
            void *bytes = NULL;
            result = vkMapMemory(RDNA4R.dev, built.memory, 0, VK_WHOLE_SIZE, 0, &bytes);
            if (result == VK_SUCCESS) {
                memset(bytes, 0, 4);
                vkUnmapMemory(RDNA4R.dev, built.memory);
            }
        }
        if (result == VK_SUCCESS) {
            plan.view.buffer = built.buffer;
            result = vkCreateBufferView(RDNA4R.dev, &plan.view, NULL, &built.view);
        }
        if (result != VK_SUCCESS) {
            if (built.buffer) vkDestroyBuffer(RDNA4R.dev, built.buffer, NULL);
            if (built.memory) vkFreeMemory(RDNA4R.dev, built.memory, NULL);
            if (why) *why = "null texel buffer allocation, zeroing or view creation failed";
            return VK_NULL_HANDLE;
        }
        *slot = built;
        return slot->view;
    }
}

GARNET_INTERNAL RDNA4SamplerState *rdna4_dummy_sampler(id dev) {
    static RDNA4SamplerState *s;
    @synchronized ([GarnetDevice class]) {
        if (!s) { NSError *e = nil; s = [[RDNA4SamplerState alloc] initWithDevice:dev descriptor:[MTLSamplerDescriptor new] error:&e];
                  if (!s) RDNA4LOG("dummy sampler failed: %s", e.localizedDescription.UTF8String); }
    }
    return s;
}

// Visits every image a stage's bindings will read or write, with the layout the descriptor needs (SHADER_READ_ONLY for sampled images,
// GENERAL for storage images), so the encoder can transition them BEFORE the draw/dispatch (never inside a render pass).
GARNET_INTERNAL void rdna4_each_tex(const RDNA4PB *pb, uint32_t n, const RDNA4StageState *st, id dev, void (^fn)(RDNA4Texture *t, VkImageLayout l, BOOL writable)) {
    for (uint32_t i = 0; i < n; i++) {
        if ((pb[i].type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || pb[i].type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER) && pb[i].binding >= 32 && pb[i].binding < 160) {
            for (uint32_t k = 0; k < pb[i].count; k++) { uint32_t ix = pb[i].binding - 32 + k;
                RDNA4Texture *t = pb[i].embedded ? (RDNA4Texture *)rdna4_embedded_resource(&pb[i], k, st, YES, dev) : (ix < 128 ? st->tex[ix] : nil);
                if (!t && !pb[i].embedded) t = rdna4_dummy_texture(dev); if (t) fn(t, pb[i].gpuArguments ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, NO); }
        } else if ((pb[i].type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || pb[i].type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) && pb[i].binding >= 480 && pb[i].binding < 608) {
            for (uint32_t k = 0; k < pb[i].count; k++) {
                uint32_t ix = pb[i].binding - 480 + k;
                RDNA4Texture *t = pb[i].embedded ? (RDNA4Texture *)rdna4_embedded_resource(&pb[i], k, st, YES, dev) : (ix < 128 ? st->tex[ix] : nil);
                if (t) fn(t, VK_IMAGE_LAYOUT_GENERAL, YES);
            }
        }
    }
}

// KernelDispatchPlan::new (metal2vulkan src/reflect/mod.rs): a dispatchThreads grid becomes <= 8 rectangular regions (full groups + boundary tails).
GARNET_INTERNAL int rdna4_plan(const uint32_t threads[3], const uint32_t nom[3], RDNA4Region *out) {
    int n = 0;
    for (int mask = 0; mask < 8; mask++) {
        RDNA4Region r = { { nom[0], nom[1], nom[2] }, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 } }; BOOL nonempty = YES;
        for (int d = 0; d < 3; d++) {
            uint32_t full = threads[d] / nom[d], tail = threads[d] % nom[d];
            if (!(mask & (1 << d))) r.groups[d] = full;
            else if (tail == 0) nonempty = NO;
            else { r.local[d] = tail; r.groups[d] = 1; r.tbase[d] = full * nom[d]; r.gbase[d] = full; }
            nonempty = nonempty && r.groups[d] != 0;
        }
        if (nonempty) out[n++] = r;
    }
    return n;
}

#ifndef RDNA4_ENCODER_BINDINGS_INC
#define RDNA4_ENCODER_BINDINGS_INC


GARNET_INTERNAL uint64_t rdna4_resource_slot_key(uint32_t kind, BOOL fragment,
                                     NSUInteger index) {
    return ((uint64_t)kind << 56) |
        ((uint64_t)(fragment ? 1u : 0u) << 48) | (uint64_t)index;
}

GARNET_INTERNAL BOOL rdna4_encoder_take_segment_change(uint64_t *tracked,
                                              uint64_t current) {
    if (!tracked || *tracked == current) return NO;
    *tracked = current;
    return YES;
}

/*
 * Current encoder state is bounded by the descriptor bands. The flat unboxed
 * table owns only objects still bound to a slot; it is not a command-buffer
 * history. Every draw/dispatch incrementally snapshots dirty entries into the
 * current segment, and snapshots all entries when the command buffer or
 * segment serial changes.
 */

@implementation RDNA4ResourceBindings
- (instancetype)init {
    self = [super init];
    if (!self) return nil;
    return self;
}
- (void)dealloc {
    for (uint32_t i = 0; i < _count; i++) {
        if (_entries[i].obj) CFRelease(_entries[i].obj);
    }
    free(_entries);
}
- (void)setObject:(id)object key:(uint64_t)key {
    for (uint32_t i = 0; i < _count; i++) {
        if (_entries[i].key != key) continue;
        if (!object) {
            CFRelease(_entries[i].obj);
            _count--;
            if (i < _count) _entries[i] = _entries[_count];
            return;
        }
        if (_entries[i].obj == (__bridge CFTypeRef)object) return;
        CFRelease(_entries[i].obj);
        _entries[i].obj = (__bridge_retained CFTypeRef)object;
        _entries[i].snapshotted = 0u;
        _hasUnsnapshotted = YES;
        return;
    }
    if (!object) return;
    if (_count == _cap) {
        uint32_t nextCap = _cap ? _cap * 2u : 16u;
        RDNA4ResBindEntry *grown = realloc(_entries, (size_t)nextCap * sizeof(*grown));
        if (!grown) return;
        _entries = grown;
        _cap = nextCap;
    }
    _entries[_count++] = (RDNA4ResBindEntry){
        .key = key,
        .obj = (__bridge_retained CFTypeRef)object,
        .snapshotted = 0u,
    };
    _hasUnsnapshotted = YES;
}
- (void)setObject:(id)object slot:(NSNumber *)slot {
    if (!slot) return;
    [self setObject:object key:slot.unsignedLongLongValue];
}
- (void)clear {
    for (uint32_t i = 0; i < _count; i++) {
        if (_entries[i].obj) CFRelease(_entries[i].obj);
    }
    _count = 0u;
    _lastCBId = 0u;
    _lastSegmentSerial = 0u;
    _hasUnsnapshotted = NO;
}
- (NSUInteger)count {
    return (NSUInteger)_count;
}
- (void)snapshotToCommandBuffer:(RDNA4CommandBuffer *)commandBuffer {
    if (!commandBuffer || !_count) return;
    const uint64_t cbid = [commandBuffer rdna4CBId];
    const uint64_t segSer = [commandBuffer rdna4SegmentSerial];
    const BOOL canCache = (cbid != 0u && segSer != 0u);
    if (canCache && cbid == _lastCBId && segSer == _lastSegmentSerial) {
        if (!_hasUnsnapshotted) return;
        for (uint32_t i = 0; i < _count; i++) {
            if (!_entries[i].snapshotted) {
                [commandBuffer rdna4RetainEncoded:(__bridge id)_entries[i].obj];
                _entries[i].snapshotted = 1u;
            }
        }
        _hasUnsnapshotted = NO;
        return;
    }
    for (uint32_t i = 0; i < _count; i++) {
        [commandBuffer rdna4RetainEncoded:(__bridge id)_entries[i].obj];
        _entries[i].snapshotted = 1u;
    }
    if (canCache) {
        _lastCBId = cbid;
        _lastSegmentSerial = segSer;
        _hasUnsnapshotted = NO;
    } else {
        _lastCBId = 0u;
        _lastSegmentSerial = 0u;
        _hasUnsnapshotted = YES;
    }
}
@end

#endif
