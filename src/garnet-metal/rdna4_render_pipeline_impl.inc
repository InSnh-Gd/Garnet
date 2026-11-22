// ---------------------------------------------------------------------------------------------------------------
// RDNA4RenderPipelineState (10d/11e): SPIR-V from Resources/spvcache/<sha256(bitcodeData)>.spv -> VkPipeline variants.
// ---------------------------------------------------------------------------------------------------------------
#include "rdna4_fragment_output.h"

@interface RDNA4RenderPipelineState : _MTLRenderPipelineState {
    id _dev; NSString *_lbl; VkShaderModule _vs, _fs; VkPipelineLayout _pl; VkRenderPass _rp; VkDescriptorSetLayout _dsl[2];
    RDNA4PB *_pbV, *_pbF; uint32_t _npbV, _npbF; BOOL _fetch; uint32_t _fetchMax;   // 11e-2: fragment reads [[color(n)]] (input attachments 0.._fetchMax)
    VkVertexInputBindingDescription _vib[32]; VkVertexInputAttributeDescription _via[32]; uint32_t _nvib, _nvia; uint32_t _need[32]; uint32_t _nneed;
    VkPipelineColorBlendAttachmentState _cba[8]; uint32_t _na; char _vep[128], _fep[128];
    NSMutableDictionary *_variants; NSLock *_lock; NSMutableArray *_owned; BOOL _fallback, _icb, _hasFrag, _rastOff;
    char _vnm[64], _fnm[64];   // native #12 P1: vertex / fragment function names (CoreDisplay's final pass = fragment "GPUPass")
    rdna4pc_t _pc; MTLPixelFormat _dpf, _spf; VkFormat _dsVk; unsigned _dsAsp;   // bundle 10: fixed config (samples, dynamic states, depth bias), the depth / stencil attachment formats, the attachment's VkFormat (UNDEFINED = none) and aspects
    MTLRenderPipelineDescriptor *_hsDesc; _Atomic(uintptr_t) _realp;   // hot-swap: creation descriptor (fallback only); the published real object (+1, written once)
    MTLRenderPipelineReflection *_reflection;
    RDNA4ArgumentTables _argumentTables;
    rdna4_argument_scan_reflection_plan _scanV, _scanF;
}
- (instancetype)initWithDevice:(id)dev descriptor:(MTLRenderPipelineDescriptor *)d error:(NSError **)err;
- (instancetype)initWithDevice:(id)dev descriptor:(MTLRenderPipelineDescriptor *)d noFallback:(BOOL)nofb error:(NSError **)err;
- (RDNA4RenderPipelineState *)rdna4Real;   // the hot-swapped real pipeline, or nil
- (BOOL)rdna4HotRebuild;
- (BOOL)rdna4IsRenderFallback;
- (VkPipeline)vkPipeline;
- (VkPipeline)pipelineForTopology:(VkPrimitiveTopology)t cull:(VkCullModeFlags)c front:(VkFrontFace)f;
- (VkPipeline)pipelineForTopology:(VkPrimitiveTopology)t cull:(VkCullModeFlags)c front:(VkFrontFace)f ds:(uint32_t)dk;   // bundle 10: dk = rdna4DSKey: (0 for a colour-only pipeline)
- (uint32_t)rdna4DSKey:(const void *)dsinfo;   // RDNA4DSInfo *, NULL = the default state (Always, no write, no stencil)
- (BOOL)rdna4HasDS; - (BOOL)rdna4HasDepth; - (BOOL)rdna4HasStencil; - (VkFormat)rdna4DSVk; - (unsigned)rdna4SampleCount;
- (VkPipelineLayout)layout;
- (VkDescriptorSetLayout)dsl:(int)i;
- (const RDNA4ArgumentTables *)argumentTables;
- (const rdna4_argument_scan_reflection_plan *)scanPlanV;
- (const rdna4_argument_scan_reflection_plan *)scanPlanF;
- (const RDNA4PB *)pbV; - (uint32_t)npbV; - (const RDNA4PB *)pbF; - (uint32_t)npbF;
- (const uint32_t *)needBindings; - (uint32_t)nNeed;
- (BOOL)fetch; - (uint32_t)fetchMax;
- (const char *)rdna4VName; - (const char *)rdna4FName; - (BOOL)rdna4HasFragment;
- (id)reflection;
- (void)rdna4SetReflection:(MTLRenderPipelineReflection *)reflection;
@end
static VkRenderPass rdna4_mk_rp(const VkAttachmentDescription *ads, uint32_t na, uint32_t nd, BOOL fetch);   // bundle 10: nd = 1 when ads[na] is the depth/stencil attachment

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
    RDNA4LinkCtx *lk = rdna4_lk_for(fn);   // build 18 (P3): a linked stage looks under the linked key first, then under the entry's own sha (an entry that needs none of its dependencies translates alone)
    for (NSString *cs in (lk ? @[ lk->lsha, sha ] : @[ sha ])) {
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

static NSData *rdna4_spv_lookup(id fn, const char *role, NSError **err, NSString **fname, NSDictionary **meta) {
    uint64_t t_ = rdna4_now(); NSData *d = rdna4_spv_lookup_impl(fn, role, err, fname, meta); rdna4t_add(&T1.spv, rdna4_now() - t_); return d;
}

static VkShaderModule rdna4_spv_module(const void *code, size_t bytes, const char *role, NSError **err) {
    VkShaderModuleCreateInfo smc = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes, .pCode = code };
    VkShaderModule m = VK_NULL_HANDLE; uint64_t t_ = rdna4_now(); VkResult r = vkCreateShaderModule(RDNA4R.dev, &smc, NULL, &m); rdna4t_add(&T1.shm, rdna4_now() - t_);
    if (r != VK_SUCCESS) { if (err) *err = rdna4_err(42, [NSString stringWithFormat:@"vkCreateShaderModule(%s) = %d", role, r]); return VK_NULL_HANDLE; }
    return m;
}

@implementation RDNA4RenderPipelineState
- (instancetype)initWithDevice:(id)dev descriptor:(MTLRenderPipelineDescriptor *)d error:(NSError **)err { return [self initWithDevice:dev descriptor:d noFallback:NO error:err]; }
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
    uintptr_t v = (uintptr_t)CFBridgingRetain(r);
    if (!rdna4hs_install(&_realp, v)) { CFRelease((CFTypeRef)v); return NO; }   // lost a race (cannot happen: one watcher): keep the first
    return YES;
}
- (instancetype)initWithDevice:(id)dev descriptor:(MTLRenderPipelineDescriptor *)d noFallback:(BOOL)nofb error:(NSError **)err {
    self = [super init];
    if (!self) return nil;
    _dev = dev; _lock = [NSLock new]; _variants = [NSMutableDictionary dictionary]; _owned = [NSMutableArray array];
    _icb = d.supportIndirectCommandBuffers;
    const BOOL hasFrag = d.fragmentFunction != nil;
    _hasFrag = hasFrag;
    _rastOff = !d.isRasterizationEnabled;
    if (!rdna4_radv_open(err)) return nil;
    const uint64_t deadline = rdna4_now() + (uint64_t)RDNA4G_SYNC_WAIT_APP_MS * 1000000ull;
    // 11e: SPIR-V from the cache; on a miss in WindowServer (or RDNA4M_FORCE_FALLBACK with RDNA4M_ALLOW) build
    // from the built-in no-write shaders instead of failing (the AIR is dumped by the caller-side rdna4_dump_pipeline).
    NSString *vname = nil, *fname = hasFrag ? nil : @"(none)"; NSError *e1 = nil, *e2 = nil; NSDictionary *mv_ = nil, *mf_ = nil;
    NSData *vsd = rdna4_spv_lookup(d.vertexFunction, "vertex", &e1, &vname, &mv_);
    NSData *fsd = hasFrag ? rdna4_spv_lookup(d.fragmentFunction, "fragment", &e2, &fname, &mf_) : nil;
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
                if (!rdna4_fallback_ok()) {
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
        char why[256] = "";
        fsp = hasFrag ? rdna4_spv_set(fsd, 1) : nil;   // fragment descriptors live in set 1 (binding-model comment above)
        rv = calloc(1, sizeof *rv); rf = hasFrag ? calloc(1, sizeof *rf) : NULL;
        BOOL bad = NO;
        if (rdna4s_parse(vsd.bytes, vsd.length / 4, &mv, why, sizeof why) || !mv.has_ep || mv.ep_model != 0 || rdna4s_reflect(&mv, 0, rv)) {
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
        if (!bad) { strlcpy(vep, mv.ep_name, sizeof vep); if (hasFrag) strlcpy(fep, mf.ep_name, sizeof fep); }
        rdna4s_free(&mv); if (hasFrag) rdna4s_free(&mf);
        if (bad) { free(rv); free(rf); return nil; }
    }
    strlcpy(_vep, vep, sizeof _vep); strlcpy(_fep, fep, sizeof _fep);
    BOOL ok = NO;
    do {
        _vs = fb ? rdna4_spv_module(rdna4_fb_vs_spv, sizeof rdna4_fb_vs_spv, "vertex", err) : rdna4_spv_module(vsd.bytes, vsd.length, "vertex", err); if (!_vs) break;
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
    for (NSUInteger i = 0; i < 8; i++) {
        MTLRenderPipelineColorAttachmentDescriptor *ca = d.colorAttachments[i];
        if (ca.pixelFormat == MTLPixelFormatInvalid) continue;
        const RDNA4Fmt *f = rdna4_fmt(ca.pixelFormat);
        if (!f) { RDNA4LOG("pipeline: colour attachment %lu pixel format %lu has no Vulkan mapping in the format table", (unsigned long)i, (unsigned long)ca.pixelFormat);
                  if (err) *err = rdna4_err(43, [NSString stringWithFormat:@"unsupported colour attachment format %lu", (unsigned long)ca.pixelFormat]); free(rv); free(rf); return nil; }
        if (f->flags & RDNA4F_A8) { RDNA4LOG("pipeline: colour attachment %lu is A8Unorm: not supported (the fragment's alpha cannot be written into an R8 attachment)", (unsigned long)i);
                  if (err) *err = rdna4_err(43, @"unsupported colour attachment format 1 (A8Unorm render targets are not supported)"); free(rv); free(rf); return nil; }
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
            _cba[na].blendEnable = VK_TRUE;
            _cba[na].srcColorBlendFactor = rdna4_bf(ca.sourceRGBBlendFactor, &bad); _cba[na].dstColorBlendFactor = rdna4_bf(ca.destinationRGBBlendFactor, &bad);
            _cba[na].srcAlphaBlendFactor = rdna4_bf(ca.sourceAlphaBlendFactor, &bad); _cba[na].dstAlphaBlendFactor = rdna4_bf(ca.destinationAlphaBlendFactor, &bad);
            _cba[na].colorBlendOp = rdna4_bo(ca.rgbBlendOperation, &bad); _cba[na].alphaBlendOp = rdna4_bo(ca.alphaBlendOperation, &bad);
            if (bad) { if (err) *err = rdna4_err(56, @"blend factor/operation not supported (dual-source Source1* factors)"); free(rv); free(rf); return nil; }
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
    // Layouts: vertex = set 0, fragment = set 1 (both always exist, possibly empty) + push constants when a stage declares a block.
    NSError *le = nil;
    if (!rdna4_stage_dsl(rv, 0, VK_SHADER_STAGE_VERTEX_BIT, mv_, &_dsl[0], &_pbV, &_npbV, _owned, &le) ||
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
    NSError *scanError = nil;
    if (!rdna4_argument_scan_plan_build(&_scanV, preV, mv_, "vertex", &scanError) ||
        !rdna4_argument_scan_plan_build(&_scanF, preF, mf_, "fragment", &scanError)) {
        if (err) *err = scanError; free(rv); free(rf); return nil;
    }
    uint32_t pcs = 0; VkShaderStageFlags pcf = 0;
    if (rv && rv->pc_size) { pcs = rv->pc_size; pcf |= VK_SHADER_STAGE_VERTEX_BIT; }
    if (rf && rf->pc_size) { if (rf->pc_size > pcs) pcs = rf->pc_size; pcf |= VK_SHADER_STAGE_FRAGMENT_BIT; }
    if (pcs > 256) { if (err) *err = rdna4_err(48, [NSString stringWithFormat:@"push constant block %u B > 256", pcs]); free(rv); free(rf); return nil; }
    VkPushConstantRange pcr = { pcf, 0, pcs };
    VkDescriptorSetLayout layouts[5] = {_dsl[0], _dsl[1], _argumentTables.dsl[0], _argumentTables.dsl[1], _argumentTables.dsl[2]};
    VkPipelineLayoutCreateInfo plc = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = _argumentTables.active ? 5 : 2, .pSetLayouts = layouts,
        .pushConstantRangeCount = pcs ? 1 : 0, .pPushConstantRanges = &pcr };
    r = vkCreatePipelineLayout(RDNA4R.dev, &plc, NULL, &_pl);
    if (r != VK_SUCCESS) { if (err) *err = rdna4_err(46, [NSString stringWithFormat:@"vkCreatePipelineLayout = %d", r]); free(rv); free(rf); return nil; }
    // Vertex input: for every SPIR-V input Location, the descriptor's attribute of that index (format, offset, bufferIndex) and its
    // buffer layout (stride, step function). A Location without a descriptor attribute reads binding 31 (stride 0, a null buffer).
    MTLVertexDescriptor *vd = d.vertexDescriptor; BOOL dummy = NO;
    for (int i = 0; rv && i < rv->nva; i++) {
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
        } else {
            RDNA4LOG("pipeline: WARN shader input location %u has no vertex-descriptor attribute: reads a zero-stride null binding", loc);
            if (!dummy) { _vib[_nvib++] = (VkVertexInputBindingDescription){ 31, 0, VK_VERTEX_INPUT_RATE_VERTEX }; _need[_nneed++] = 31; dummy = YES; }
            _via[_nvia++] = (VkVertexInputAttributeDescription){ loc, 31, rv->va[i].format, 0 };
        }
    }
    free(rv); free(rf);
    uint32_t dk0 = [self rdna4DSKey:NULL];   // bundle 10: 0 for a colour-only pipeline (the key below is then exactly the old one)
    NSError *ve = nil; VkPipeline p0 = [self rdna4Build:VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST cull:VK_CULL_MODE_NONE front:VK_FRONT_FACE_CLOCKWISE ds:dk0 error:&ve];
    if (!p0) { if (err) *err = ve; return nil; }
    _variants[@((uint64_t)VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST | ((uint64_t)VK_CULL_MODE_NONE << 8) | ((uint64_t)VK_FRONT_FACE_CLOCKWISE << 16) | ((uint64_t)dk0 << 24))] = [NSValue valueWithPointer:(void *)p0];
    RDNA4LOG("RDNA4RenderPipelineState %p: pipeline %p (%u colour attachment(s), set0 %u / set1 %u bindings, %u vertex input(s), %u vertex binding(s)%s%s)", (__bridge void *)self, (void *)p0, na,
           _npbV, _npbF, _nvia, _nvib, fb ? ", FALLBACK no-write" : "", _fetch ? ", FRAMEBUFFER FETCH (input attachments)" : "");
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
- (VkPipeline)rdna4Build:(VkPrimitiveTopology)topo cull:(VkCullModeFlags)cull front:(VkFrontFace)front ds:(uint32_t)dk error:(NSError **)err {
    VkPipelineShaderStageCreateInfo st[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = _vs, .pName = _vep },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = _fs, .pName = _fep } };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = _nvib, .pVertexBindingDescriptions = _vib, .vertexAttributeDescriptionCount = _nvia, .pVertexAttributeDescriptions = _via };
    BOOL strip = topo == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP || topo == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;   // Metal restarts indexed strips at the max index value
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = topo, .primitiveRestartEnable = strip };
    VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL,
        .rasterizerDiscardEnable = _rastOff ? VK_TRUE : VK_FALSE,
        .cullMode = cull, .frontFace = front, .lineWidth = 1.0f, .depthBiasEnable = _pc.depthBias ? VK_TRUE : VK_FALSE };   // bundle 10: the bias VALUES are dynamic (setDepthBias:), on for depth pipelines only
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = (VkSampleCountFlagBits)_pc.samples, .alphaToCoverageEnable = _pc.alphaToCoverage ? VK_TRUE : VK_FALSE };
    rdna4ds_vk_t dv; rdna4ds_decode(dk, &dv);
    VkPipelineDepthStencilStateCreateInfo dss = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = dv.depthTest ? VK_TRUE : VK_FALSE, .depthWriteEnable = dv.depthWrite ? VK_TRUE : VK_FALSE, .depthCompareOp = (VkCompareOp)dv.depthCompareOp, .stencilTestEnable = dv.stencilTest ? VK_TRUE : VK_FALSE,
        .front = { (VkStencilOp)dv.front.failOp, (VkStencilOp)dv.front.passOp, (VkStencilOp)dv.front.depthFailOp, (VkCompareOp)dv.front.compareOp, 0, 0, 0 },
        .back = { (VkStencilOp)dv.back.failOp, (VkStencilOp)dv.back.passOp, (VkStencilOp)dv.back.depthFailOp, (VkCompareOp)dv.back.compareOp, 0, 0, 0 } };
    VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .flags = _fetch && RDNA4R.orderedColorAccess ? VK_PIPELINE_COLOR_BLEND_STATE_CREATE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_BIT_EXT : 0,
        .attachmentCount = _na, .pAttachments = _cba };
    VkDynamicState dyn[8]; for (unsigned i = 0; i < _pc.ndyn && i < 8; i++) dyn[i] = (VkDynamicState)_pc.dyn[i];   // bundle 10: viewport, scissor, blend constants (+ stencil masks / reference and depth bias for a depth/stencil pipeline)
    VkPipelineDynamicStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = _pc.ndyn, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo gpc = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = _fs ? 2u : 1u, .pStages = st, .pVertexInputState = &vi,
        .pInputAssemblyState = &ia, .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = _pc.hasDS ? &dss : NULL, .pColorBlendState = &cb,
        .pDynamicState = &ds, .layout = _pl, .renderPass = _rp };
    VkPipeline p = VK_NULL_HANDLE; uint64_t t_ = rdna4_now(); VkResult r = vkCreateGraphicsPipelines(RDNA4R.dev, RDNA4R.pc, 1, &gpc, NULL, &p); rdna4t_add(&T1.gfx, rdna4_now() - t_); atomic_fetch_add(&rdna4_pipes_new, 1);
    if (r != VK_SUCCESS) { if (err) *err = rdna4_err(47, [NSString stringWithFormat:@"vkCreateGraphicsPipelines = %d", r]); return VK_NULL_HANDLE; }
    return p;
}
- (VkPipeline)pipelineForTopology:(VkPrimitiveTopology)topo cull:(VkCullModeFlags)cull front:(VkFrontFace)front { return [self pipelineForTopology:topo cull:cull front:front ds:[self rdna4DSKey:NULL]]; }
- (VkPipeline)pipelineForTopology:(VkPrimitiveTopology)topo cull:(VkCullModeFlags)cull front:(VkFrontFace)front ds:(uint32_t)dk {
    NSNumber *k = @((uint64_t)topo | ((uint64_t)cull << 8) | ((uint64_t)front << 16) | ((uint64_t)dk << 24));
    [_lock lock];
    NSValue *v = _variants[k];
    if (!v) {
        NSError *e = nil; VkPipeline p = [self rdna4Build:topo cull:cull front:front ds:dk error:&e];
        if (!p) RDNA4LOG("pipeline variant (topology %d cull %d front %d) failed: %s", topo, cull, front, e.localizedDescription.UTF8String);
        else { v = [NSValue valueWithPointer:(void *)p]; _variants[k] = v; RDNA4LOG("pipeline variant topology %d cull %d front %d created", topo, cull, front); }
    }
    [_lock unlock];
    return v ? (VkPipeline)v.pointerValue : VK_NULL_HANDLE;
}
- (void)dealloc {
    if (RDNA4R.ok) {
        for (NSValue *v in _variants.allValues) vkDestroyPipeline(RDNA4R.dev, (VkPipeline)v.pointerValue, NULL);
        if (_pl) vkDestroyPipelineLayout(RDNA4R.dev, _pl, NULL);
        for (int i = 0; i < 2; i++) if (_dsl[i]) vkDestroyDescriptorSetLayout(RDNA4R.dev, _dsl[i], NULL);
        rdna4_argument_tables_destroy(&_argumentTables);
        for (NSValue *v in _owned) rdna4_destroy_sampler((VkSampler)v.pointerValue, NULL);
        if (_rp) vkDestroyRenderPass(RDNA4R.dev, _rp, NULL);
        if (_vs) vkDestroyShaderModule(RDNA4R.dev, _vs, NULL);
        if (_fs) vkDestroyShaderModule(RDNA4R.dev, _fs, NULL);
    }
    rdna4_argument_scan_reflection_plan_destroy(&_scanV);
    rdna4_argument_scan_reflection_plan_destroy(&_scanF);
    free(_pbV); free(_pbF);
    uintptr_t rp = rdna4hs_load(&_realp); if (rp) CFRelease((CFTypeRef)rp);   // the published real object
}
RDNA4_DNR(RDNA4RenderPipelineState)
- (VkPipeline)vkPipeline { return [self pipelineForTopology:VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST cull:VK_CULL_MODE_NONE front:VK_FRONT_FACE_CLOCKWISE]; }
- (id)reflection { return _reflection; }
- (void)rdna4SetReflection:(MTLRenderPipelineReflection *)reflection { _reflection = reflection; }
- (VkPipelineLayout)layout { return _pl; }
- (VkDescriptorSetLayout)dsl:(int)i { return _dsl[i]; }
- (const RDNA4ArgumentTables *)argumentTables { return &_argumentTables; }
- (const rdna4_argument_scan_reflection_plan *)scanPlanV { return &_scanV; }
- (const rdna4_argument_scan_reflection_plan *)scanPlanF { return &_scanF; }
- (BOOL)fetch { return _fetch; } - (uint32_t)fetchMax { return _fetchMax; }
- (const RDNA4PB *)pbV { return _pbV; } - (uint32_t)npbV { return _npbV; } - (const RDNA4PB *)pbF { return _pbF; } - (uint32_t)npbF { return _npbF; }
- (const uint32_t *)needBindings { return _need; } - (uint32_t)nNeed { return _nneed; }
- (id)device { return _dev; }
- (NSString *)label { return _lbl; }
- (void)setLabel:(NSString *)l { _lbl = [l copy]; }
- (NSUInteger)maxTotalThreadsPerThreadgroup { return 0; }
- (NSUInteger)threadgroupSizeMatchesTileSize { return 0; }
- (NSUInteger)imageblockSampleLength { return 0; }
- (BOOL)supportIndirectCommandBuffers { return _icb; }   // CPU replay is wired below; patch and compute remain gated
@end

