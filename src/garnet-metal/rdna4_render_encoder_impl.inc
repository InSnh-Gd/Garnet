// ---------------------------------------------------------------------------------------------------------------
// RDNA4RenderEncoder / RDNA4ComputeEncoder / RDNA4BlitEncoder (10c, 10d, 11e)
// ---------------------------------------------------------------------------------------------------------------
#include "rdna4_counters.h"

#ifndef RDNA4_COUNTER_INTERNAL_PROTO
#define RDNA4_COUNTER_INTERNAL_PROTO
@protocol RDNA4CounterSampleBufferInternal <NSObject>
- (id)device;
- (NSUInteger)sampleCount;
- (MTLStorageMode)rdna4StorageMode;
- (BOOL)rdna4SampleOnCommandBuffer:(RDNA4CommandBuffer *)cb
                   vkCommandBuffer:(VkCommandBuffer)cmd
                       sampleIndex:(NSUInteger)sampleIndex
                           barrier:(BOOL)barrier
                        stageFlags:(VkPipelineStageFlagBits)stage
                               who:(const char *)who;
- (BOOL)rdna4ResolveRange:(NSRange)range
                intoBytes:(uint64_t *)dst
                    error:(NSError **)err;
@end
#endif

static id<RDNA4CounterSampleBufferInternal> rdna4_counter_buffer_check(
    id sampleBuffer, id expectedDevice, NSUInteger sampleIndex,
    BOOL checkIndex, NSString **whyOut) {
    const rdna4dc_metal3_caps m3 = rdna4_metal3_caps();
    if (!rdna4dc_metal3_timestamps_supported(&m3)) {
        if (whyOut) *whyOut = @"GPU timestamp counters are unsupported on this device";
        return nil;
    }
    Class cls = objc_getClass("RDNA4CounterSampleBuffer");
    if (!cls || !sampleBuffer || ![sampleBuffer isKindOfClass:cls]) {
        if (whyOut) *whyOut = @"invalid counter sample buffer";
        return nil;
    }
    id<RDNA4CounterSampleBufferInternal> csb = (id<RDNA4CounterSampleBufferInternal>)sampleBuffer;
    if ([csb device] != expectedDevice) {
        if (whyOut) *whyOut = @"counter sample buffer belongs to another device";
        return nil;
    }
    if (checkIndex && !rdna4cnt_index_ok((uint64_t)[csb sampleCount], (uint64_t)sampleIndex)) {
        if (whyOut) *whyOut = [NSString stringWithFormat:@"sampleIndex %lu out of range (sampleCount %lu)",
                               (unsigned long)sampleIndex, (unsigned long)[csb sampleCount]];
        return nil;
    }
    return csb;
}

static BOOL rdna4_counter_pass_index_valid(id<RDNA4CounterSampleBufferInternal> csb, NSUInteger index) {
    return index == MTLCounterDontSample ||
           rdna4cnt_index_ok((uint64_t)[csb sampleCount], (uint64_t)index);
}

static void rdna4_full_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}
static VkPrimitiveTopology rdna4_topo(MTLPrimitiveType t) {
    switch (t) { case MTLPrimitiveTypePoint: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST; case MTLPrimitiveTypeLine: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        case MTLPrimitiveTypeLineStrip: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; case MTLPrimitiveTypeTriangleStrip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; }
}
// Without ordered attachment access, the encoder emits one primitive per draw
// and uses the self-dependency to make each primitive's writes visible.
static VkRenderPass rdna4_mk_rp(const VkAttachmentDescription *ads, uint32_t na, uint32_t nd, BOOL fetch) {
    VkAttachmentReference refs[8], ins[8], dref = { na, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };   // bundle 10: nd = 1: ads[na] is the depth/stencil attachment
    for (uint32_t i = 0; i < na; i++) { refs[i] = (VkAttachmentReference){ i, fetch ? VK_IMAGE_LAYOUT_GENERAL : ads[i].finalLayout }; ins[i] = (VkAttachmentReference){ i, VK_IMAGE_LAYOUT_GENERAL }; }
    VkSubpassDescription sp = { .flags = fetch && RDNA4R.orderedColorAccess ? VK_SUBPASS_DESCRIPTION_RASTERIZATION_ORDER_ATTACHMENT_COLOR_ACCESS_BIT_EXT : 0,
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = na, .pColorAttachments = refs,
        .inputAttachmentCount = fetch ? na : 0, .pInputAttachments = fetch ? ins : NULL, .pDepthStencilAttachment = nd ? &dref : NULL };
    VkSubpassDependency deps[3] = {
        { .srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0, .srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, .dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
          .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT },
        { .srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL, .srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, .dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
          .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT },
        { .srcSubpass = 0, .dstSubpass = 0, .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
          .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
          .dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT } };
    // deps[0] = external in, deps[1] = external out, deps[2] = the fetch self-dependency (only declared for a fetch pass)
    VkRenderPassCreateInfo rpc = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = na + nd, .pAttachments = ads, .subpassCount = 1,
        .pSubpasses = &sp, .dependencyCount = fetch ? 3 : 2, .pDependencies = deps };
    VkRenderPass rp = VK_NULL_HANDLE; VkResult r = vkCreateRenderPass(RDNA4R.dev, &rpc, NULL, &rp);
    if (r != VK_SUCCESS) { RDNA4LOG("render pass: vkCreateRenderPass = %d", r); return VK_NULL_HANDLE; }
    return rp;
}
// P5b: cached passes belong to the device; uncached passes belong to the caller.
static VkRenderPass rdna4_mk_rp_c(const VkAttachmentDescription *ads, uint32_t na, uint32_t nd, BOOL fetch, BOOL *cached) {
    *cached = NO;
    if (!RDNA4DO.on || na + nd > 8) return rdna4_mk_rp(ads, na, nd, fetch);   // bundle 10: the key counts every attachment; 8 colour + depth is not cached
    rdna4dc_rpkey k; memset(&k, 0, sizeof k); k.na = na + nd; k.fetch = (fetch ? 1u : 0u) | (nd << 1);   // nd == 0: exactly the old key
    for (uint32_t i = 0; i < na + nd; i++)
        k.a[i] = (rdna4dc_att){ (uint32_t)ads[i].format, (uint32_t)ads[i].samples, (uint32_t)ads[i].loadOp, (uint32_t)ads[i].storeOp, (uint32_t)ads[i].stencilLoadOp,
                              (uint32_t)ads[i].stencilStoreOp, (uint32_t)ads[i].initialLayout, (uint32_t)ads[i].finalLayout };
    uint64_t h;
    pthread_mutex_lock(&RDNA4DO.mu); h = rdna4dc_rp_find(RDNA4DO.rp, &k); pthread_mutex_unlock(&RDNA4DO.mu);
    if (h) { atomic_fetch_add(&RDNA4DO.rpHits, 1); *cached = YES; return (VkRenderPass)(uintptr_t)h; }
    VkRenderPass rp = rdna4_mk_rp(ads, na, nd, fetch); if (!rp) return VK_NULL_HANDLE;
    pthread_mutex_lock(&RDNA4DO.mu);
    h = rdna4dc_rp_find(RDNA4DO.rp, &k);   // another thread may have entered the same key meanwhile
    if (!h && rdna4dc_rp_add(RDNA4DO.rp, &k, (uint64_t)(uintptr_t)rp)) { pthread_mutex_unlock(&RDNA4DO.mu); atomic_fetch_add(&RDNA4DO.rpCreated, 1); *cached = YES; return rp; }
    pthread_mutex_unlock(&RDNA4DO.mu);
    if (h) { vkDestroyRenderPass(RDNA4R.dev, rp, NULL); atomic_fetch_add(&RDNA4DO.rpHits, 1); *cached = YES; return (VkRenderPass)(uintptr_t)h; }
    atomic_fetch_add(&RDNA4DO.rpUncached, 1);
    return rp;
}

// The encoder owns reusable handles while recording; each consuming segment
// shares ownership until its fence retires. Cached render passes stay device-owned.
@interface RDNA4RenderHandles : NSObject {
    VkRenderPass _renderPass;
    VkFramebuffer _framebuffer;
}
- (instancetype)initWithRenderPass:(VkRenderPass)renderPass
                       framebuffer:(VkFramebuffer)framebuffer;
@end

@implementation RDNA4RenderHandles
- (instancetype)initWithRenderPass:(VkRenderPass)renderPass
                       framebuffer:(VkFramebuffer)framebuffer {
    self = [super init];
    if (!self) return nil;
    _renderPass = renderPass;
    _framebuffer = framebuffer;
    return self;
}
- (void)dealloc {
    if (!RDNA4R.ok) return;
    if (_framebuffer) vkDestroyFramebuffer(RDNA4R.dev, _framebuffer, NULL);
    if (_renderPass) vkDestroyRenderPass(RDNA4R.dev, _renderPass, NULL);
}
@end

@interface RDNA4RenderEncoder : _MTLCommandEncoder {
    RDNA4CommandBuffer *_cb; BOOL _ended, _inPass, _begun, _hostWriteDropped; uint32_t _roWarnV, _roWarnF; uint32_t _w, _h, _na, _layers; NSString *_lbl;
    VkRenderPass _rp1; BOOL _rp1c; VkFramebuffer _fb, _fbf; VkClearValue _cvs[9]; VkAttachmentDescription _ads[9];
    RDNA4RenderHandles *_baseHandles, *_fetchHandles;
    VkImageView _views[9]; RDNA4Texture *_tex[9]; VkImageLayout _curLay[9]; BOOL _fetch;   // bundle 10: slot [_na] is the depth/stencil attachment when _nd == 1; 11e-2: _fetch = the current/next pass is the framebuffer-fetch shape
    uint32_t _nd; VkFormat _dsVkFmt; unsigned _encSamples;   // bundle 10: depth/stencil attachment present; its VkFormat (UNDEFINED = none); the pass's sample count (0 until the first attachment, then >= 1)
    id _rtex[8]; uint32_t _slvl[8], _sslice[8], _rlvl[8], _rslice[8]; NSUInteger _sact[8];   // bundle 10: per colour attachment the resolve texture (+ level, slice), its source level / slice and the store action
    id _drtex, _srtex; uint32_t _dlvl, _dslice, _drlvl, _drslice, _srlvl, _srslice; NSUInteger _dsact, _ssact, _dresFilter, _sresFilter;
    RDNA4DSInfo _dsi; uint32_t _sref[2]; float _bias[3];   // bundle 10: the bound MTLDepthStencilState, the stencil reference values (front, back) and depth bias (constant, clamp, slope)
    RDNA4RenderPipelineState *_pso; MTLRenderPipelineReflection *_roReflection; RDNA4StageState _sv, _sf;
    MTLViewport _vp, _vps[16]; uint32_t _nvp; MTLScissorRect _sr, _srs[16]; uint32_t _nsr;
    MTLTriangleFillMode _fillMode; MTLDepthClipMode _depthClipMode;
    BOOL _hasSamplePos; MTLSamplePosition _samplePos[8]; uint32_t _samplePosCount;
    float _blend[4]; MTLCullMode _cull; MTLWinding _wind;
    id _tessFactorBuf; NSUInteger _tessFactorOff, _tessFactorStride; float _tessFactorScale;
    NSMutableSet *_samplerBindings;
    rdna4dc_enc _dc; BOOL _dcOn;   // P5b: per-draw redundancy state (_dcOn = the rdna4m-drawopt switch as latched at device creation)
    rdna4occ *_occ; VkQueryPool _qp; uint32_t _qbase;   // build 18 (P2): this pass's occlusion bookkeeping (owned by the command buffer), its query pool and the first of its RDNA4OCC_MAXQ reserved query indices
    uint32_t _clrm; NSUInteger _dvc;   // #12 D2: attachments whose load action was Clear and not yet consumed by a presentable draw; vertex/index count of the draw being prepared (diagnostic)
    NSMutableSet *_argumentResources, *_argumentWritable;
    RDNA4ResourceBindings *_resourceBindings;
    uint64_t _resourceSegmentSerial;
    uint32_t _ampCount, _ampLayerOff, _activeLayerOff;
    BOOL _ampHasMappings;
    rdna4amp_mapping _ampMappings[RDNA4AMP_MAX_SUPPORTED_COUNT];
    id<RDNA4CounterSampleBufferInternal> _csbEnd[RDNA4CNT_MAX_PASS_ATTACHMENTS];
    NSUInteger _csbEndV[RDNA4CNT_MAX_PASS_ATTACHMENTS], _csbEndF[RDNA4CNT_MAX_PASS_ATTACHMENTS];
    uint32_t _nCsbEnd;
}
- (instancetype)initWithCommandBuffer:(id)cb descriptor:(MTLRenderPassDescriptor *)d;
- (void)rdna4ReleaseRenderHandles;
@end

@implementation RDNA4RenderEncoder
- (BOOL)rdna4CheckDSResolve:(BOOL)isDepth {
    if (!_nd) return NO;
    RDNA4Texture *src = _tex[_na]; id r = isDepth ? _drtex : _srtex;
    uint32_t rlvl = isDepth ? _drlvl : _srlvl, rslice = isDepth ? _drslice : _srslice;
    NSUInteger filter = isDepth ? _dresFilter : _sresFilter;
    if (!RDNA4R.ok || !vkCreateRenderPass2 || !vkCmdBeginRenderPass2 || !vkCmdEndRenderPass2) {
        RDNA4LOG("render pass: depth/stencil resolve requires Vulkan 1.2 renderPass2; refused"); return NO;
    }
    if (![r isKindOfClass:[RDNA4Texture class]]) { RDNA4LOG("render pass: %s resolve texture is not an RDNA4Texture; refused", isDepth ? "depth" : "stencil"); return NO; }
    RDNA4Texture *dst = r;
    if (dst == src || [dst rdna4IsView] || [dst textureType] == MTLTextureType3D) {
        RDNA4LOG("render pass: %s resolve texture is self, a view or a 3D texture; refused", isDepth ? "depth" : "stencil"); return NO;
    }
    if (rlvl >= [dst mipmapLevelCount] || rslice + _layers > [dst rdna4Layers]) {
        RDNA4LOG("render pass: %s resolve level %u slice %u+%u outside resolve texture; refused", isDepth ? "depth" : "stencil", rlvl, rslice, _layers); return NO;
    }
    unsigned sw = (unsigned)MAX((NSUInteger)1, src.width >> _dlvl), sh = (unsigned)MAX((NSUInteger)1, src.height >> _dlvl);
    unsigned dw = (unsigned)MAX((NSUInteger)1, dst.width >> rlvl), dh = (unsigned)MAX((NSUInteger)1, dst.height >> rlvl);
    VkFormat svkf = [src vkFormat], dvkf = [dst vkFormat];
    BOOL compatFmt = (svkf == dvkf) || (svkf == VK_FORMAT_D32_SFLOAT_S8_UINT && ((isDepth && dvkf == VK_FORMAT_D32_SFLOAT) || (!isDepth && dvkf == VK_FORMAT_S8_UINT)));
    const char *why = NULL;
    if (!rdna4sa_ds_target_ok(sw, sh, (unsigned)svkf, dw, dh, compatFmt ? (unsigned)svkf : (unsigned)dvkf, (unsigned)[dst rdna4Samples], &why)) {
        RDNA4LOG("render pass: %s resolve: %s; refused", isDepth ? "depth" : "stencil", why); return NO;
    }
    if (isDepth) {
        VkResolveModeFlagBits rm = filter == 1 ? VK_RESOLVE_MODE_MIN_BIT : filter == 2 ? VK_RESOLVE_MODE_MAX_BIT : VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
        if (filter > 2 || !(RDNA4R.depthResolveModes & rm)) {
            RDNA4LOG("render pass: depthResolveFilter %lu is not supported on RADV (modes 0x%x); refused", (unsigned long)filter, RDNA4R.depthResolveModes); return NO;
        }
    } else {
        if (filter != 0) {
            RDNA4LOG("render pass: stencilResolveFilter %lu (DepthResolvedSample) is not supported on Vulkan; refused", (unsigned long)filter); return NO;
        }
        if (!(RDNA4R.stencilResolveModes & VK_RESOLVE_MODE_SAMPLE_ZERO_BIT)) {
            RDNA4LOG("render pass: stencil Sample0 resolve is not supported on RADV (modes 0x%x); refused", RDNA4R.stencilResolveModes); return NO;
        }
    }
    return YES;
}
- (instancetype)initWithCommandBuffer:(id)cb descriptor:(MTLRenderPassDescriptor *)d {
    self = [super initWithCommandBuffer:cb];
    if (!self) return nil;
    _cb = cb; _wind = MTLWindingClockwise; _tessFactorScale = 1.0f;
    _fillMode = MTLTriangleFillModeFill; _depthClipMode = MTLDepthClipModeClip;
    _ampCount = 1u; _ampHasMappings = NO; _ampLayerOff = 0u; _activeLayerOff = 0u;
    _resourceBindings = [RDNA4ResourceBindings new];
    _resourceSegmentSerial = [_cb rdna4SegmentSerial];
    NSUInteger reqLayers = [d respondsToSelector:@selector(renderTargetArrayLength)] ? d.renderTargetArrayLength : 0;
    if (reqLayers && (!RDNA4R.ok || reqLayers > RDNA4R.lim.maxFramebufferLayers)) {
        RDNA4LOG("render pass: renderTargetArrayLength %lu exceeds RADV maxFramebufferLayers %u; refused", (unsigned long)reqLayers, RDNA4R.lim.maxFramebufferLayers);
        return nil;
    }
    _layers = reqLayers ? (uint32_t)reqLayers : 1u;
    // Keep attachment indices and render-pass compatibility intact: an unsafe pass records nothing.
    for (NSUInteger i = 0; i < 8; i++) {
        id texture = d.colorAttachments[i].texture;
        if ([texture isKindOfClass:[RDNA4Texture class]] &&
            rdna4nc_drop_write(texture, RDNA4NC_ATTACHMENT, "render attachment store", "RenderCommandEncoder")) {
            _hostWriteDropped = YES;
            [_cb rdna4CBTraceSurface:texture use:"attachment-refused"];
        }
    }
    id depth = d.depthAttachment.texture, stencil = d.stencilAttachment.texture;
    if ([depth isKindOfClass:[RDNA4Texture class]])
        _hostWriteDropped |= rdna4nc_drop_write(depth, RDNA4NC_ATTACHMENT, "depth attachment store", "RenderCommandEncoder");
    if (stencil != depth && [stencil isKindOfClass:[RDNA4Texture class]])
        _hostWriteDropped |= rdna4nc_drop_write(stencil, RDNA4NC_ATTACHMENT, "stencil attachment store", "RenderCommandEncoder");
    if (_hostWriteDropped) { [_cb rdna4Fail:@"render attachment writes a read-only no-copy texture"]; return self; }
    _dcOn = RDNA4DO.on; if (_dcOn) rdna4dc_enc_init(&_dc);
    VkImageView views[9]; uint32_t na = 0; _dsi = rdna4_dsinfo_default();
    for (NSUInteger i = 0; i < 8; i++) {
        MTLRenderPassColorAttachmentDescriptor *ca = d.colorAttachments[i];
        RDNA4Texture *t = (RDNA4Texture *)ca.texture;
        if (!t) continue;
        if (![t isKindOfClass:[RDNA4Texture class]]) { RDNA4LOG("render pass: attachment %lu is not an RDNA4Texture", (unsigned long)i); return nil; }
        NSUInteger tsm = [t rdna4Samples];   // bundle 10: every attachment of a pass has the same sample count
        if (_encSamples && tsm != _encSamples) { RDNA4LOG("render pass: attachment %lu has %lu samples, the others %u; refused", (unsigned long)i, (unsigned long)tsm, _encSamples); return nil; }
        _encSamples = (unsigned)tsm;
        // m11h9: a mip level of a 2D texture can be the attachment (the compositor renders into the base level of mipmapped textures and generates the rest)
        NSUInteger lvl = ca.level;
        if (lvl >= [t mipmapLevelCount] || ca.slice >= [t rdna4Layers] || _layers > [t rdna4Layers] - ca.slice || ca.depthPlane != 0) { RDNA4LOG("render pass: attachment %lu level %lu slice %lu+%u depthPlane %lu outside the texture (%lu level(s), %u slice(s)); refused", (unsigned long)i, (unsigned long)lvl, (unsigned long)ca.slice, _layers, (unsigned long)ca.depthPlane, (unsigned long)[t mipmapLevelCount], [t rdna4Layers]); return nil; }
        const NSUInteger attachmentWidth = MAX((NSUInteger)1, t.width >> lvl);
        const NSUInteger attachmentHeight = MAX((NSUInteger)1, t.height >> lvl);
        if (na == 0) { _w = (uint32_t)attachmentWidth; _h = (uint32_t)attachmentHeight; }
        const BOOL preserveOutsideClear = ca.loadAction == MTLLoadActionClear && [t rdna4IsIOS] &&
            (_w < attachmentWidth || _h < attachmentHeight);
        rdna4_ios_use(_cb, t, ca.loadAction != MTLLoadActionClear || preserveOutsideClear, YES);
        [_cb rdna4DispNote:t bits:(RDNA4DF_W_PASS | (ca.loadAction == MTLLoadActionClear ? RDNA4DF_W_CLEAR : 0)) pso:nil];   // native #12 P1
        BOOL load = (ca.loadAction == MTLLoadActionLoad || ([t rdna4IsIOS] && ca.loadAction != MTLLoadActionClear)) && [t layout] != VK_IMAGE_LAYOUT_UNDEFINED;
        // The layout of a texture is tracked for the WHOLE image. A render pass only transitions its attachment's level, so a multi-level texture is moved to the attachment layout
        // explicitly (outside any pass: the previous encoder has ended) and the pass then starts from that layout.
        BOOL wholeImg = [t rdna4Levels] > 1 || [t rdna4ImageLayers] > 1;   // bundle 9: a layered texture is moved as a whole too (the pass only transitions its one layer)
        VkImageLayout attachmentLayout = rdna4_use_layout(t, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        if (wholeImg) rdna4_tex_to([_cb vk], t, attachmentLayout);
        // storeOp is always STORE: a pass may be split (a texture bound mid-encoder needs a layout change outside the pass) and the
        // continuation LOADs what the first part stored.
        _ads[na] = (VkAttachmentDescription){ .format = [t vkFormat], .samples = (VkSampleCountFlagBits)tsm,
            .loadOp = ca.loadAction == MTLLoadActionClear ? VK_ATTACHMENT_LOAD_OP_CLEAR : load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = wholeImg || attachmentLayout == VK_IMAGE_LAYOUT_GENERAL ? attachmentLayout :
                (load || preserveOutsideClear ? [t layout] : VK_IMAGE_LAYOUT_UNDEFINED),
            .finalLayout = attachmentLayout };
        if (ca.loadAction == MTLLoadActionClear) _clrm |= 1u << na;
        views[na] = _layers > 1
            ? [t rdna4AttViewLevel:(uint32_t)lvl layer:(uint32_t)ca.slice layerCount:_layers]
            : [t rdna4AttViewLevel:(uint32_t)lvl layer:(uint32_t)ca.slice];
        _views[na] = views[na]; _tex[na] = t; _sf.att[na] = t;
        [self rdna4OwnResource:t kind:RDNA4_RESOURCE_ATTACHMENT
                      fragment:NO index:na];
        MTLClearColor c = ca.clearColor;
        if ([t rdna4IsUInt]) {   // bundle 19: an integer attachment is cleared through .uint32 with Metal's conversion (truncate, saturate; measured on an Apple-silicon Mac, rdna4_intfmt.h); .float32 would put float bits in the texture
            unsigned rgbB = 16, aB = 16; [t rdna4IntBitsRGB:&rgbB alpha:&aB];
            _cvs[na] = (VkClearValue){ .color = { .uint32 = { rdna4if_clear_u(c.red, rgbB), rdna4if_clear_u(c.green, rgbB), rdna4if_clear_u(c.blue, rgbB), rdna4if_clear_u(c.alpha, aB) } } };
        } else if ([t rdna4IsSInt]) {
            unsigned rgbB = 32, aB = 32; [t rdna4IntBitsRGB:&rgbB alpha:&aB];
            _cvs[na] = (VkClearValue){ .color = { .int32 = { rdna4if_clear_s(c.red, rgbB), rdna4if_clear_s(c.green, rgbB), rdna4if_clear_s(c.blue, rgbB), rdna4if_clear_s(c.alpha, aB) } } };
        } else
        _cvs[na] = (VkClearValue){ .color = { .float32 = { (float)c.red, (float)c.green, (float)c.blue, (float)c.alpha } } };
        [t setLayout:attachmentLayout];
        // bundle 10: store action / multisample resolve.  The attachment itself is always stored; a resolve texture receives vkCmdResolveImage at endEncoding (a pass may be split, so never inside it).
        _slvl[na] = (uint32_t)lvl; _sslice[na] = (uint32_t)ca.slice; _sact[na] = (NSUInteger)ca.storeAction; _rtex[na] = ca.resolveTexture; _rlvl[na] = (uint32_t)ca.resolveLevel; _rslice[na] = (uint32_t)ca.resolveSlice;
        if (ca.resolveTexture)
            [self rdna4OwnResource:ca.resolveTexture
                              kind:RDNA4_RESOURCE_RESOLVE
                          fragment:NO index:na];
        { rdna4sa_t sat; if (!rdna4sa_map((unsigned long)ca.storeAction, ca.resolveTexture != nil, tsm > 1, &sat)) { RDNA4LOG("render pass: attachment %lu: %s; refused", (unsigned long)i, sat.why); return nil; }
          if (sat.resolve && ![self rdna4CheckResolve:na]) return nil; }
        na++;
    }
    if (d.depthAttachment.texture || d.stencilAttachment.texture) {   // bundle 10: the depth / stencil attachment (one Vulkan attachment: one texture, depth and/or stencil aspect)
        id dtI = d.depthAttachment.texture, stI = d.stencilAttachment.texture;
        MTLRenderPassDepthAttachmentDescriptor *da = d.depthAttachment; MTLRenderPassStencilAttachmentDescriptor *sa = d.stencilAttachment;
        if (dtI && stI && dtI != stI) { RDNA4LOG("render pass: separate depth and stencil textures are not supported (use one Depth32Float_Stencil8 texture); refused"); return nil; }
        id ti = dtI ? dtI : stI;
        if (![ti isKindOfClass:[RDNA4Texture class]]) { RDNA4LOG("render pass: the depth/stencil attachment is not an RDNA4Texture"); return nil; }
        RDNA4Texture *dt = ti; unsigned asp = [dt rdna4Aspects];
        if (![dt rdna4IsDS]) { RDNA4LOG("render pass: the depth/stencil attachment texture has a colour pixel format; refused"); return nil; }
        if ((dtI && !(asp & RDNA4DP_ASP_DEPTH)) || (stI && !(asp & RDNA4DP_ASP_STENCIL))) { RDNA4LOG("render pass: the %s attachment texture (pixel format %lu) has no %s aspect; refused", dtI ? "depth" : "stencil", (unsigned long)dt.pixelFormat, dtI ? "depth" : "stencil"); return nil; }
        if ([dt rdna4IsView] && ([dt pixelFormat] == MTLPixelFormatX32_Stencil8 || asp != rdna4dp_view_aspect(asp))) { RDNA4LOG("render pass: a view of a combined depth/stencil texture cannot be an attachment; refused"); return nil; }
        if (dtI && stI && (da.level != sa.level || da.slice != sa.slice)) { RDNA4LOG("render pass: the depth and stencil attachments name different levels / slices; refused"); return nil; }
        NSUInteger lvl = dtI ? da.level : sa.level, slc = dtI ? da.slice : sa.slice, dpl = dtI ? da.depthPlane : sa.depthPlane;
        if (lvl >= [dt mipmapLevelCount] || slc >= [dt rdna4Layers] || _layers > [dt rdna4Layers] - slc || dpl != 0) { RDNA4LOG("render pass: the depth/stencil attachment level %lu slice %lu+%u depthPlane %lu is outside the texture; refused", (unsigned long)lvl, (unsigned long)slc, _layers, (unsigned long)dpl); return nil; }
        NSUInteger tsm = [dt rdna4Samples];
        if (_encSamples && tsm != _encSamples) { RDNA4LOG("render pass: the depth/stencil attachment has %lu samples, the colour attachments %u; refused", (unsigned long)tsm, _encSamples); return nil; }
        _encSamples = (unsigned)tsm;
        uint32_t dw = (uint32_t)MAX((NSUInteger)1, dt.width >> lvl), dh = (uint32_t)MAX((NSUInteger)1, dt.height >> lvl);
        if (na == 0) { _w = dw; _h = dh; }
        else if (dw < _w || dh < _h) { RDNA4LOG("render pass: the depth/stencil attachment (%ux%u) is smaller than the colour attachments (%ux%u); refused", dw, dh, _w, _h); return nil; }
        rdna4rp_ds_t ops; rdna4rp_ds(asp, dtI != nil, (unsigned)da.loadAction, stI != nil, (unsigned)sa.loadAction, [dt layout] == VK_IMAGE_LAYOUT_UNDEFINED, &ops);
        BOOL wholeImg = [dt rdna4Levels] > 1 || [dt rdna4ImageLayers] > 1;
        if (wholeImg) rdna4_tex_to([_cb vk], dt, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        _ads[na] = (VkAttachmentDescription){ .format = [dt vkFormat], .samples = (VkSampleCountFlagBits)tsm,
            .loadOp = (VkAttachmentLoadOp)ops.depthLoad, .storeOp = (VkAttachmentStoreOp)ops.depthStore, .stencilLoadOp = (VkAttachmentLoadOp)ops.stencilLoad, .stencilStoreOp = (VkAttachmentStoreOp)ops.stencilStore,
            .initialLayout = wholeImg ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : ops.undefinedInitial ? VK_IMAGE_LAYOUT_UNDEFINED : [dt layout], .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
        _cvs[na] = (VkClearValue){ .depthStencil = { (float)da.clearDepth, (uint32_t)sa.clearStencil } };
        views[na] = _layers > 1
            ? [dt rdna4AttViewLevel:(uint32_t)lvl layer:(uint32_t)slc layerCount:_layers]
            : [dt rdna4AttViewLevel:(uint32_t)lvl layer:(uint32_t)slc];
        _views[na] = views[na]; _tex[na] = dt; _dsVkFmt = [dt vkFormat]; _nd = 1;
        _dlvl = (uint32_t)lvl; _dslice = (uint32_t)slc;
        if (dtI) {
            _dsact = (NSUInteger)da.storeAction; _drtex = da.resolveTexture; _drlvl = (uint32_t)da.resolveLevel; _drslice = (uint32_t)da.resolveSlice;
            _dresFilter = [da respondsToSelector:@selector(depthResolveFilter)] ? (NSUInteger)da.depthResolveFilter : 0;
            if (da.resolveTexture) [self rdna4OwnResource:da.resolveTexture kind:RDNA4_RESOURCE_RESOLVE fragment:NO index:8];
        }
        if (stI) {
            _ssact = (NSUInteger)sa.storeAction; _srtex = sa.resolveTexture; _srlvl = (uint32_t)sa.resolveLevel; _srslice = (uint32_t)sa.resolveSlice;
            _sresFilter = [sa respondsToSelector:@selector(stencilResolveFilter)] ? (NSUInteger)sa.stencilResolveFilter : 0;
            if (sa.resolveTexture) [self rdna4OwnResource:sa.resolveTexture kind:RDNA4_RESOURCE_RESOLVE fragment:NO index:9];
        }
        { rdna4sa_t sat;
          if (dtI && !rdna4sa_map((unsigned long)da.storeAction, da.resolveTexture != nil, tsm > 1, &sat)) { RDNA4LOG("render pass: depth attachment: %s; refused", sat.why); return nil; }
          if (dtI && sat.resolve && ![self rdna4CheckDSResolve:YES]) return nil;
          if (stI && !rdna4sa_map((unsigned long)sa.storeAction, sa.resolveTexture != nil, tsm > 1, &sat)) { RDNA4LOG("render pass: stencil attachment: %s; refused", sat.why); return nil; }
          if (stI && sat.resolve && ![self rdna4CheckDSResolve:NO]) return nil; }
        [self rdna4OwnResource:dt kind:RDNA4_RESOURCE_ATTACHMENT
                      fragment:NO index:na];
        [dt setLayout:VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL];
        RDNA4LOGR("render pass: depth/stencil attachment %p pixel format %lu (vk %d) aspects 0x%x level %lu slice %lu, %lu sample(s), depth load %u store %u, stencil load %u store %u", (__bridge void *)dt, (unsigned long)dt.pixelFormat, [dt vkFormat], asp,
                (unsigned long)lvl, (unsigned long)slc, (unsigned long)tsm, ops.depthLoad, ops.depthStore, ops.stencilLoad, ops.stencilStore);
    }
    if (!_encSamples) _encSamples = 1;
    if (!na && !_nd) { RDNA4LOG("render pass: no colour or depth/stencil attachments"); return nil; }
    if ([d respondsToSelector:@selector(getSamplePositions:count:)]) {
        NSUInteger spc = [d getSamplePositions:NULL count:0];
        if (spc > 0) {
            if (!RDNA4R.sampleLocationsExt || spc != _encSamples || spc > 8 || !(RDNA4R.sampleLocationCounts & _encSamples)) {
                RDNA4LOG("render pass: programmable sample positions count %lu for %u samples unsupported (ext %d counts 0x%x); refused",
                         (unsigned long)spc, _encSamples, RDNA4R.sampleLocationsExt, RDNA4R.sampleLocationCounts);
                return nil;
            }
            [d getSamplePositions:_samplePos count:spc];
            _samplePosCount = (uint32_t)spc;
            _hasSamplePos = YES;
        }
    }
    if ([d respondsToSelector:@selector(sampleBufferAttachments)]) {
        MTLRenderPassSampleBufferAttachmentDescriptorArray *sbas = d.sampleBufferAttachments;
        for (NSUInteger k = 0; sbas && k < RDNA4CNT_MAX_PASS_ATTACHMENTS; k++) {
            MTLRenderPassSampleBufferAttachmentDescriptor *att = sbas[k];
            if (!att || !att.sampleBuffer) continue;
            NSString *cwhy = nil;
            id<RDNA4CounterSampleBufferInternal> csb = rdna4_counter_buffer_check(att.sampleBuffer, [_cb device], 0, NO, &cwhy);
            if (!csb || !rdna4_counter_pass_index_valid(csb, att.startOfVertexSampleIndex) ||
                !rdna4_counter_pass_index_valid(csb, att.endOfVertexSampleIndex) ||
                !rdna4_counter_pass_index_valid(csb, att.startOfFragmentSampleIndex) ||
                !rdna4_counter_pass_index_valid(csb, att.endOfFragmentSampleIndex)) {
                RDNA4LOG("render pass: sampleBufferAttachments[%lu] invalid (%s); refused",
                         (unsigned long)k, cwhy ? cwhy.UTF8String : "sample index out of range");
                _ended = YES;
                [super endEncoding];
                return nil;
            }
        }
    }
    _na = na;
    for (uint32_t i = 0; i < na + _nd; i++) _curLay[i] = _ads[i].initialLayout;
    _rp1 = rdna4_mk_rp_c(_ads, na, _nd, NO, &_rp1c); if (!_rp1) return nil;
    VkFramebufferCreateInfo fbc = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = _rp1, .attachmentCount = na + _nd, .pAttachments = views, .width = _w, .height = _h, .layers = _layers };
    VkResult r = vkCreateFramebuffer(RDNA4R.dev, &fbc, NULL, &_fb);
    if (r != VK_SUCCESS) { RDNA4LOG("render pass: vkCreateFramebuffer = %d", r); if (!_rp1c) vkDestroyRenderPass(RDNA4R.dev, _rp1, NULL); return nil; }
    _baseHandles = [[RDNA4RenderHandles alloc]
        initWithRenderPass:_rp1c ? VK_NULL_HANDLE : _rp1 framebuffer:_fb];
    // Metal's default viewport = the attachment, y down; translated shaders do not flip clip Y (F6): the viewport is emitted with a negative height.
    _vp = (MTLViewport){ 0, 0, _w, _h, 0, 1 }; _sr = (MTLScissorRect){ 0, 0, _w, _h };
    _vps[0] = _vp; _nvp = 1; _srs[0] = _sr; _nsr = 1;
    RDNA4LOGR("RDNA4RenderEncoder %p: %u x %u x %u, %u attachment(s); the Vulkan render pass begins at the first draw (or at endEncoding)", (__bridge void *)self, _w, _h, _layers, na);
    if ([d respondsToSelector:@selector(sampleBufferAttachments)]) {
        MTLRenderPassSampleBufferAttachmentDescriptorArray *sbas = d.sampleBufferAttachments;
        for (NSUInteger k = 0; sbas && k < RDNA4CNT_MAX_PASS_ATTACHMENTS; k++) {
            MTLRenderPassSampleBufferAttachmentDescriptor *att = sbas[k];
            if (!att || !att.sampleBuffer) continue;
            id<RDNA4CounterSampleBufferInternal> csb = (id<RDNA4CounterSampleBufferInternal>)att.sampleBuffer;
            if (att.startOfVertexSampleIndex != MTLCounterDontSample)
                [csb rdna4SampleOnCommandBuffer:_cb vkCommandBuffer:[_cb vk]
                                    sampleIndex:att.startOfVertexSampleIndex barrier:NO
                                     stageFlags:VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT who:"render-pass-start-vertex"];
            if (att.startOfFragmentSampleIndex != MTLCounterDontSample)
                [csb rdna4SampleOnCommandBuffer:_cb vkCommandBuffer:[_cb vk]
                                    sampleIndex:att.startOfFragmentSampleIndex barrier:NO
                                     stageFlags:VK_PIPELINE_STAGE_VERTEX_SHADER_BIT who:"render-pass-start-fragment"];
            if ((att.endOfVertexSampleIndex != MTLCounterDontSample ||
                 att.endOfFragmentSampleIndex != MTLCounterDontSample) &&
                _nCsbEnd < RDNA4CNT_MAX_PASS_ATTACHMENTS) {
                _csbEnd[_nCsbEnd] = csb;
                _csbEndV[_nCsbEnd] = att.endOfVertexSampleIndex;
                _csbEndF[_nCsbEnd] = att.endOfFragmentSampleIndex;
                _nCsbEnd++;
            }
        }
    }
    [self rdna4OccInit:d];   // build 18 (P2): outside any render pass (the previous encoder has ended), so the query reset is legal
    return self;
}
RDNA4_DNR(RDNA4RenderEncoder)
- (void)rdna4OwnResource:(id)object kind:(uint32_t)kind
                fragment:(BOOL)fragment index:(NSUInteger)index {
    [_resourceBindings setObject:object
                            slot:rdna4_resource_slot(kind, fragment, index)];
}
- (void)rdna4SyncResourceSegment {
    uint64_t serial = [_cb rdna4SegmentSerial];
    if (!rdna4_encoder_take_segment_change(&_resourceSegmentSerial, serial))
        return;
    if (_dcOn) rdna4dc_enc_reset_bound(&_dc);
}
- (void)rdna4SnapshotResources {
    [_resourceBindings snapshotToCommandBuffer:_cb];
    [_cb rdna4RetainEncoded:_baseHandles];
    [_cb rdna4RetainEncoded:_fetchHandles];
}
- (void)rdna4ReleaseRenderHandles {
    _baseHandles = nil;
    _fetchHandles = nil;
    _rp1 = VK_NULL_HANDLE;
    _fb = _fbf = VK_NULL_HANDLE;
}
// ---- build 18 (P2): occlusion queries (rdna4_occ.h has the model and the measured Metal semantics) ----
// Called at the end of the encoder's init. A pass with a visibility result buffer gets RDNA4OCC_MAXQ query indices from the command buffer's pool, reset HERE (a reset is illegal inside a render pass).
- (void)rdna4OccInit:(MTLRenderPassDescriptor *)d {
    id vb = d.visibilityResultBuffer;
    if (!vb) return;
    if (![vb isKindOfClass:[RDNA4Buffer class]] || ![(RDNA4Buffer *)vb contents]) { RDNA4_ONCE("occlusion: the visibility result buffer is not a CPU-mapped RDNA4Buffer; its offsets are not written"); return; }
    if (rdna4nc_drop_write(vb, RDNA4NC_VISIBILITY, "visibility result store", "RenderCommandEncoder")) { [_cb rdna4Fail:@"visibility result buffer is read-only"]; return; }
    int acc = 0;
    if ([d respondsToSelector:@selector(visibilityResultType)]) acc = (long)[(id)d visibilityResultType] == 1;   // MTLVisibilityResultTypeAccumulate
    rdna4occ *o = [_cb rdna4OccNew:(RDNA4Buffer *)vb accumulate:acc pool:&_qp base:&_qbase];
    if (!o) return;
    _occ = o;
    if (_qp != VK_NULL_HANDLE) vkCmdResetQueryPool([_cb vk], _qp, _qbase, RDNA4OCC_MAXQ);
    RDNA4LOGR("occlusion: pass with a visibility buffer (%lu bytes, %s)", (unsigned long)[vb length], acc ? "Accumulate" : "Reset");
}
- (void)rdna4OccEndQuery {   // the open interval ends here: always BEFORE the render pass it began in ends
    uint32_t q;
    if (_occ && rdna4occ_end(_occ, &q)) vkCmdEndQuery([_cb vk], _qp, _qbase + q);
}
- (void)rdna4OccDraw {   // inside the render pass, right before a draw is recorded
    uint32_t q; int precise = 0;
    if (_occ && rdna4occ_draw(_occ, &q, &precise)) vkCmdBeginQuery([_cb vk], _qp, _qbase + q, (precise && RDNA4R.occPrecise) ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
}
- (void)rdna4EndPass {   // THE one place a Vulkan render pass of this encoder ends (the pass is split by layout changes, uploads, barriers, framebuffer fetch; each part is a new query interval)
    if (!_inPass) return;
    [self rdna4OccEndQuery];
    vkCmdEndRenderPass([_cb vk]); _inPass = NO;
}
- (void)setVisibilityResultMode:(NSUInteger)mode offset:(NSUInteger)offset {
    if (!_occ) { RDNA4_ONCE("setVisibilityResultMode:offset: on a pass without a (usable) visibility result buffer: ignored"); return; }
    int endq = 0; int rc = rdna4occ_set(_occ, mode, offset, &endq);
    if (endq) vkCmdEndQuery([_cb vk], _qp, _qbase + _occ->activeQ);   // rdna4occ_set closed the interval; activeQ names its query
    if (rc) RDNA4LOG("setVisibilityResultMode:%lu offset:%lu REJECTED (mode > 2, offset not a multiple of 8 or past the %llu-byte buffer): treated as Disabled", (unsigned long)mode, (unsigned long)offset, (unsigned long long)_occ->buflen);
}
- (NSString *)label { return _lbl; }
- (void)setLabel:(NSString *)l { _lbl = [l copy]; }
- (NSUInteger)getType { return 1; }

// ---- bundle 10 + Metal 2: multisample resolve ----
// 1 when colour attachment i's resolve texture is usable (a 2D single-sample texture of the attachment's size and format); logs and returns 0 otherwise.
- (BOOL)rdna4CheckResolve:(uint32_t)i {
    RDNA4Texture *src = _tex[i]; id r = _rtex[i];
    if (![r isKindOfClass:[RDNA4Texture class]]) { RDNA4LOG("render pass: attachment %u: the resolve texture is not an RDNA4Texture; refused", i); return NO; }
    RDNA4Texture *dst = r;
    if (dst == src || [dst rdna4IsView] || [dst textureType] == MTLTextureType3D) { RDNA4LOG("render pass: attachment %u: the resolve texture is the attachment itself, a view or a 3D texture; refused", i); return NO; }
    if (_rlvl[i] >= [dst mipmapLevelCount] || _rslice[i] + _layers > [dst rdna4Layers]) { RDNA4LOG("render pass: attachment %u: resolve level %u slice %u+%u is outside the resolve texture; refused", i, _rlvl[i], _rslice[i], _layers); return NO; }
    unsigned sw = (unsigned)MAX((NSUInteger)1, src.width >> _slvl[i]), sh = (unsigned)MAX((NSUInteger)1, src.height >> _slvl[i]), dw = (unsigned)MAX((NSUInteger)1, dst.width >> _rlvl[i]), dh = (unsigned)MAX((NSUInteger)1, dst.height >> _rlvl[i]);
    const char *why = NULL;
    if (!rdna4sa_target_ok(sw, sh, (unsigned)[src vkFormat], [src rdna4Aspects], dw, dh, (unsigned)[dst vkFormat], (unsigned)[dst rdna4Samples], &why)) { RDNA4LOG("render pass: attachment %u: %s; refused", i, why); return NO; }
    return YES;
}
- (void)rdna4RunDSResolveInto:(RDNA4Texture *)dst level:(uint32_t)dlvl slice:(uint32_t)dslice
                    depthMode:(VkResolveModeFlagBits)dMode stencilMode:(VkResolveModeFlagBits)sMode {
    RDNA4Texture *src = _tex[_na];
    VkCommandBuffer cmd = [_cb vk];
    if (rdna4nc_drop_write(dst, RDNA4NC_RESOLVE, "depth/stencil resolve", "RenderCommandEncoder")) { [_cb rdna4Fail:@"depth/stencil resolve destination is read-only"]; return; }
    [_cb rdna4RetainEncoded:src];
    [_cb rdna4RetainEncoded:dst];
    rdna4_ios_usek(_cb, dst, NO, YES, RDNA4DF_W_BLIT);
    rdna4_tex_to(cmd, src, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    rdna4_tex_to(cmd, dst, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    uint32_t sw = (uint32_t)MAX((NSUInteger)1, src.width >> _dlvl), sh = (uint32_t)MAX((NSUInteger)1, src.height >> _dlvl);
    VkImageView srcView = _layers > 1 ? [src rdna4AttViewLevel:_dlvl layer:_dslice layerCount:_layers] : [src rdna4AttViewLevel:_dlvl layer:_dslice];
    VkImageView dstView = _layers > 1 ? [dst rdna4AttViewLevel:dlvl layer:dslice layerCount:_layers] : [dst rdna4AttViewLevel:dlvl layer:dslice];
    unsigned srcAsp = [src rdna4Aspects], dstAsp = [dst rdna4Aspects];
    BOOL dstHasD = (dstAsp & RDNA4DP_ASP_DEPTH) != 0, dstHasS = (dstAsp & RDNA4DP_ASP_STENCIL) != 0;
    VkAttachmentDescription2 ad[2] = {
        { .sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2, .format = [src vkFormat], .samples = (VkSampleCountFlagBits)[src rdna4Samples],
          .loadOp = (srcAsp & RDNA4DP_ASP_DEPTH) ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
          .storeOp = (srcAsp & RDNA4DP_ASP_DEPTH) ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE,
          .stencilLoadOp = (srcAsp & RDNA4DP_ASP_STENCIL) ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
          .stencilStoreOp = (srcAsp & RDNA4DP_ASP_STENCIL) ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE,
          .initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL },
        { .sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2, .format = [dst vkFormat], .samples = VK_SAMPLE_COUNT_1_BIT,
          .loadOp = !dstHasD ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : dMode ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_LOAD,
          .storeOp = dstHasD ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE,
          .stencilLoadOp = !dstHasS ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : sMode ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_LOAD,
          .stencilStoreOp = dstHasS ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE,
          .initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL }
    };
    VkAttachmentReference2 refSrc = { .sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, .attachment = 0, .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, .aspectMask = (VkImageAspectFlags)srcAsp };
    VkAttachmentReference2 refDst = { .sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, .attachment = 1, .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, .aspectMask = (VkImageAspectFlags)dstAsp };
    VkSubpassDescriptionDepthStencilResolve dsRes = { .sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE,
        .depthResolveMode = dMode, .stencilResolveMode = sMode, .pDepthStencilResolveAttachment = &refDst };
    VkSubpassDescription2 sp = { .sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2, .pNext = &dsRes,
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .pDepthStencilAttachment = &refSrc };
    VkSubpassDependency2 deps[2] = {
        { .sType = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2, .srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0,
          .srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, .dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
          .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT },
        { .sType = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2, .srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL,
          .srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, .dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
          .srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT }
    };
    VkRenderPassCreateInfo2 rpci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2, .attachmentCount = 2, .pAttachments = ad,
        .subpassCount = 1, .pSubpasses = &sp, .dependencyCount = 2, .pDependencies = deps };
    VkRenderPass rp = VK_NULL_HANDLE;
    VkResult vr = vkCreateRenderPass2(RDNA4R.dev, &rpci, NULL, &rp);
    if (vr != VK_SUCCESS) { [_cb rdna4Fail:[NSString stringWithFormat:@"vkCreateRenderPass2 (depth/stencil resolve) = %d", vr]]; return; }
    VkImageView views[2] = { srcView, dstView };
    VkFramebufferCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = 2, .pAttachments = views, .width = sw, .height = sh, .layers = _layers };
    VkFramebuffer fb = VK_NULL_HANDLE;
    vr = vkCreateFramebuffer(RDNA4R.dev, &fci, NULL, &fb);
    if (vr != VK_SUCCESS) { vkDestroyRenderPass(RDNA4R.dev, rp, NULL); [_cb rdna4Fail:[NSString stringWithFormat:@"vkCreateFramebuffer (depth/stencil resolve) = %d", vr]]; return; }
    [_cb rdna4RetainEncoded:[[RDNA4RenderHandles alloc] initWithRenderPass:rp framebuffer:fb]];
    VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb, .renderArea = { { 0, 0 }, { sw, sh } } };
    VkSubpassBeginInfo sbi = { .sType = VK_STRUCTURE_TYPE_SUBPASS_BEGIN_INFO, .contents = VK_SUBPASS_CONTENTS_INLINE };
    VkSubpassEndInfo sei = { .sType = VK_STRUCTURE_TYPE_SUBPASS_END_INFO };
    vkCmdBeginRenderPass2(cmd, &rbi, &sbi);
    vkCmdEndRenderPass2(cmd, &sei);
    [dst setLayout:VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL];
    [src setLayout:VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL];
}
// At endEncoding (after the last vkCmdEndRenderPass): every colour and depth/stencil attachment whose FINAL store action resolves is resolved into its resolve texture.
- (void)rdna4ResolveAll {
    VkCommandBuffer cmd = [_cb vk];
    for (uint32_t i = 0; i < _na; i++) {
        rdna4sa_t sat; RDNA4Texture *src = _tex[i];
        if (!rdna4sa_map((unsigned long)_sact[i], _rtex[i] != nil, [src rdna4Samples] > 1, &sat)) { [_cb rdna4Fail:[NSString stringWithFormat:@"attachment %u: %s", i, sat.why]]; continue; }
        if (!sat.resolve) continue;
        if (![self rdna4CheckResolve:i]) { [_cb rdna4Fail:[NSString stringWithFormat:@"attachment %u: the resolve texture is unusable (see the log)", i]]; continue; }
        RDNA4Texture *dst = _rtex[i];
        if (rdna4nc_drop_write(dst, RDNA4NC_RESOLVE, "multisample resolve", "RenderCommandEncoder")) { [_cb rdna4Fail:@"multisample resolve destination is read-only"]; continue; }
        [_cb rdna4RetainEncoded:dst];
        rdna4_ios_usek(_cb, dst, NO, YES, RDNA4DF_W_BLIT);
        rdna4_tex_to(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL); rdna4_tex_to(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageResolve rg = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, _slvl[i], _sslice[i], _layers }, .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, _rlvl[i], _rslice[i], _layers },
            .extent = { (uint32_t)MAX((NSUInteger)1, src.width >> _slvl[i]), (uint32_t)MAX((NSUInteger)1, src.height >> _slvl[i]), 1 } };
        vkCmdResolveImage(cmd, [src vkImage], rdna4_use_layout(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
            [dst vkImage], rdna4_use_layout(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL), 1, &rg);
        RDNA4LOGR("render pass: attachment %u (%lu samples) resolved into texture %p level %u slice %u+%u", i, (unsigned long)[src rdna4Samples], (__bridge void *)dst, _rlvl[i], _rslice[i], _layers);
    }
    if (_nd) {
        RDNA4Texture *dsSrc = _tex[_na];
        unsigned srcSamples = (unsigned)[dsSrc rdna4Samples];
        rdna4sa_t dsat = {0}, ssat = {0};
        BOOL wantD = _drtex != nil || _dsact == MTLStoreActionMultisampleResolve || _dsact == MTLStoreActionStoreAndMultisampleResolve;
        BOOL wantS = _srtex != nil || _ssact == MTLStoreActionMultisampleResolve || _ssact == MTLStoreActionStoreAndMultisampleResolve;
        BOOL resD = NO, resS = NO;
        if (wantD && ([dsSrc rdna4Aspects] & RDNA4DP_ASP_DEPTH)) {
            if (!rdna4sa_map((unsigned long)_dsact, _drtex != nil, srcSamples > 1, &dsat)) { [_cb rdna4Fail:[NSString stringWithFormat:@"depth resolve: %s", dsat.why]]; return; }
            if (dsat.resolve) {
                if (![self rdna4CheckDSResolve:YES]) { [_cb rdna4Fail:@"depth resolve target or filter invalid"]; return; }
                resD = YES;
            }
        }
        if (wantS && ([dsSrc rdna4Aspects] & RDNA4DP_ASP_STENCIL)) {
            if (!rdna4sa_map((unsigned long)_ssact, _srtex != nil, srcSamples > 1, &ssat)) { [_cb rdna4Fail:[NSString stringWithFormat:@"stencil resolve: %s", ssat.why]]; return; }
            if (ssat.resolve) {
                if (![self rdna4CheckDSResolve:NO]) { [_cb rdna4Fail:@"stencil resolve target or filter invalid"]; return; }
                resS = YES;
            }
        }
        VkResolveModeFlagBits dMode = _dresFilter == 1 ? VK_RESOLVE_MODE_MIN_BIT : _dresFilter == 2 ? VK_RESOLVE_MODE_MAX_BIT : VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
        if (resD && resS && _drtex == _srtex && _drlvl == _srlvl && _drslice == _srslice) {
            [self rdna4RunDSResolveInto:_drtex level:_drlvl slice:_drslice depthMode:dMode stencilMode:VK_RESOLVE_MODE_SAMPLE_ZERO_BIT];
        } else {
            if (resD) [self rdna4RunDSResolveInto:_drtex level:_drlvl slice:_drslice depthMode:dMode stencilMode:VK_RESOLVE_MODE_NONE];
            if (resS) [self rdna4RunDSResolveInto:_srtex level:_srlvl slice:_srslice depthMode:VK_RESOLVE_MODE_NONE stencilMode:VK_RESOLVE_MODE_SAMPLE_ZERO_BIT];
        }
    }
}
// ---- pass management ----
- (void)rdna4BeginPass {
    if (_hostWriteDropped) return;
    if (_inPass) return;
    if (_dcOn) rdna4dc_enc_reset_bound(&_dc);   // P5b: a new Vulkan render pass starts: forget the bound pipeline and the cached descriptor sets
    VkRenderPass rp = _rp1; VkFramebuffer fb = _fb; uint32_t ncv = _begun ? 0 : _na + _nd;
    BOOL rpc = _rp1c;
    RDNA4RenderHandles *continuationHandles = nil;
    if (_begun || _fetch || _ampLayerOff) {   // continuation/fetch/shifted-layer passes use the attachments' current layouts
        VkAttachmentDescription a2[9];
        for (uint32_t i = 0; i < _na; i++) {
            a2[i] = _ads[i]; a2[i].initialLayout = _curLay[i];
            if (_begun) a2[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            a2[i].finalLayout = _fetch ? VK_IMAGE_LAYOUT_GENERAL : _ads[i].finalLayout;
        }
        if (_nd) {   // bundle 10: the depth/stencil attachment continues with what the first part stored
            a2[_na] = _ads[_na]; a2[_na].initialLayout = _curLay[_na]; a2[_na].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            if (_begun) { a2[_na].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; a2[_na].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD; }
        }
        rp = rdna4_mk_rp_c(a2, _na, _nd, _fetch, &rpc);
        if (!rp) { [_cb rdna4Fail:@"cannot create the continuation / framebuffer-fetch render pass"]; return; }
        if (_ampLayerOff && _ampLayerOff < _layers) {
            VkImageView ampViews[9];
            uint32_t ampLayers = _layers - _ampLayerOff;
            for (uint32_t i = 0; i < _na; i++) {
                ampViews[i] = ampLayers > 1
                    ? [_tex[i] rdna4AttViewLevel:_slvl[i] layer:_sslice[i] + _ampLayerOff layerCount:ampLayers]
                    : [_tex[i] rdna4AttViewLevel:_slvl[i] layer:_sslice[i] + _ampLayerOff];
            }
            if (_nd) {
                ampViews[_na] = ampLayers > 1
                    ? [_tex[_na] rdna4AttViewLevel:_dlvl layer:_dslice + _ampLayerOff layerCount:ampLayers]
                    : [_tex[_na] rdna4AttViewLevel:_dlvl layer:_dslice + _ampLayerOff];
            }
            BOOL viewsOk = YES;
            for (uint32_t i = 0; i < _na + _nd; i++) {
                if (ampViews[i] == VK_NULL_HANDLE) { viewsOk = NO; break; }
            }
            if (!viewsOk) {
                if (!rpc) vkDestroyRenderPass(RDNA4R.dev, rp, NULL);
                [_cb rdna4Fail:@"amplification layer offset: attachment image view unavailable"];
                return;
            }
            VkFramebuffer ampFb = VK_NULL_HANDLE;
            VkFramebufferCreateInfo fbc = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp,
                .attachmentCount = _na + _nd, .pAttachments = ampViews, .width = _w, .height = _h, .layers = ampLayers };
            VkResult r = vkCreateFramebuffer(RDNA4R.dev, &fbc, NULL, &ampFb);
            if (r != VK_SUCCESS) {
                if (!rpc) vkDestroyRenderPass(RDNA4R.dev, rp, NULL);
                [_cb rdna4Fail:[NSString stringWithFormat:@"vkCreateFramebuffer (amplification layer offset) = %d", r]];
                return;
            }
            continuationHandles = [[RDNA4RenderHandles alloc]
                initWithRenderPass:rpc ? VK_NULL_HANDLE : rp framebuffer:ampFb];
            fb = ampFb;
        } else if (_fetch) {
            if (!_fbf) {   // the fetch framebuffer: same views, a fetch-shaped (input-attachment) render pass for compatibility
                VkFramebufferCreateInfo fbc = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = _na + _nd, .pAttachments = _views, .width = _w, .height = _h, .layers = _layers };
                VkResult r = vkCreateFramebuffer(RDNA4R.dev, &fbc, NULL, &_fbf);
                if (r != VK_SUCCESS) { _fbf = VK_NULL_HANDLE; if (!rpc) vkDestroyRenderPass(RDNA4R.dev, rp, NULL); [_cb rdna4Fail:[NSString stringWithFormat:@"vkCreateFramebuffer (fetch) = %d", r]]; return; }
                _fetchHandles = [[RDNA4RenderHandles alloc]
                    initWithRenderPass:rpc ? VK_NULL_HANDLE : rp framebuffer:_fbf];
            } else if (!rpc) continuationHandles = [[RDNA4RenderHandles alloc]
                initWithRenderPass:rp framebuffer:VK_NULL_HANDLE];
            fb = _fbf;
        } else if (!rpc) continuationHandles = [[RDNA4RenderHandles alloc]
            initWithRenderPass:rp framebuffer:VK_NULL_HANDLE];
    }
    for (uint32_t i = 0; i < _na; i++) { _curLay[i] = _fetch ? VK_IMAGE_LAYOUT_GENERAL : _ads[i].finalLayout; [_tex[i] setLayout:_curLay[i]]; }
    if (_nd) { _curLay[_na] = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL; [_tex[_na] setLayout:_curLay[_na]]; }
    VkSampleLocationEXT vkSL[8];
    if (_hasSamplePos) for (uint32_t s = 0; s < _samplePosCount && s < 8; s++) vkSL[s] = (VkSampleLocationEXT){ _samplePos[s].x, _samplePos[s].y };
    VkSampleLocationsInfoEXT slInfo = { .sType = VK_STRUCTURE_TYPE_SAMPLE_LOCATIONS_INFO_EXT,
        .sampleLocationsPerPixel = (VkSampleCountFlagBits)_encSamples,
        .sampleLocationGridSize = { 1, 1 },
        .sampleLocationsCount = _samplePosCount, .pSampleLocations = vkSL };
    VkAttachmentSampleLocationsEXT attLoc = { .attachmentIndex = _na, .sampleLocationsInfo = slInfo };
    VkSubpassSampleLocationsEXT postLoc = { .subpassIndex = 0, .sampleLocationsInfo = slInfo };
    VkRenderPassSampleLocationsBeginInfoEXT rpSL = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_SAMPLE_LOCATIONS_BEGIN_INFO_EXT,
        .attachmentInitialSampleLocationsCount = _nd ? 1u : 0u, .pAttachmentInitialSampleLocations = _nd ? &attLoc : NULL,
        .postSubpassSampleLocationsCount = 1u, .pPostSubpassSampleLocations = &postLoc };
    VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .pNext = _hasSamplePos ? &rpSL : NULL,
        .renderPass = rp, .framebuffer = fb, .renderArea = { { 0, 0 }, { _w, _h } }, .clearValueCount = ncv, .pClearValues = _cvs };
    // A clear-only pass can begin before its encoder's first draw. Snapshot
    // attachments into the segment that actually records the pass.
    [self rdna4SnapshotResources];
    [_cb rdna4RetainEncoded:continuationHandles];
    vkCmdBeginRenderPass([_cb vk], &rbi, VK_SUBPASS_CONTENTS_INLINE);
    if (!_begun) for (uint32_t index = 0; index < _na; index++)
        if (_ads[index].loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR)
            [_cb rdna4NoteSurfaceOverwrite:_tex[index] width:_w height:_h level:_slvl[index]];
    _inPass = YES; _begun = YES; _activeLayerOff = _ampLayerOff;
}
- (void)rdna4Need:(RDNA4Texture *)t layout:(VkImageLayout)l {
    l = rdna4_use_layout(t, l);
    if ([t layout] == l) return;
    if (_inPass) { [self rdna4EndPass]; RDNA4LOGR("render pass split: texture %p needs layout %d (was %d)", (__bridge void *)t, l, [t layout]); }
    rdna4_tex_to([_cb vk], t, l);
}

// ---- state ----
- (void)setRenderPipelineState:(id)pso {
    if (![pso isKindOfClass:[RDNA4RenderPipelineState class]]) { RDNA4LOG("setRenderPipelineState: not an RDNA4RenderPipelineState"); return; }
    _pso = pso; _roReflection = [_pso reflection];
    _sv.scanPlan = [_pso scanPlanV];
    _sf.scanPlan = [_pso scanPlanF];
    _sv.rtAsRefl = [_pso rdna4RtAsReflV];
    _sf.rtAsRefl = [_pso rdna4RtAsReflF];
    [self rdna4OwnResource:pso kind:RDNA4_RESOURCE_PIPELINE
                  fragment:NO index:0];
}
- (void)setCullMode:(MTLCullMode)m { _cull = m; }
- (void)setFrontFacingWinding:(MTLWinding)w { _wind = w; }
- (void)setViewport:(MTLViewport)v { _vp = v; _vps[0] = v; _nvp = 1; }
- (void)setViewports:(const MTLViewport *)v count:(NSUInteger)n {
    if (!n || !v) return;
    if (n > (RDNA4R.maxViewports ?: 1u)) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"setViewports: count %lu > maxViewports %u", (unsigned long)n, RDNA4R.maxViewports ?: 1u]];
        return;
    }
    for (NSUInteger i = 0; i < n; i++) _vps[i] = v[i];
    _nvp = (uint32_t)n; _vp = v[0];
}
- (void)setScissorRect:(MTLScissorRect)r { _sr = r; _srs[0] = r; _nsr = 1; }
- (void)setScissorRects:(const MTLScissorRect *)r count:(NSUInteger)n {
    if (!n || !r) return;
    if (n > (RDNA4R.maxViewports ?: 1u)) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"setScissorRects: count %lu > maxViewports %u", (unsigned long)n, RDNA4R.maxViewports ?: 1u]];
        return;
    }
    for (NSUInteger i = 0; i < n; i++) _srs[i] = r[i];
    _nsr = (uint32_t)n; _sr = r[0];
}
- (void)setVertexAmplificationCount:(NSUInteger)count
                       viewMappings:(const MTLVertexAmplificationViewMapping *)viewMappings {
    if (_hostWriteDropped) return;
    if (_ended) { [_cb rdna4Fail:@"render setVertexAmplificationCount after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    rdna4amp_mapping tmp[RDNA4AMP_MAX_SUPPORTED_COUNT];
    const rdna4amp_mapping *mp = NULL;
    if (viewMappings && count >= 1u && count <= RDNA4AMP_MAX_SUPPORTED_COUNT) {
        for (NSUInteger i = 0; i < count; i++) {
            tmp[i].viewport_offset = viewMappings[i].viewportArrayIndexOffset;
            tmp[i].layer_offset = viewMappings[i].renderTargetArrayIndexOffset;
        }
        mp = tmp;
    }
    const char *why = NULL;
    if (!rdna4amp_validate_encoder_set((uint64_t)count, mp, RDNA4AMP_MAX_SUPPORTED_COUNT,
                                       RDNA4R.maxViewports ?: 1u, _layers, &why)) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"setVertexAmplificationCount:%lu: %s",
                        (unsigned long)count, why ?: "invalid"]];
        return;
    }
    _ampCount = (uint32_t)count;
    _ampHasMappings = (viewMappings != NULL);
    if (_ampHasMappings) memcpy(_ampMappings, tmp, count * sizeof(rdna4amp_mapping));
    else memset(_ampMappings, 0, sizeof _ampMappings);
    _ampLayerOff = _ampHasMappings ? _ampMappings[0].layer_offset : 0u;
}
- (void)sampleCountersInBuffer:(id<MTLCounterSampleBuffer>)sampleBuffer
                 atSampleIndex:(NSUInteger)sampleIndex
                   withBarrier:(BOOL)barrier {
    if (_hostWriteDropped) return;
    if (_ended) { [_cb rdna4Fail:@"render sampleCountersInBuffer after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    NSString *why = nil;
    id<RDNA4CounterSampleBufferInternal> csb = rdna4_counter_buffer_check(sampleBuffer, [_cb device], sampleIndex, YES, &why);
    if (!csb) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"render sampleCountersInBuffer: %@", why ?: @"invalid"]];
        return;
    }
    // vkCmdResetQueryPool is forbidden inside an active Vulkan render pass; split cleanly first.
    if (_inPass) [self rdna4EndPass];
    [csb rdna4SampleOnCommandBuffer:_cb vkCommandBuffer:[_cb vk] sampleIndex:sampleIndex barrier:barrier
                         stageFlags:(barrier ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT)
                                who:"render"];
}
- (void)setBlendColorRed:(float)r green:(float)g blue:(float)b alpha:(float)a { _blend[0] = r; _blend[1] = g; _blend[2] = b; _blend[3] = a; }
- (void)setTriangleFillMode:(MTLTriangleFillMode)m { _fillMode = m; }
- (void)setDepthClipMode:(NSUInteger)m { _depthClipMode = (MTLDepthClipMode)m; }
- (void)setDepthBias:(float)a slopeScale:(float)b clamp:(float)c { _bias[0] = a; _bias[1] = RDNA4R.dbClamp ? c : 0.0f; _bias[2] = b; if (c != 0.0f && !RDNA4R.dbClamp) RDNA4_ONCE("setDepthBias: a non-zero clamp is ignored (the device lacks depthBiasClamp)"); }   // bundle 10
- (void)setStencilReferenceValue:(uint32_t)v { _sref[0] = _sref[1] = v; }
- (void)setDepthStencilState:(id)s {   // bundle 10: recorded; read at every draw of a pipeline that has a depth/stencil attachment (a colour-only pipeline never looks at it)
    if (!s) {
        _dsi = rdna4_dsinfo_default();
        [self rdna4OwnResource:nil kind:RDNA4_RESOURCE_DEPTH_STENCIL
                      fragment:NO index:0];
        return;
    }
    if (![s isKindOfClass:[RDNA4DepthStencilState class]]) { RDNA4LOG("setDepthStencilState: not an RDNA4DepthStencilState"); return; }
    _dsi = [(RDNA4DepthStencilState *)s rdna4Info];
    [self rdna4OwnResource:s kind:RDNA4_RESOURCE_DEPTH_STENCIL
                  fragment:NO index:0];
}
// ---- gap census: barriers, fences, residency, store actions (QuartzCore sends -memoryBarrierWithScope:afterStages:beforeStages: 6369 times in 45 s) ----
// A barrier between draws of one encoder = end the Vulkan render pass (the next draw starts a LOADing continuation pass; the pass-split machinery already
// exists for layout changes) plus a full memory barrier, which orders everything before it against everything after it.
- (void)rdna4Barrier:(const char *)what {
    if (_hostWriteDropped) return;
    if (_ended) return;
    if (_inPass) { [self rdna4EndPass]; }
    rdna4_full_barrier([_cb vk]);
    RDNA4_ONCE("render encoder %s: render pass split + full barrier", what);
}
- (void)memoryBarrierWithScope:(NSUInteger)sc afterStages:(NSUInteger)a beforeStages:(NSUInteger)b { (void)sc; (void)a; (void)b; [self rdna4Barrier:"memoryBarrierWithScope:"]; }
- (void)memoryBarrierWithResources:(const id __unsafe_unretained *)r count:(NSUInteger)n afterStages:(NSUInteger)a beforeStages:(NSUInteger)b { (void)r; (void)n; (void)a; (void)b; [self rdna4Barrier:"memoryBarrierWithResources:"]; }
- (void)textureBarrier { [self rdna4Barrier:"textureBarrier"]; }
- (void)updateFence:(id)f afterStages:(NSUInteger)st {
    (void)st;   // a full-pass split is a conservative superset of every selected render stage
    if (_ended) { [_cb rdna4Fail:@"render updateFence after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    if (!_begun) [self rdna4BeginPass];   // a clear-only pass records its load before the checkpoint
    if ([_cb rdna4Error]) return;
    if (_inPass) [self rdna4EndPass];
    [_cb rdna4UpdateFence:f];
}
- (void)waitForFence:(id)f beforeStages:(NSUInteger)st {
    (void)st;
    if (_ended) { [_cb rdna4Fail:@"render waitForFence after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    if (_inPass) [self rdna4EndPass];
    [_cb rdna4WaitForFence:f];
}
- (void)setColorStoreAction:(NSUInteger)a atIndex:(NSUInteger)i { if (i < _na) _sact[i] = a; else RDNA4_ONCE("setColorStoreAction: index past the colour attachments; ignored"); }   // bundle 10: only a resolve changes anything (colour attachments are always stored)
- (void)setColorStoreActionOptions:(NSUInteger)a atIndex:(NSUInteger)i { (void)a; (void)i; }
- (void)setDepthStoreAction:(NSUInteger)a { if (_nd) _dsact = a; }
- (void)setDepthStoreActionOptions:(NSUInteger)a { (void)a; }
- (void)setStencilStoreAction:(NSUInteger)a { if (_nd) _ssact = a; }
- (void)setStencilStoreActionOptions:(NSUInteger)a { (void)a; }
- (void)setStencilFrontReferenceValue:(uint32_t)f backReferenceValue:(uint32_t)b { _sref[0] = f; _sref[1] = b; }
- (void)setLineWidth:(float)w { (void)w; }
RDNA4_ENCODER_SHARED
- (void)pushDebugGroup:(NSString *)s { (void)s; }
- (void)popDebugGroup {}
- (void)insertDebugSignpost:(NSString *)s { (void)s; }

- (RDNA4StageState *)rdna4Stage:(BOOL)frag { return frag ? &_sf : &_sv; }
- (void)rdna4SetBuffer:(id)buf offset:(NSUInteger)off index:(NSUInteger)i frag:(BOOL)f {
    if (i >= 31) { [_cb rdna4Fail:[NSString stringWithFormat:@"buffer index %lu >= 31", (unsigned long)i]]; return; }
    if (buf && ![buf isKindOfClass:[RDNA4Buffer class]]) { [_cb rdna4Fail:@"setBuffer: not an RDNA4Buffer"]; return; }
    RDNA4StageState *s = [self rdna4Stage:f]; s->buf[i].b = buf; s->buf[i].off = off;
    s->rt.as[i] = nil;
    [self rdna4OwnResource:nil kind:RDNA4_RESOURCE_ACCEL fragment:f index:i];
    [self rdna4OwnResource:buf kind:RDNA4_RESOURCE_BUFFER fragment:f index:i];
}
- (void)rdna4SetBytes:(const void *)p length:(NSUInteger)n index:(NSUInteger)i frag:(BOOL)f {
    if (i >= 31 || !p) { [_cb rdna4Fail:[NSString stringWithFormat:@"setBytes: index %lu / bytes %p", (unsigned long)i, p]]; return; }
    RDNA4Buffer *b = nil; NSUInteger off = 0;
    if ([_cb rdna4Bytes:p length:n buffer:&b offset:&off]) [self rdna4SetBuffer:b offset:off index:i frag:f];
}
- (void)rdna4SetTexture:(id)t index:(NSUInteger)i frag:(BOOL)f {
    if (i >= 128) { [_cb rdna4Fail:@"texture index >= 128"]; return; }
    if (t && ![t isKindOfClass:[RDNA4Texture class]]) { [_cb rdna4Fail:@"setTexture: not an RDNA4Texture"]; return; }
    [self rdna4Stage:f]->tex[i] = t;
    [self rdna4OwnResource:t kind:RDNA4_RESOURCE_TEXTURE fragment:f index:i];
}
- (void)rdna4SetSampler:(id)s lodMinClamp:(float)a lodMaxClamp:(float)b
                 override:(BOOL)override index:(NSUInteger)i frag:(BOOL)f {
    if ([_cb rdna4Error]) return;
    if (i >= 16) { [_cb rdna4Fail:@"sampler index >= 16"]; return; }
    if (s && ![s isKindOfClass:[RDNA4SamplerState class]]) { [_cb rdna4Fail:@"setSamplerState: not an RDNA4SamplerState"]; return; }
    RDNA4SamplerBinding *binding = nil;
    if (s) {
        NSError *error = nil;
        RDNA4SamplerState *sampler = (RDNA4SamplerState *)s;
        binding = override
            ? [sampler rdna4BindingWithLODMinClamp:a lodMaxClamp:b error:&error]
            : [sampler rdna4Binding:&error];
        if (!binding) {
            [_cb rdna4Fail:error.localizedDescription ?: @"sampler binding unavailable"];
            return;
        }
        [_cb rdna4RetainArgument:binding];
        if ([_cb rdna4Error]) return;
    }
    RDNA4StageState *state = [self rdna4Stage:f];
    RDNA4SamplerBinding *old = state->smp[i];
    state->smp[i] = binding;
    if (binding) {
        if (!_samplerBindings) _samplerBindings = [NSMutableSet set];
        [_samplerBindings addObject:binding];
    }
    if (old && old != binding) {
        BOOL used = NO;
        for (NSUInteger k = 0; k < 16 && !used; ++k)
            used = _sv.smp[k] == old || _sf.smp[k] == old;
        if (!used) [_samplerBindings removeObject:old];
    }
}
- (void)rdna4SetSampler:(id)s index:(NSUInteger)i frag:(BOOL)f {
    [self rdna4SetSampler:s lodMinClamp:0 lodMaxClamp:0 override:NO index:i frag:f];
}
- (void)setVertexBuffer:(id)b offset:(NSUInteger)o atIndex:(NSUInteger)i { [self rdna4SetBuffer:b offset:o index:i frag:NO]; }
- (void)setFragmentBuffer:(id)b offset:(NSUInteger)o atIndex:(NSUInteger)i { [self rdna4SetBuffer:b offset:o index:i frag:YES]; }
- (void)setVertexBufferOffset:(NSUInteger)o atIndex:(NSUInteger)i { if (i < 31) _sv.buf[i].off = o; }
- (void)setFragmentBufferOffset:(NSUInteger)o atIndex:(NSUInteger)i { if (i < 31) _sf.buf[i].off = o; }
- (void)setVertexBuffers:(const id __unsafe_unretained *)b offsets:(const NSUInteger *)o withRange:(NSRange)r { for (NSUInteger k = 0; k < r.length; k++) [self rdna4SetBuffer:b[k] offset:o[k] index:r.location + k frag:NO]; }
- (void)setFragmentBuffers:(const id __unsafe_unretained *)b offsets:(const NSUInteger *)o withRange:(NSRange)r { for (NSUInteger k = 0; k < r.length; k++) [self rdna4SetBuffer:b[k] offset:o[k] index:r.location + k frag:YES]; }
- (void)rdna4SetVisibleFunctionTable:(id<MTLVisibleFunctionTable>)t atBufferIndex:(NSUInteger)i frag:(BOOL)f {
    if ([_cb rdna4Error]) return;
    if (i >= 31) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"%s setVisibleFunctionTable: buffer index %lu >= 31",
                        f ? "fragment" : "vertex", (unsigned long)i]];
        return;
    }
    if (!t) { [self rdna4SetBuffer:nil offset:0 index:i frag:f]; return; }
    if (![t isKindOfClass:[RDNA4VisibleFunctionTable class]] || [t device] != [_cb device]) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"%s setVisibleFunctionTable: not an RDNA4VisibleFunctionTable for this device",
                        f ? "fragment" : "vertex"]];
        return;
    }
    RDNA4VisibleFunctionTable *vft = (RDNA4VisibleFunctionTable *)t;
    [vft rdna4RetainIntoCommandBuffer:_cb];
    [self rdna4SetBuffer:[vft rdna4BackingBuffer] offset:0 index:i frag:f];
}
- (void)setVertexVisibleFunctionTable:(id<MTLVisibleFunctionTable>)t atBufferIndex:(NSUInteger)i {
    [self rdna4SetVisibleFunctionTable:t atBufferIndex:i frag:NO];
}
- (void)setVertexVisibleFunctionTables:(const id<MTLVisibleFunctionTable> __unsafe_unretained *)t withBufferRange:(NSRange)r {
    if (r.length == 0) return;
    if (!t || r.location > 31 || r.length > 31 - r.location) {
        [_cb rdna4Fail:@"vertex setVisibleFunctionTables: invalid range or NULL array"];
        return;
    }
    for (NSUInteger k = 0; k < r.length; k++) [self rdna4SetVisibleFunctionTable:t[k] atBufferIndex:r.location + k frag:NO];
}
- (void)setFragmentVisibleFunctionTable:(id<MTLVisibleFunctionTable>)t atBufferIndex:(NSUInteger)i {
    [self rdna4SetVisibleFunctionTable:t atBufferIndex:i frag:YES];
}
- (void)setFragmentVisibleFunctionTables:(const id<MTLVisibleFunctionTable> __unsafe_unretained *)t withBufferRange:(NSRange)r {
    if (r.length == 0) return;
    if (!t || r.location > 31 || r.length > 31 - r.location) {
        [_cb rdna4Fail:@"fragment setVisibleFunctionTables: invalid range or NULL array"];
        return;
    }
    for (NSUInteger k = 0; k < r.length; k++) [self rdna4SetVisibleFunctionTable:t[k] atBufferIndex:r.location + k frag:YES];
}
- (void)rdna4SetAccelerationStructure:(id<MTLAccelerationStructure>)accel atBufferIndex:(NSUInteger)i frag:(BOOL)f {
    if ([_cb rdna4Error]) return;
    RDNA4StageState *s = [self rdna4Stage:f];
    if (!rdna4_rt_encoder_bind_accel(_cb, [_cb device], f ? "fragment" : "vertex", &s->rt, accel, i)) return;
    s->buf[i] = (RDNA4BufSlot){ nil, 0 };
    [self rdna4OwnResource:nil kind:RDNA4_RESOURCE_BUFFER fragment:f index:i];
    [self rdna4OwnResource:accel kind:RDNA4_RESOURCE_ACCEL fragment:f index:i];
}
- (void)setVertexAccelerationStructure:(id<MTLAccelerationStructure>)accel atBufferIndex:(NSUInteger)i {
    [self rdna4SetAccelerationStructure:accel atBufferIndex:i frag:NO];
}
- (void)setFragmentAccelerationStructure:(id<MTLAccelerationStructure>)accel atBufferIndex:(NSUInteger)i {
    [self rdna4SetAccelerationStructure:accel atBufferIndex:i frag:YES];
}
- (void)rdna4SetIntersectionFunctionTable:(id<MTLIntersectionFunctionTable>)t atBufferIndex:(NSUInteger)i frag:(BOOL)f {
    if ([_cb rdna4Error]) return;
    RDNA4StageState *s = [self rdna4Stage:f];
    RDNA4Buffer *backing = nil;
    if (!rdna4_rt_encoder_bind_ift(_cb, [_cb device], f ? "fragment" : "vertex", &s->rt, t, i, &backing)) return;
    [self rdna4SetBuffer:backing offset:0 index:i frag:f];
}
- (void)setVertexIntersectionFunctionTable:(id<MTLIntersectionFunctionTable>)t atBufferIndex:(NSUInteger)i {
    [self rdna4SetIntersectionFunctionTable:t atBufferIndex:i frag:NO];
}
- (void)setVertexIntersectionFunctionTables:(const id<MTLIntersectionFunctionTable> __unsafe_unretained *)t withBufferRange:(NSRange)r {
    if ([_cb rdna4Error]) return;
    (void)rdna4_rt_encoder_bind_ifts(_cb, [_cb device], "vertex", &_sv.rt, t, r,
                                     ^(RDNA4Buffer *buf, NSUInteger idx) {
                                         [self rdna4SetBuffer:buf offset:0 index:idx frag:NO];
                                     });
}
- (void)setFragmentIntersectionFunctionTable:(id<MTLIntersectionFunctionTable>)t atBufferIndex:(NSUInteger)i {
    [self rdna4SetIntersectionFunctionTable:t atBufferIndex:i frag:YES];
}
- (void)setFragmentIntersectionFunctionTables:(const id<MTLIntersectionFunctionTable> __unsafe_unretained *)t withBufferRange:(NSRange)r {
    if ([_cb rdna4Error]) return;
    (void)rdna4_rt_encoder_bind_ifts(_cb, [_cb device], "fragment", &_sf.rt, t, r,
                                     ^(RDNA4Buffer *buf, NSUInteger idx) {
                                         [self rdna4SetBuffer:buf offset:0 index:idx frag:YES];
                                     });
}
- (void)setVertexBytes:(const void *)p length:(NSUInteger)n atIndex:(NSUInteger)i { [self rdna4SetBytes:p length:n index:i frag:NO]; }
- (void)setFragmentBytes:(const void *)p length:(NSUInteger)n atIndex:(NSUInteger)i { [self rdna4SetBytes:p length:n index:i frag:YES]; }
- (void)setVertexTexture:(id)t atIndex:(NSUInteger)i { [self rdna4SetTexture:t index:i frag:NO]; }
- (void)setFragmentTexture:(id)t atIndex:(NSUInteger)i { [self rdna4SetTexture:t index:i frag:YES]; }
- (void)setVertexTextures:(const id __unsafe_unretained *)t withRange:(NSRange)r { for (NSUInteger k = 0; k < r.length; k++) [self rdna4SetTexture:t[k] index:r.location + k frag:NO]; }
- (void)setFragmentTextures:(const id __unsafe_unretained *)t withRange:(NSRange)r { for (NSUInteger k = 0; k < r.length; k++) [self rdna4SetTexture:t[k] index:r.location + k frag:YES]; }
- (void)setVertexSamplerState:(id)s atIndex:(NSUInteger)i { [self rdna4SetSampler:s index:i frag:NO]; }
- (void)setFragmentSamplerState:(id)s atIndex:(NSUInteger)i { [self rdna4SetSampler:s index:i frag:YES]; }
- (void)setVertexSamplerState:(id)s lodMinClamp:(float)a lodMaxClamp:(float)b atIndex:(NSUInteger)i { [self rdna4SetSampler:s lodMinClamp:a lodMaxClamp:b override:YES index:i frag:NO]; }
- (void)setFragmentSamplerState:(id)s lodMinClamp:(float)a lodMaxClamp:(float)b atIndex:(NSUInteger)i { [self rdna4SetSampler:s lodMinClamp:a lodMaxClamp:b override:YES index:i frag:YES]; }
- (void)setVertexSamplerStates:(const id __unsafe_unretained *)s withRange:(NSRange)r { for (NSUInteger k = 0; k < r.length; k++) [self rdna4SetSampler:s[k] index:r.location + k frag:NO]; }
- (void)setFragmentSamplerStates:(const id __unsafe_unretained *)s withRange:(NSRange)r { for (NSUInteger k = 0; k < r.length; k++) [self rdna4SetSampler:s[k] index:r.location + k frag:YES]; }
- (void)setVertexSamplerStates:(const id __unsafe_unretained *)s
                   lodMinClamps:(const float *)lo
                   lodMaxClamps:(const float *)hi
                      withRange:(NSRange)r {
    if (r.length && (!s || !lo || !hi)) {
        [_cb rdna4Fail:@"setVertexSamplerStates:lodMinClamps:lodMaxClamps: nil array"];
        return;
    }
    for (NSUInteger k = 0; k < r.length; k++)
        [self rdna4SetSampler:s[k] lodMinClamp:lo[k] lodMaxClamp:hi[k]
            override:YES index:r.location + k frag:NO];
}
- (void)setFragmentSamplerStates:(const id __unsafe_unretained *)s
                     lodMinClamps:(const float *)lo
                     lodMaxClamps:(const float *)hi
                        withRange:(NSRange)r {
    if (r.length && (!s || !lo || !hi)) {
        [_cb rdna4Fail:@"setFragmentSamplerStates:lodMinClamps:lodMaxClamps: nil array"];
        return;
    }
    for (NSUInteger k = 0; k < r.length; k++)
        [self rdna4SetSampler:s[k] lodMinClamp:lo[k] lodMaxClamp:hi[k]
            override:YES index:r.location + k frag:YES];
}

// ---- P5b helpers (only reached when the rdna4m-drawopt switch is ON) ----
- (void)rdna4NeedTex:(RDNA4Texture *)t layout:(VkImageLayout)l writable:(BOOL)writable {   // the body of needTex in rdna4PrepareDraw, verbatim
    if ([t rdna4IsIOS]) (void)rdna4_ios_touch(_cb, t, YES, writable, ^{   // 11h.6 / build 16 (P1): upload / flush outside the render pass (splits it if one is open)
        if (self->_inPass) { [self rdna4EndPass]; RDNA4LOGR("render pass split: IOSurface texture %p uploaded", (__bridge void *)t); } });
    [_cb rdna4CBRead:t];
    if ([t rdna4DispImgOnly]) (void)[_cb rdna4IOFirstTouch:t];   // P6: retained for the command buffer, nothing to upload
    if (([t rdna4IsIOS] || [t rdna4DispImgOnly]) && writable) { [_cb rdna4IOWritten:t]; [_cb rdna4DispNote:t bits:RDNA4DF_W_COMPUTE pso:nil]; }
    [self rdna4Need:t layout:l];
}
- (void)rdna4EachTex:(const RDNA4PB *)pb n:(uint32_t)n state:(const RDNA4StageState *)st dev:(id)dev {   // rdna4_each_tex, with the callback inlined
    for (uint32_t i = 0; i < n; i++) {
        if ((pb[i].type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || pb[i].type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER) && pb[i].binding >= 32 && pb[i].binding < 160) {
            for (uint32_t k = 0; k < pb[i].count; k++) { uint32_t ix = pb[i].binding - 32 + k;
                RDNA4Texture *t = pb[i].embedded ? (RDNA4Texture *)rdna4_embedded_resource(&pb[i], k, st, YES, dev) : (ix < 128 ? st->tex[ix] : nil);
                if (!t && !pb[i].embedded) t = rdna4_dummy_texture(dev); if (t) [self rdna4NeedTex:t layout:pb[i].gpuArguments ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL writable:NO]; }
        } else if ((pb[i].type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || pb[i].type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) && pb[i].binding >= 480 && pb[i].binding < 608) {
            for (uint32_t k = 0; k < pb[i].count; k++) {
                uint32_t ix = pb[i].binding - 480 + k;
                RDNA4Texture *t = pb[i].embedded ? (RDNA4Texture *)rdna4_embedded_resource(&pb[i], k, st, YES, dev) : (ix < 128 ? st->tex[ix] : nil);
                if (t) [self rdna4NeedTex:t layout:VK_IMAGE_LAYOUT_GENERAL writable:YES];
            }
        }
    }
}
// One stage (idx 0 = vertex set, 1 = fragment set): signature of exactly the slots rdna4FillSet reads for these bindings; equal to the last bound one = no new set, no vkCmdBindDescriptorSets.
// Decision (RADV, radv_cmd_buffer.c): radv_mark_descriptors_dirty() only does `descriptors_state->dirty |= descriptors_state->valid`: bound sets stay valid across vkCmdBindPipeline and are
// re-flushed by the next draw, so a skipped vkCmdBindDescriptorSets after a pipeline bind leaves RADV with valid state (and radv_bind_descriptor_sets itself skips an already-bound valid set).
- (BOOL)rdna4DcStage:(int)idx pb:(const RDNA4PB *)pb n:(uint32_t)n state:(const RDNA4StageState *)st dsl:(VkDescriptorSetLayout)dsl name:(const char *)nm cmd:(VkCommandBuffer)cmd {
    rdna4dc_sig sig; rdna4dc_sig_begin(&sig, _dc.layout, (uint64_t)(uintptr_t)dsl);
    for (uint32_t i = 0; i < n; i++) {
        const RDNA4PB *p = &pb[i];
        if (p->addressTable || p->samplerCoordinateFlags) { sig.over = 1; break; }
        switch (p->type) {
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            for (uint32_t k = 0; k < p->count; k++) { uint32_t ix = p->binding + k; RDNA4BufSlot sl = ix < 31 ? st->buf[ix] : (RDNA4BufSlot){ nil, 0 };
                rdna4dc_sig_push(&sig, sl.b ? (uint64_t)(uintptr_t)[sl.b vkBuffer] : 0); rdna4dc_sig_push(&sig, sl.b ? [sl.b rdna4VkOffset] + (uint64_t)sl.off : 0); }
            break;
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            for (uint32_t k = 0; k < p->count; k++) { uint32_t ix = p->binding - 32 + k;
                RDNA4Texture *t = p->embedded ? (RDNA4Texture *)rdna4_embedded_resource(p, k, st, YES, [_cb device]) : ((p->binding >= 32 && ix < 128) ? st->tex[ix] : nil);
                if (t) [_cb rdna4RetainEncoded:t];
                rdna4dc_sig_push(&sig, (uint64_t)(uintptr_t)(__bridge void *)t); rdna4dc_sig_push(&sig, t ? (uint64_t)(uintptr_t)[t vkView] : 0); }
            break;
        case VK_DESCRIPTOR_TYPE_SAMPLER:
            rdna4dc_sig_push(&sig, (uint64_t)(uintptr_t)p->stat);
            for (uint32_t k = 0; k < p->count; k++) { uint32_t ix = p->binding - (p->comparisonSampler ? 736 : 160) + k;
                id sm = p->embedded ? rdna4_embedded_resource(p, k, st, NO, [_cb device]) : (ix < 16 ? st->smp[ix] : nil);
                if (sm) [_cb rdna4RetainArgument:sm];
                VkSampler handle = VK_NULL_HANDLE;
                if ([sm isKindOfClass:[RDNA4SamplerBinding class]])
                    handle = [(RDNA4SamplerBinding *)sm vkSampler];
                else if ([sm isKindOfClass:[RDNA4SamplerState class]])
                    handle = [(RDNA4SamplerState *)sm vkSampler];
                rdna4dc_sig_push(&sig, (uint64_t)(uintptr_t)handle); }
            break;
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: {
            BOOL inp = p->type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
            for (uint32_t k = 0; k < p->count; k++) {
                uint32_t ix = p->binding - (inp ? 192 : 480) + k;
                RDNA4Texture *t = (!inp && p->embedded)
                    ? (RDNA4Texture *)rdna4_embedded_resource(p, k, st, YES, [_cb device])
                    : ((p->binding >= (inp ? 192u : 480u) && ix < (inp ? 8u : 128u)) ? (inp ? st->att[ix] : st->tex[ix]) : nil);
                rdna4dc_sig_push(&sig, (uint64_t)(uintptr_t)(__bridge void *)t); rdna4dc_sig_push(&sig, t ? (uint64_t)(uintptr_t)(inp ? [t rdna4AttViewLevel:0] : [t rdna4StorageView]) : 0); }
            break; }
        case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
            rdna4_rt_dc_push_accel_sig(&sig, p->binding, p->count, &st->rt);
            break;
        default: sig.over = 1; break;   // unsupported type: let rdna4FillSet refuse it as before
        }
        if (sig.over) break;
    }
    uint64_t set;
    if (rdna4dc_stage_match(&_dc.st[idx], &sig, &set)) { _dc.setsReused++; return YES; }   // same slots, same layout: the set bound at this index is already right
    VkDescriptorSet s = [_cb rdna4AllocSet:dsl]; if (!s || ![_cb rdna4FillSet:s bindings:pb count:n state:st stage:nm]) { rdna4dc_stage_reset(&_dc.st[idx]); return NO; }
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, [_pso layout], (uint32_t)idx, 1, &s, 0, NULL);
    rdna4dc_stage_store(&_dc.st[idx], &sig, (uint64_t)(uintptr_t)s); _dc.setsAllocated++;
    return YES;
}
// ---- draws ----
- (BOOL)rdna4PrepareDraw:(VkPrimitiveTopology)topo {
    if (_hostWriteDropped) return NO;
    if (_ended) { [_cb rdna4Fail:@"draw after endEncoding"]; return NO; }
    if ([_cb rdna4Error]) return NO;
    if ([_cb rdna4HasArgumentWrites]) {
        if (_inPass) [self rdna4EndPass];
        [_cb rdna4ArgumentCheckpoint];
        if ([_cb rdna4Error]) return NO;
    }
    if (!_pso) { [_cb rdna4Fail:@"draw without a render pipeline state"]; return NO; }
    { RDNA4RenderPipelineState *r = [_pso rdna4Real];   // hot-swap: switch to the real pipeline BETWEEN draws; every accessor below then reads one coherent object
      if (r) {
          _pso = r;
          _sv.scanPlan = [_pso scanPlanV];
          _sf.scanPlan = [_pso scanPlanF];
          _sv.rtAsRefl = [_pso rdna4RtAsReflV];
          _sf.rtAsRefl = [_pso rdna4RtAsReflF];
          [self rdna4OwnResource:r kind:RDNA4_RESOURCE_PIPELINE
                        fragment:NO index:0];
          RDNA4LOGR("HOT-SWAP: render encoder now uses the real pipeline %p", (__bridge void *)r);
      } }
    if ([_pso rdna4IsMeshPipeline]) {
        [_cb rdna4Fail:@"drawPrimitives/drawPatches called with a mesh render pipeline"];
        return NO;
    }
    if ([_pso rdna4IsTessellation] != (topo == VK_PRIMITIVE_TOPOLOGY_PATCH_LIST)) {
        [_cb rdna4Fail:[_pso rdna4IsTessellation]
            ? @"drawPrimitives called with a tessellation render pipeline"
            : @"drawPatches called with a non-tessellation render pipeline"];
        return NO;
    }
    MTLRenderPipelineReflection *reflection = [_pso reflection] ?: _roReflection;
    const BOOL hasFrag = [_pso rdna4HasFragment];
    BOOL roV = rdna4nc_stage_writes(&_sv, rdna4nc_stage_arguments(reflection, RDNA4NC_VERTEX), RDNA4NC_VERTEX, "vertex draw", &_roWarnV);
    BOOL roF = hasFrag ? rdna4nc_stage_writes(&_sf, rdna4nc_stage_arguments(reflection, RDNA4NC_FRAGMENT), RDNA4NC_FRAGMENT, "fragment draw", &_roWarnF) : NO;
    if (roV || roF) { [_cb rdna4Fail:@"render draw writes a read-only no-copy buffer"]; return NO; }
    [self rdna4SyncResourceSegment];
    [self rdna4SnapshotResources];
    VkCullModeFlags cull = _cull == MTLCullModeFront ? VK_CULL_MODE_FRONT_BIT : _cull == MTLCullModeBack ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    VkFrontFace front = _wind == MTLWindingCounterClockwise ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;   // both are window-space (y down) senses
    if ([_pso rdna4DSVk] != _dsVkFmt || [_pso rdna4SampleCount] != _encSamples) {   // bundle 10: the pipeline's attachments must be the pass's (Metal validation; a Vulkan render-pass compatibility rule)
        [_cb rdna4Fail:[NSString stringWithFormat:@"the render pipeline's depth/stencil format (vk %d) or sample count (%u) differs from the pass's (vk %d, %u)", (int)[_pso rdna4DSVk], [_pso rdna4SampleCount], (int)_dsVkFmt, _encSamples]]; return NO; }
    const char *ampWhy = NULL;
    if (!rdna4amp_draw_ok(_ampCount, [_pso rdna4MaxVertexAmplificationCount],
                          _ampHasMappings ? _ampMappings : NULL, _nvp, _layers, &ampWhy)) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"render draw vertex amplification invalid: %s", ampWhy ?: "unsupported"]];
        return NO;
    }
    uint32_t dk = [_pso rdna4DSKey:&_dsi];   // bundle 10: 0 for a colour-only pipeline
    uint32_t vpOff = _ampHasMappings ? _ampMappings[0].viewport_offset : 0u;
    uint32_t vpCount = rdna4amp_viewport_count(_nvp, _nsr, vpOff, RDNA4R.maxViewports ?: 1u);
    uint32_t rflags = ((_fillMode == MTLTriangleFillModeLines) ? 1u : 0u) |
                      ((_depthClipMode == MTLDepthClipModeClamp) ? 2u : 0u) |
                      (_hasSamplePos ? 4u : 0u) | ((vpCount - 1u) << 3);
    uint32_t ckey = (uint32_t)cull | (rflags << 4);
    uint32_t fkey = (uint32_t)front | (dk << 1);   // the last-answer cache compares (topology, cull+rflags, front+ds, pso)
    VkPipeline vkp;
    if (_dcOn) {   // P5b item 3: the last (topology, cull, front, pso) -> VkPipeline answer skips pipelineForTopology's NSNumber / NSLock / NSDictionary
        uint64_t pp;
        if (rdna4dc_enc_pc_get(&_dc, (uint32_t)topo, ckey, fkey, (__bridge const void *)_pso, &pp)) vkp = (VkPipeline)(uintptr_t)pp;
        else { vkp = [_pso pipelineForTopology:topo cull:cull front:front ds:dk rflags:rflags]; rdna4dc_enc_pc_put(&_dc, (uint32_t)topo, ckey, fkey, (__bridge const void *)_pso, (uint64_t)(uintptr_t)vkp); }
    } else vkp = [_pso pipelineForTopology:topo cull:cull front:front ds:dk rflags:rflags];
    if (!vkp) { [_cb rdna4Fail:@"cannot build a pipeline variant for this draw"]; return NO; }
    const uint32_t *need = [_pso needBindings]; uint32_t nn = [_pso nNeed];
    for (uint32_t i = 0; i < nn; i++) if (need[i] != 31 && !_sv.buf[need[i]].b) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"vertex buffer at index %u is read by the vertex descriptor but never bound", need[i]]]; return NO; }
    id dev = [_cb device];
    void (^needTex)(RDNA4Texture *, VkImageLayout, BOOL) = ^(RDNA4Texture *t, VkImageLayout l, BOOL writable) {
        if ([t rdna4IsIOS]) (void)rdna4_ios_touch(self->_cb, t, YES, writable, ^{   // 11h.6 / build 16 (P1): upload / flush outside the render pass (splits it if one is open)
            if (self->_inPass) { [self rdna4EndPass]; RDNA4LOGR("render pass split: IOSurface texture %p uploaded", (__bridge void *)t); } });
        [self->_cb rdna4CBRead:t];   // Stage 0b: sampled (or storage-accessed) by a render draw
        if ([t rdna4DispImgOnly]) (void)[self->_cb rdna4IOFirstTouch:t];   // P6: retained for the command buffer, nothing to upload
        if (([t rdna4IsIOS] || [t rdna4DispImgOnly]) && writable) { [self->_cb rdna4IOWritten:t]; [self->_cb rdna4DispNote:t bits:RDNA4DF_W_COMPUTE pso:nil]; }   // a storage-image write from a render pass
        [self rdna4Need:t layout:l];
    };
    if (_dcOn) {   // P5b item 5: the same visit as rdna4_each_tex, as a plain loop (no heap-copied block)
        [self rdna4EachTex:[_pso pbV] n:[_pso npbV] state:&_sv dev:dev];
        if (hasFrag) [self rdna4EachTex:[_pso pbF] n:[_pso npbF] state:&_sf dev:dev];
    } else {
    rdna4_each_tex([_pso pbV], [_pso npbV], &_sv, dev, needTex);
    if (hasFrag) rdna4_each_tex([_pso pbF], [_pso npbF], &_sf, dev, needTex);
    }
    NSMutableSet *argumentResources = [_argumentResources mutableCopy] ?: [NSMutableSet set];
    NSMutableSet *argumentWritable = [_argumentWritable mutableCopy] ?: [NSMutableSet set];
    rdna4_argument_stage_resources(argumentResources, argumentWritable, [_pso pbV], [_pso npbV], &_sv, rdna4nc_stage_arguments(reflection, RDNA4NC_VERTEX));
    if (hasFrag) rdna4_argument_stage_resources(argumentResources, argumentWritable, [_pso pbF], [_pso npbF], &_sf, rdna4nc_stage_arguments(reflection, RDNA4NC_FRAGMENT));
    for (uint32_t i = 0; i < 31; i++) {
        if (_sv.buf[i].b) rdna4_rt_retain_argbuf_resources(_sv.buf[i].b, _cb);
        if (hasFrag && _sf.buf[i].b) rdna4_rt_retain_argbuf_resources(_sf.buf[i].b, _cb);
    }
    for (id res in argumentResources) {
        if ([res isKindOfClass:[RDNA4Buffer class]])
            rdna4_rt_retain_argbuf_resources((RDNA4Buffer *)res, _cb);
    }
    RDNA4ArgumentScanPrepared scan = {0};
    const RDNA4StageState *scanStages[] = {&_sv, &_sf};
    NSString *scanError = nil;
    if (!rdna4_argument_scan_prepare(&scan, scanStages, hasFrag ? 2 : 1, argumentWritable, dev,
                                     &scanError)) {
        [_cb rdna4Fail:scanError ?: @"argument scan preparation failed"];
        return NO;
    }
    BOOL argumentsReady = [_cb rdna4PrepareArguments:[_pso argumentTables]
        resources:argumentResources writable:argumentWritable scan:&scan
        textures:needTex];
    rdna4_argument_scan_prepared_destroy(&scan);
    if (!argumentsReady) return NO;
    if ([_pso fetch] != _fetch) {   // 11e-2: framebuffer-fetch draws need the input-attachment render-pass shape; a change of shape is a pass split (LOAD continuation)
        if ([_pso fetch] && [_pso fetchMax] >= _na) { [_cb rdna4Fail:[NSString stringWithFormat:@"pipeline reads [[color(%u)]] but the encoder has %u colour attachment(s)", [_pso fetchMax], _na]]; return NO; }
        if (_inPass) { [self rdna4EndPass]; RDNA4LOGR("render pass split: framebuffer fetch %s", [_pso fetch] ? "on" : "off"); }
        _fetch = [_pso fetch];
    }
    if (_ampLayerOff && !_begun) {
        // Ensure initial Clear/Load runs across all pass layers before starting a layer-shifted continuation pass.
        uint32_t savedLayerOff = _ampLayerOff;
        _ampLayerOff = 0u;
        [self rdna4BeginPass];
        if ([_cb rdna4Error]) { _ampLayerOff = savedLayerOff; return NO; }
        [self rdna4EndPass];
        _ampLayerOff = savedLayerOff;
    }
    if (_inPass && _activeLayerOff != _ampLayerOff) [self rdna4EndPass];
    [self rdna4BeginPass];
    if ([_cb rdna4Error]) return NO;
    [self rdna4OccDraw];   // build 18 (P2): the first draw after a visibility mode / offset change (or pass split) begins its query interval
    const BOOL fallback = [_pso rdna4IsRenderFallback];
    if (fallback) [_cb rdna4NoteShaderFallback];
    {   // native #12 P1/D1: this draw writes a display surface; GPUPass = CoreDisplay's final display pass, ColorFill = a fill, anything else a SkyLight composite
        BOOL anyd = NO; for (uint32_t i = 0; i < _na; i++) if ([_tex[i] rdna4IsDisp]) anyd = YES;
        if (anyd) {
            RDNA4Texture *src = nil;   // the first IOSurface texture this draw samples in the fragment stage
            const RDNA4PB *pbf = [_pso pbF]; uint32_t npf = [_pso npbF];
            for (uint32_t i = 0; i < npf && !src; i++) if (pbf[i].type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE && pbf[i].binding >= 32 && pbf[i].binding < 160)
                for (uint32_t k = 0; k < pbf[i].count && !src; k++) { uint32_t ix = pbf[i].binding - 32 + k; RDNA4Texture *tt = ix < 128 ? _sf.tex[ix] : nil; if (tt && [tt rdna4IsIOS]) src = tt; }
            uint32_t bits = fallback ? RDNA4DF_W_SHADER_FALLBACK : rdna4df_draw_bits([_pso rdna4FName]);
            rdna4df_rect_t rc = rdna4df_draw_rect(_vp.originX, _vp.originY, _vp.width, _vp.height, (int64_t)_sr.x, (int64_t)_sr.y, (int64_t)_sr.width, (int64_t)_sr.height, _w, _h);
            for (uint32_t i = 0; i < _na; i++) if ([_tex[i] rdna4IsDisp]) {
                BOOL clr = (_clrm >> i) & 1u; if (clr && (bits & (RDNA4DF_W_DRAW_FINAL | RDNA4DF_W_DRAW_COMP))) _clrm &= ~(1u << i); else clr = NO;
                [_cb rdna4DispNote:_tex[i] bits:bits pso:_pso];
                [_cb rdna4DispDraw:_tex[i] bits:bits rect:rc w:_w h:_h clear:clr src:src vp:_vp sc:_sr];
            }
            static _Atomic int vlog;   // D1 sample of the first 30 display draws: viewport, scissor, vertex count, and the first 32 bytes of the first bound vertex-stage buffer (the extents may live in vertex data)
            if (atomic_load(&vlog) < 30 && atomic_fetch_add(&vlog, 1) < 30) {
                const uint8_t *vb = NULL; uint32_t vbi = 0;
                for (uint32_t i = 0; i < 31 && !vb; i++) if (_sv.buf[i].b) { const uint8_t *c = (const uint8_t *)[_sv.buf[i].b contents]; if (c) { vb = c + _sv.buf[i].off; vbi = i; } }
                char hx[200]; hx[0] = 0; if (vb) for (int k = 0; k < 8; k++) { float f; uint32_t u; memcpy(&u, vb + 4 * k, 4); memcpy(&f, &u, 4); size_t l = strlen(hx); snprintf(hx + l, sizeof hx - l, "%s%08x(%g)", k ? " " : "", u, (double)f); }
                RDNA4LOG("scanout: DRAW sample #%d: %s/%s count %lu viewport %.1f,%.1f %.1fx%.1f scissor %lu,%lu %lux%lu -> rect [%d,%d)-[%d,%d); source texture: %s id %u %lux%lu; vertex buffer %u words: %s",
                    atomic_load(&vlog), [_pso rdna4VName], [_pso rdna4FName], (unsigned long)_dvc, _vp.originX, _vp.originY, _vp.width, _vp.height, (unsigned long)_sr.x, (unsigned long)_sr.y, (unsigned long)_sr.width, (unsigned long)_sr.height,
                    rc.x0, rc.y0, rc.x1, rc.y1, src ? ([src rdna4IsDisp] ? "display surface" : "IOSurface") : "none", src && [src rdna4IOSRef] ? IOSurfaceGetID([src rdna4IOSRef]) : 0, src ? (unsigned long)src.width : 0UL, src ? (unsigned long)src.height : 0UL, vbi, vb ? hx : "(none or private)");
            }
        }
    }
    VkCommandBuffer cmd = [_cb vk];
    if (_fetch) {   // earlier draws' colour writes must be visible to this draw's input-attachment reads (self-dependency of the pass)
        VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_DEPENDENCY_BY_REGION_BIT, 1, &mb, 0, NULL, 0, NULL);
    }
    if (_dcOn) {   // P5b item 2: RADV's radv_CmdBindPipeline re-marks descriptors dirty and re-binds dynamic state even for the same pipeline; viewport / scissor / blend are set below every draw anyway
        _dc.draws++;
        if (!rdna4dc_enc_pipe_same(&_dc, (uint64_t)(uintptr_t)vkp)) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkp);
    } else
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkp);
    VkViewport vps[16]; VkRect2D scs[16];
    for (uint32_t i = 0; i < vpCount; i++) {
        uint32_t vi = i + vpOff, si = i + vpOff;
        MTLViewport mv = vi < _nvp ? _vps[vi] : _vps[_nvp - 1];
        MTLScissorRect ms = si < _nsr ? _srs[si] : (MTLScissorRect){ 0, 0, _w, _h };
        vps[i] = (VkViewport){ (float)mv.originX, (float)(mv.originY + mv.height), (float)mv.width, -(float)mv.height, (float)mv.znear, (float)mv.zfar };
        scs[i] = (VkRect2D){ { (int32_t)ms.x, (int32_t)ms.y }, { (uint32_t)ms.width, (uint32_t)ms.height } };
    }
    vkCmdSetViewport(cmd, 0, vpCount, vps); vkCmdSetScissor(cmd, 0, vpCount, scs); vkCmdSetBlendConstants(cmd, _blend);
    if (_hasSamplePos && rdna4_vkCmdSetSampleLocationsEXT) {
        VkSampleLocationEXT vkSL[8];
        for (uint32_t s = 0; s < _samplePosCount && s < 8; s++) vkSL[s] = (VkSampleLocationEXT){ _samplePos[s].x, _samplePos[s].y };
        VkSampleLocationsInfoEXT slInfo = { .sType = VK_STRUCTURE_TYPE_SAMPLE_LOCATIONS_INFO_EXT,
            .sampleLocationsPerPixel = (VkSampleCountFlagBits)_encSamples,
            .sampleLocationGridSize = { 1, 1 },
            .sampleLocationsCount = _samplePosCount, .pSampleLocations = vkSL };
        rdna4_vkCmdSetSampleLocationsEXT(cmd, &slInfo);
    }
    if ([_pso rdna4HasDS]) {   // bundle 10: the dynamic depth/stencil state (masks and reference per face; the bias when the pipeline has depth)
        vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_BIT, _dsi.fRead); vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_BACK_BIT, _dsi.bRead);
        vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_BIT, _dsi.fWrite); vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_BACK_BIT, _dsi.bWrite);
        vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_BIT, _sref[0]); vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_BACK_BIT, _sref[1]);
        if ([_pso rdna4HasDepth]) vkCmdSetDepthBias(cmd, _bias[0], _bias[1], _bias[2]);
    }
    for (uint32_t i = 0; i < nn; i++) {
        RDNA4Buffer *b; NSUInteger off;
        if (need[i] == 31) { b = rdna4_dummy_buffer(dev); off = 0; if (!b) { [_cb rdna4Fail:@"no dummy buffer"]; return NO; } [_cb rdna4RetainEncoded:b]; }
        else { b = _sv.buf[need[i]].b; off = _sv.buf[need[i]].off; }
        uint64_t vo, viewSize;
        if ([b device] != dev || !rdna4_buffer_view(b.length, off, [b rdna4VkOffset], 1, UINT32_MAX, &vo, &viewSize)) {
            [_cb rdna4Fail:@"vertex buffer window is outside the logical resource"]; return NO;
        }
        VkBuffer vb = [b vkBuffer];
        vkCmdBindVertexBuffers(cmd, need[i], 1, &vb, &vo);
    }
    [_cb rdna4BindArguments:[_pso argumentTables] point:VK_PIPELINE_BIND_POINT_GRAPHICS layout:[_pso layout]];
    if (_dcOn && ![_pso argumentTables]->active) {   // GPU tables get a fresh stage set for each immutable snapshot.
        rdna4dc_enc_layout(&_dc, (uint64_t)(uintptr_t)[_pso layout]);
        if ([_pso npbV] && ![self rdna4DcStage:0 pb:[_pso pbV] n:[_pso npbV] state:&_sv dsl:[_pso dsl:0] name:"vertex" cmd:cmd]) return NO;
        if ([_pso npbF] && ![self rdna4DcStage:1 pb:[_pso pbF] n:[_pso npbF] state:&_sf dsl:[_pso dsl:1] name:"fragment" cmd:cmd]) return NO;
        return YES;
    }
    if ([_pso npbV]) {
        VkDescriptorSet s = [_cb rdna4AllocSet:[_pso dsl:0]]; if (!s || ![_cb rdna4FillSet:s bindings:[_pso pbV] count:[_pso npbV] state:&_sv stage:"vertex"]) return NO;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, [_pso layout], 0, 1, &s, 0, NULL);
    }
    if ([_pso npbF]) {
        VkDescriptorSet s = [_cb rdna4AllocSet:[_pso dsl:1]]; if (!s || ![_cb rdna4FillSet:s bindings:[_pso pbF] count:[_pso npbF] state:&_sf stage:"fragment"]) return NO;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, [_pso layout], 1, 1, &s, 0, NULL);
    }
    return YES;
}
- (void)drawPrimitives:(MTLPrimitiveType)t vertexStart:(NSUInteger)s vertexCount:(NSUInteger)c instanceCount:(NSUInteger)ic baseInstance:(NSUInteger)bi {
    if (_hostWriteDropped) return;
    _dvc = c * (ic ? ic : 1);
    if (!c || !ic) return;
    if (c > UINT32_MAX || ic > UINT32_MAX || s > UINT32_MAX || bi > UINT32_MAX || c - 1 > UINT32_MAX - s || ic - 1 > UINT32_MAX - bi) {
        [_cb rdna4Fail:@"draw vertex or instance range exceeds Vulkan's 32-bit limits"]; return;
    }
    if ([[_pso rdna4Real] fetch] || [_pso fetch]) {
        if (!RDNA4R.orderedColorAccess) {
            NSUInteger size = t == MTLPrimitiveTypePoint ? 1 : (t == MTLPrimitiveTypeLine || t == MTLPrimitiveTypeLineStrip) ? 2 : 3;
            NSUInteger step = t == MTLPrimitiveTypeLineStrip || t == MTLPrimitiveTypeTriangleStrip ? 1 : size;
            if (c < size) return;
            MTLWinding winding = _wind;
            for (NSUInteger instance = 0; instance < ic; instance++) {
                for (NSUInteger first = 0; first <= c - size; first += step) {
                    // A separate strip draw restarts parity; preserve front-facing and stencil semantics.
                    _wind = t == MTLPrimitiveTypeTriangleStrip && (first & 1) ? (winding == MTLWindingClockwise ? MTLWindingCounterClockwise : MTLWindingClockwise) : winding;
                    BOOL ready = [self rdna4PrepareDraw:rdna4_topo(t)];
                    _wind = winding;
                    if (!ready) return;
                    vkCmdDraw([_cb vk], (uint32_t)size, 1, (uint32_t)(s + first), (uint32_t)(bi + instance));
                }
            }
            return;
        }
    }
    if ([self rdna4PrepareDraw:rdna4_topo(t)]) vkCmdDraw([_cb vk], (uint32_t)c, (uint32_t)ic, (uint32_t)s, (uint32_t)bi);
}
- (void)drawPrimitives:(MTLPrimitiveType)t vertexStart:(NSUInteger)s vertexCount:(NSUInteger)c { [self drawPrimitives:t vertexStart:s vertexCount:c instanceCount:1 baseInstance:0]; }
- (void)drawPrimitives:(MTLPrimitiveType)t vertexStart:(NSUInteger)s vertexCount:(NSUInteger)c instanceCount:(NSUInteger)ic { [self drawPrimitives:t vertexStart:s vertexCount:c instanceCount:ic baseInstance:0]; }
- (BOOL)rdna4IndirectBuffer:(id)buffer offset:(NSUInteger)off bytes:(NSUInteger)bytes {
    if (_hostWriteDropped) return NO;
    if (_ended) { [_cb rdna4Fail:@"indirect draw after endEncoding"]; return NO; }
    if ([_cb rdna4Error]) return NO;
    if (![buffer isKindOfClass:[RDNA4Buffer class]] || (off & 3) || off > [buffer length] || bytes > [buffer length] - off) {
        [_cb rdna4Fail:@"indirect argument buffer, alignment or range is invalid"]; return NO;
    }
    if (!rdna4nc_vk_offset_ok([(RDNA4Buffer *)buffer rdna4VkOffset] + off, 4, RDNA4NC_ALIGN_INDIRECT)) {
        [_cb rdna4Fail:@"indirect draw Vulkan offset must be a multiple of 4"]; return NO;
    }
    if (!vkCmdDrawIndirect || !vkCmdDrawIndexedIndirect || !RDNA4R.indirectFirstInstance) {
        [_cb rdna4Fail:@"backend lacks indirect drawing with nonzero base instances"]; return NO;
    }
    if (!RDNA4R.orderedColorAccess && ([[_pso rdna4Real] fetch] || [_pso fetch])) {
        [_cb rdna4Fail:@"indirect framebuffer fetch requires rasterization-order attachment access"]; return NO;
    }
    // A shader in an earlier draw may have generated these arguments.
    if (_inPass) [self rdna4EndPass];
    rdna4_full_barrier([_cb vk]);
    _dvc = 0;
    return YES;
}
- (void)drawPrimitives:(MTLPrimitiveType)t indirectBuffer:(id)buffer indirectBufferOffset:(NSUInteger)off {
    if (![self rdna4IndirectBuffer:buffer offset:off bytes:sizeof(VkDrawIndirectCommand)]) return;
    if ([self rdna4PrepareDraw:rdna4_topo(t)]) {
        [_cb rdna4RetainEncoded:buffer];
        vkCmdDrawIndirect([_cb vk], [(RDNA4Buffer *)buffer vkBuffer], [(RDNA4Buffer *)buffer rdna4VkOffset] + off, 1, sizeof(VkDrawIndirectCommand));
    }
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)t indexType:(MTLIndexType)it indexBuffer:(id)ib indexBufferOffset:(NSUInteger)ioff
              indirectBuffer:(id)buffer indirectBufferOffset:(NSUInteger)off {
    if (_hostWriteDropped) return;
    if (![ib isKindOfClass:[RDNA4Buffer class]] || (it != MTLIndexTypeUInt16 && it != MTLIndexTypeUInt32)) {
        [_cb rdna4Fail:@"indirect index buffer or index type is invalid"]; return;
    }
    NSUInteger size = it == MTLIndexTypeUInt32 ? 4 : 2;
    if (ioff % size || ioff >= [ib length]) { [_cb rdna4Fail:@"indirect index buffer offset is invalid"]; return; }
    if (!rdna4nc_vk_offset_ok([(RDNA4Buffer *)ib rdna4VkOffset] + ioff, size, RDNA4NC_ALIGN_INDEX)) {
        [_cb rdna4Fail:@"index buffer Vulkan offset must be a multiple of the index size"]; return;
    }
    if (![self rdna4IndirectBuffer:buffer offset:off bytes:sizeof(VkDrawIndexedIndirectCommand)]) return;
    if (![self rdna4PrepareDraw:rdna4_topo(t)]) return;
    [_cb rdna4RetainEncoded:ib];
    [_cb rdna4RetainEncoded:buffer];
    vkCmdBindIndexBuffer([_cb vk], [(RDNA4Buffer *)ib vkBuffer], [(RDNA4Buffer *)ib rdna4VkOffset] + ioff, it == MTLIndexTypeUInt32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexedIndirect([_cb vk], [(RDNA4Buffer *)buffer vkBuffer], [(RDNA4Buffer *)buffer rdna4VkOffset] + off, 1, sizeof(VkDrawIndexedIndirectCommand));
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)t indexCount:(NSUInteger)c indexType:(MTLIndexType)it indexBuffer:(id)ib indexBufferOffset:(NSUInteger)off instanceCount:(NSUInteger)ic baseVertex:(NSInteger)bv baseInstance:(NSUInteger)bi {
    if (_hostWriteDropped) return;
    if (![ib isKindOfClass:[RDNA4Buffer class]]) { [_cb rdna4Fail:@"drawIndexedPrimitives: index buffer is not an RDNA4Buffer"]; return; }
    NSUInteger isz = it == MTLIndexTypeUInt32 ? 4 : 2;
    if (off % isz) { [_cb rdna4Fail:@"index buffer offset is not a multiple of the index size"]; return; }
    if (!rdna4nc_vk_offset_ok([(RDNA4Buffer *)ib rdna4VkOffset] + off, isz, RDNA4NC_ALIGN_INDEX)) {
        [_cb rdna4Fail:@"index buffer Vulkan offset must be a multiple of the index size"]; return;
    }
    if (off > [ib length] || c > ([ib length] - off) / isz) { [_cb rdna4Fail:@"indexed draw exceeds the index buffer"]; return; }
    _dvc = c * (ic ? ic : 1);
    if (!c || !ic) return;
    if (c > UINT32_MAX || ic > UINT32_MAX || bi > UINT32_MAX || ic - 1 > UINT32_MAX - bi || bv < INT32_MIN || bv > INT32_MAX) {
        [_cb rdna4Fail:@"indexed draw range exceeds Vulkan's 32-bit limits"]; return;
    }
    if (!RDNA4R.orderedColorAccess && ([[_pso rdna4Real] fetch] || [_pso fetch])) {
        if (t == MTLPrimitiveTypeLineStrip || t == MTLPrimitiveTypeTriangleStrip) {
            [_cb rdna4Fail:@"framebuffer fetch on indexed strips requires rasterization-order attachment access (restart parity cannot be inferred on the CPU)"]; return;
        }
        NSUInteger size = t == MTLPrimitiveTypePoint ? 1 : t == MTLPrimitiveTypeLine ? 2 : 3;
        if (c < size) return;
        for (NSUInteger instance = 0; instance < ic; instance++) {
            for (NSUInteger first = 0; first <= c - size; first += size) {
                if (![self rdna4PrepareDraw:rdna4_topo(t)]) return;
                [_cb rdna4RetainEncoded:ib];
                vkCmdBindIndexBuffer([_cb vk], [(RDNA4Buffer *)ib vkBuffer], [(RDNA4Buffer *)ib rdna4VkOffset] + off, it == MTLIndexTypeUInt32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
                vkCmdDrawIndexed([_cb vk], (uint32_t)size, 1, (uint32_t)first, (int32_t)bv, (uint32_t)(bi + instance));
            }
        }
        return;
    }
    if (![self rdna4PrepareDraw:rdna4_topo(t)]) return;
    [_cb rdna4RetainEncoded:ib];
    vkCmdBindIndexBuffer([_cb vk], [(RDNA4Buffer *)ib vkBuffer], [(RDNA4Buffer *)ib rdna4VkOffset] + off, it == MTLIndexTypeUInt32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexed([_cb vk], (uint32_t)c, (uint32_t)ic, 0, (int32_t)bv, (uint32_t)bi);
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)t indexCount:(NSUInteger)c indexType:(MTLIndexType)it indexBuffer:(id)ib indexBufferOffset:(NSUInteger)off instanceCount:(NSUInteger)ic {
    [self drawIndexedPrimitives:t indexCount:c indexType:it indexBuffer:ib indexBufferOffset:off instanceCount:ic baseVertex:0 baseInstance:0];
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)t indexCount:(NSUInteger)c indexType:(MTLIndexType)it indexBuffer:(id)ib indexBufferOffset:(NSUInteger)off {
    [self drawIndexedPrimitives:t indexCount:c indexType:it indexBuffer:ib indexBufferOffset:off instanceCount:1 baseVertex:0 baseInstance:0];
}
- (void)setTessellationFactorBuffer:(id)buffer offset:(NSUInteger)offset instanceStride:(NSUInteger)instanceStride {
    _tessFactorBuf = buffer; _tessFactorOff = offset; _tessFactorStride = instanceStride;
}
- (void)setTessellationFactorScale:(float)scale { _tessFactorScale = scale; }
- (BOOL)rdna4PreparePatchDraw:(NSUInteger)numCP indexed:(BOOL)indexed
                   patchIndex:(id)patchBuf patchIndexOffset:(NSUInteger)patchOff
                 controlIndex:(id)cpBuf controlIndexOffset:(NSUInteger)cpOff
                     indirect:(BOOL)indirect {
    if (_hostWriteDropped) return NO;
    if (_ended) { [_cb rdna4Fail:@"drawPatches after endEncoding"]; return NO; }
    if ([_cb rdna4Error]) return NO;
    if (!_pso) { [_cb rdna4Fail:@"drawPatches without a render pipeline state"]; return NO; }
    RDNA4RenderPipelineState *real = [_pso rdna4Real] ?: _pso;
    if (![real rdna4IsTessellation]) {
        [_cb rdna4Fail:@"drawPatches: called with a non-tessellation render pipeline"]; return NO;
    }
    if (numCP > RDNA4R.maxTessPatchSize) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"numberOfPatchControlPoints %lu exceeds %u", (unsigned long)numCP, RDNA4R.maxTessPatchSize]];
        return NO;
    }
    id dev = [_cb device];
    const rdna4_tess_cfg_t *cfg = [real tessCfg];
    if (![_tessFactorBuf isKindOfClass:[RDNA4Buffer class]] || [(RDNA4Buffer *)_tessFactorBuf device] != dev ||
        (_tessFactorOff & 3) || (_tessFactorStride & 3) || _tessFactorOff > [_tessFactorBuf length] ||
        ![(RDNA4Buffer *)_tessFactorBuf gpuAddress] || _tessFactorOff > UINT64_MAX - [(RDNA4Buffer *)_tessFactorBuf gpuAddress]) {
        [_cb rdna4Fail:@"tessellation factor buffer, offset or instance stride is invalid"]; return NO;
    }
    uint32_t fbytes = rdna4_tess_factor_stride(cfg);
    if ((!indirect || cfg->factor_step == RDNA4_TESS_FSTEP_CONST) && fbytes > [_tessFactorBuf length] - _tessFactorOff) {
        [_cb rdna4Fail:@"tessellation factor buffer range exceeds buffer length"]; return NO;
    }
    if (patchBuf && (![patchBuf isKindOfClass:[RDNA4Buffer class]] || [(RDNA4Buffer *)patchBuf device] != dev ||
                     (patchOff & 3) || patchOff > [patchBuf length] ||
                     ![(RDNA4Buffer *)patchBuf gpuAddress] || patchOff > UINT64_MAX - [(RDNA4Buffer *)patchBuf gpuAddress])) {
        [_cb rdna4Fail:@"tessellation patch index buffer or offset is invalid"]; return NO;
    }
    if (indexed) {
        if (cfg->cp_idx_type == RDNA4_TESS_CPIDX_NONE) {
            [_cb rdna4Fail:@"drawIndexedPatches: requires tessellationControlPointIndexType != None"]; return NO;
        }
        NSUInteger align = cfg->cp_idx_type == RDNA4_TESS_CPIDX_U32 ? 4u : 2u;
        if (![cpBuf isKindOfClass:[RDNA4Buffer class]] || [(RDNA4Buffer *)cpBuf device] != dev ||
            (cpOff & (align - 1u)) || cpOff > [cpBuf length] ||
            ![(RDNA4Buffer *)cpBuf gpuAddress] || cpOff > UINT64_MAX - [(RDNA4Buffer *)cpBuf gpuAddress]) {
            [_cb rdna4Fail:@"control point index buffer, alignment or offset is invalid"]; return NO;
        }
    }
    rdna4_tess_draw_params_t params = {0};
    params.factor_bda = [(RDNA4Buffer *)_tessFactorBuf gpuAddress] + _tessFactorOff;
    params.factor_size = [_tessFactorBuf length] - _tessFactorOff;
    params.factor_instance_stride = _tessFactorStride;
    if (patchBuf) {
        params.patch_idx_bda = [(RDNA4Buffer *)patchBuf gpuAddress] + patchOff;
        params.patch_idx_size = [patchBuf length] - patchOff;
    }
    if (indexed) {
        params.cp_idx_bda = [(RDNA4Buffer *)cpBuf gpuAddress] + cpOff;
        params.cp_idx_size = [cpBuf length] - cpOff;
    }
    params.factor_scale = _tessFactorScale;
    params.num_patch_cp = (uint32_t)numCP;
    RDNA4Buffer *retainVbs[RDNA4_TESS_MAX_VBUFS];
    uint32_t nRetainVbs = 0;
    for (uint32_t i = 0; i < cfg->num_inputs; i++) {
        const rdna4_tess_input_t *in = &cfg->inputs[i];
        if (!in->has_attr) continue;
        RDNA4Buffer *vb = _sv.buf[in->buf_idx].b;
        NSUInteger vo = _sv.buf[in->buf_idx].off;
        if (!vb || ![vb isKindOfClass:[RDNA4Buffer class]] || [vb device] != dev || vo > [vb length]) {
            [_cb rdna4Fail:[NSString stringWithFormat:@"tessellation vertex buffer at index %u is missing or out of range", in->buf_idx]];
            return NO;
        }
        uint64_t va = [vb gpuAddress];
        if (!va || vo > UINT64_MAX - va) {
            [_cb rdna4Fail:[NSString stringWithFormat:@"tessellation vertex buffer at index %u has invalid GPU address", in->buf_idx]];
            return NO;
        }
        if (!params.vb_bda[in->buf_idx]) {
            retainVbs[nRetainVbs++] = vb;
            params.vb_bda[in->buf_idx] = va + vo;
            params.vb_size[in->buf_idx] = [vb length] - vo;
        }
    }
    if (![self rdna4PrepareDraw:VK_PRIMITIVE_TOPOLOGY_PATCH_LIST]) return NO;
    for (uint32_t i = 0; i < nRetainVbs; i++) [_cb rdna4RetainEncoded:retainVbs[i]];
    [_cb rdna4RetainEncoded:_tessFactorBuf];
    if (patchBuf) [_cb rdna4RetainEncoded:patchBuf];
    if (indexed) [_cb rdna4RetainEncoded:cpBuf];
    RDNA4Buffer *pbuf = nil; NSUInteger poff = 0;
    if (![_cb rdna4Bytes:&params length:sizeof params buffer:&pbuf offset:&poff]) return NO;
    VkDescriptorSet ds = [_cb rdna4AllocSet:[_pso tessDsl]];
    if (!ds) return NO;
    VkDescriptorBufferInfo dbi = { [pbuf vkBuffer], [pbuf rdna4VkOffset] + poff, sizeof params };
    VkWriteDescriptorSet wds = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0,
        .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi };
    vkUpdateDescriptorSets(RDNA4R.dev, 1, &wds, 0, NULL);
    vkCmdBindDescriptorSets([_cb vk], VK_PIPELINE_BIND_POINT_GRAPHICS, [_pso layout], [_pso tessSet], 1, &ds, 0, NULL);
    return YES;
}
- (void)drawPatches:(NSUInteger)numberOfPatchControlPoints patchStart:(NSUInteger)patchStart patchCount:(NSUInteger)patchCount patchIndexBuffer:(id)patchIndexBuffer patchIndexBufferOffset:(NSUInteger)patchIndexBufferOffset instanceCount:(NSUInteger)instanceCount baseInstance:(NSUInteger)baseInstance {
    if (_hostWriteDropped) return;
    _dvc = patchCount * (instanceCount ? instanceCount : 1);
    if (!patchCount || !instanceCount) return;
    if (patchCount > UINT32_MAX || instanceCount > UINT32_MAX || patchStart > UINT32_MAX || baseInstance > UINT32_MAX ||
        patchCount - 1 > UINT32_MAX - patchStart || instanceCount - 1 > UINT32_MAX - baseInstance) {
        [_cb rdna4Fail:@"drawPatches range exceeds Vulkan's 32-bit limits"]; return;
    }
    if ([self rdna4PreparePatchDraw:numberOfPatchControlPoints indexed:NO patchIndex:patchIndexBuffer patchIndexOffset:patchIndexBufferOffset controlIndex:nil controlIndexOffset:0 indirect:NO]) {
        vkCmdDraw([_cb vk], (uint32_t)patchCount, (uint32_t)instanceCount, (uint32_t)patchStart, (uint32_t)baseInstance);
    }
}
- (void)drawPatches:(NSUInteger)numberOfPatchControlPoints patchIndexBuffer:(id)patchIndexBuffer patchIndexBufferOffset:(NSUInteger)patchIndexBufferOffset indirectBuffer:(id)indirectBuffer indirectBufferOffset:(NSUInteger)indirectBufferOffset {
    if (![self rdna4IndirectBuffer:indirectBuffer offset:indirectBufferOffset bytes:sizeof(VkDrawIndirectCommand)]) return;
    if ([self rdna4PreparePatchDraw:numberOfPatchControlPoints indexed:NO patchIndex:patchIndexBuffer patchIndexOffset:patchIndexBufferOffset controlIndex:nil controlIndexOffset:0 indirect:YES]) {
        [_cb rdna4RetainEncoded:indirectBuffer];
        vkCmdDrawIndirect([_cb vk], [(RDNA4Buffer *)indirectBuffer vkBuffer], [(RDNA4Buffer *)indirectBuffer rdna4VkOffset] + indirectBufferOffset, 1, sizeof(VkDrawIndirectCommand));
    }
}
- (void)drawIndexedPatches:(NSUInteger)numberOfPatchControlPoints patchStart:(NSUInteger)patchStart patchCount:(NSUInteger)patchCount patchIndexBuffer:(id)patchIndexBuffer patchIndexBufferOffset:(NSUInteger)patchIndexBufferOffset controlPointIndexBuffer:(id)controlPointIndexBuffer controlPointIndexBufferOffset:(NSUInteger)controlPointIndexBufferOffset instanceCount:(NSUInteger)instanceCount baseInstance:(NSUInteger)baseInstance {
    if (_hostWriteDropped) return;
    _dvc = patchCount * (instanceCount ? instanceCount : 1);
    if (!patchCount || !instanceCount) return;
    if (patchCount > UINT32_MAX || instanceCount > UINT32_MAX || patchStart > UINT32_MAX || baseInstance > UINT32_MAX ||
        patchCount - 1 > UINT32_MAX - patchStart || instanceCount - 1 > UINT32_MAX - baseInstance) {
        [_cb rdna4Fail:@"drawIndexedPatches range exceeds Vulkan's 32-bit limits"]; return;
    }
    if ([self rdna4PreparePatchDraw:numberOfPatchControlPoints indexed:YES patchIndex:patchIndexBuffer patchIndexOffset:patchIndexBufferOffset controlIndex:controlPointIndexBuffer controlIndexOffset:controlPointIndexBufferOffset indirect:NO]) {
        vkCmdDraw([_cb vk], (uint32_t)patchCount, (uint32_t)instanceCount, (uint32_t)patchStart, (uint32_t)baseInstance);
    }
}
- (void)drawIndexedPatches:(NSUInteger)numberOfPatchControlPoints patchIndexBuffer:(id)patchIndexBuffer patchIndexBufferOffset:(NSUInteger)patchIndexBufferOffset controlPointIndexBuffer:(id)controlPointIndexBuffer controlPointIndexBufferOffset:(NSUInteger)controlPointIndexBufferOffset indirectBuffer:(id)indirectBuffer indirectBufferOffset:(NSUInteger)indirectBufferOffset {
    if (![self rdna4IndirectBuffer:indirectBuffer offset:indirectBufferOffset bytes:sizeof(VkDrawIndirectCommand)]) return;
    if ([self rdna4PreparePatchDraw:numberOfPatchControlPoints indexed:YES patchIndex:patchIndexBuffer patchIndexOffset:patchIndexBufferOffset controlIndex:controlPointIndexBuffer controlIndexOffset:controlPointIndexBufferOffset indirect:YES]) {
        [_cb rdna4RetainEncoded:indirectBuffer];
        vkCmdDrawIndirect([_cb vk], [(RDNA4Buffer *)indirectBuffer vkBuffer], [(RDNA4Buffer *)indirectBuffer rdna4VkOffset] + indirectBufferOffset, 1, sizeof(VkDrawIndirectCommand));
    }
}
- (void)endEncoding {
    if (_ended) return;
    if (_hostWriteDropped) { _ended = YES; [super endEncoding]; return; }
    [self rdna4SyncResourceSegment];
    [self rdna4SnapshotResources];
    if (!_begun && ![_cb rdna4Error]) { _ampLayerOff = 0u; [self rdna4BeginPass]; }   // a clear-only encoder still has to run its load action across all layers
    _ended = YES;
    if (_dcOn) {   // P5b: fold this encoder's counters into the device totals
        atomic_fetch_add(&RDNA4DO.draws, _dc.draws); atomic_fetch_add(&RDNA4DO.setsReused, _dc.setsReused); atomic_fetch_add(&RDNA4DO.setsAlloc, _dc.setsAllocated);
        atomic_fetch_add(&RDNA4DO.pipesSkipped, _dc.pipesSkipped); atomic_fetch_add(&RDNA4DO.pipesBound, _dc.pipesBound); atomic_fetch_add(&RDNA4DO.pcHits, _dc.pcHits);
    }
    if (_inPass) { [self rdna4EndPass]; }
    [self rdna4ResolveAll];   // bundle 10: multisample resolve, outside the pass
    rdna4_full_barrier([_cb vk]);
    if (_nCsbEnd) {
        BOOL cbOk = ![_cb rdna4Error];
        for (uint32_t k = 0; k < _nCsbEnd; k++) {
            id<RDNA4CounterSampleBufferInternal> csb = _csbEnd[k];
            if (cbOk && csb && _csbEndV[k] != MTLCounterDontSample)
                [csb rdna4SampleOnCommandBuffer:_cb vkCommandBuffer:[_cb vk] sampleIndex:_csbEndV[k]
                                        barrier:NO stageFlags:VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT
                                            who:"render-pass-end-vertex"];
            if (cbOk && csb && _csbEndF[k] != MTLCounterDontSample)
                [csb rdna4SampleOnCommandBuffer:_cb vkCommandBuffer:[_cb vk] sampleIndex:_csbEndF[k]
                                        barrier:NO stageFlags:VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT
                                            who:"render-pass-end-fragment"];
            _csbEnd[k] = nil;
        }
        _nCsbEnd = 0u;
    }
    [_cb rdna4ArgumentCheckpoint];
    [self rdna4ReleaseRenderHandles];
    RDNA4LOGR("RDNA4RenderEncoder endEncoding (render pass ended)");
    [super endEncoding];
}
@end

