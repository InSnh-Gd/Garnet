// garnet_binary_archive.m - GarnetMetal modular compilation unit: MTLBinaryArchive and Vulkan pipeline caches.
#if !defined(GARNET_INTERNAL) && !defined(GARNET_METAL_MODULAR_BUILD)
#define GARNET_INTERNAL static
#endif
#ifdef GARNET_METAL_MODULAR_BUILD
#include "garnet_metal_internal.h"
#endif
#include "garnet_pipelines_internal.h"
#include "garnet_shaders_internal.h"

// ---------------------------------------------------------------------------------------------------------------
// RDNA4BinaryArchive (Task 3.1, Metal 3 MTLBinaryArchive):
// Versioned Garnet binary archive ('GRBA') persisting translated SPIR-V, reflection metadata JSON, and
// RADV VkPipelineCache blobs indexed by canonical SHA-256 + 64-bit pipeline descriptor keys.
//
// Wave 2B integration checklist (GarnetDevice.m + rdna4_render_pipeline_impl.inc):
//   1. GarnetDevice.m (right after #include "garnet_icb_abi.h", before #include "rdna4_render_pipeline_impl.m"):
//        #include "rdna4_binary_archive_impl.m"
//   2. GarnetDevice.m +load:
//        { const char *n[] = { "MTLBinaryArchive" }; rdna4_add_protocols([RDNA4BinaryArchive class], n, 1, "RDNA4BinaryArchive"); }
//   3. GarnetDevice.m -newBinaryArchiveWithDescriptor:error: (keep -supportsBinaryArchives NO until Task 8.2):
//        - (id<MTLBinaryArchive>)newBinaryArchiveWithDescriptor:(MTLBinaryArchiveDescriptor *)descriptor error:(NSError **)error {
//            return [[RDNA4BinaryArchive alloc] initWithDevice:self descriptor:descriptor error:error];
//        }
//   4. GarnetDevice.m compute descriptor methods (newComputePipelineStateWithDescriptor:...):
//        Forward `d` and `options` into [[RDNA4ComputePipelineState alloc] initWithDevice:self descriptor:d options:o error:&e]
//        (and return nil immediately without inproc/fallback when `o & MTLPipelineOptionFailOnBinaryArchiveMiss` fails).
//   5. rdna4_render_pipeline_impl.inc (render pipeline hook, ~8 lines):
//        Before rdna4_spv_lookup(), call:
//          BOOL baHit = NO; NSData *baVk = nil; rdna4ba_fingerprint_t baFp = {0}; NSError *baErr = nil;
//          if (!rdna4_ba_lookup_render(dev, d, options, &vsd, &mv_, &fsd, &mf_, &baVk, &baFp, &baHit, &baErr)) {
//              if (err) *err = baErr; return nil;
//          }
//        When baHit && baVk.length, wrap vkCreateGraphicsPipelines (line 862, using rdna4_ba_vkcache())
//        with rdna4_ba_vkcache_scope_begin(&baScope, baVk, &baFp) / rdna4_ba_vkcache_scope_end(&baScope).
// ---------------------------------------------------------------------------------------------------------------



// Forward declarations for pipeline helpers defined later in the same translation unit.




static NSError *rdna4_ba_err(MTLBinaryArchiveError code, NSString *msg, NSError *underlying) {
    NSMutableDictionary *info = [NSMutableDictionary dictionaryWithObject:(msg ?: @"Garnet binary archive error")
                                                                   forKey:NSLocalizedDescriptionKey];
    if (underlying) info[NSUnderlyingErrorKey] = underlying;
    return [NSError errorWithDomain:MTLBinaryArchiveDomain code:(NSInteger)code userInfo:info];
}

// ---- Vulkan pipeline-cache helpers (lazy proc lookup; no backend header edits needed) ----
static PFN_vkDestroyPipelineCache rdna4_ba_vkDestroyPipelineCache;

static void rdna4_ba_resolve_vk_procs(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        if (RDNA4R.ok && RDNA4R.dev && vkGetDeviceProcAddr)
            rdna4_ba_vkDestroyPipelineCache = (PFN_vkDestroyPipelineCache)vkGetDeviceProcAddr(RDNA4R.dev, "vkDestroyPipelineCache");
    });
}

static __thread VkPipelineCache rdna4_tl_ba_vkcache;

GARNET_INTERNAL VkPipelineCache rdna4_ba_vkcache(void) {
    return rdna4_tl_ba_vkcache ? rdna4_tl_ba_vkcache : RDNA4R.pc;
}


static BOOL rdna4_ba_device_fingerprint(rdna4ba_fingerprint_t *out, NSError **err) {
    memset(out, 0, sizeof *out);
    NSError *re = nil;
    if (!rdna4_radv_open(&re)) {
        if (err) *err = rdna4_ba_err(MTLBinaryArchiveErrorInvalidFile,
            @"Cannot open RADV backend for binary archive device fingerprint", re);
        return NO;
    }
    VkPhysicalDeviceProperties pp;
    memset(&pp, 0, sizeof pp);
    vkGetPhysicalDeviceProperties(RDNA4R.pd, &pp);
    out->garnet_abi_version = RDNA4BA_ABI_VERSION;
    out->translator_abi_version = RDNA4X_ABI_VERSION;
    out->vendor_id = pp.vendorID;
    out->device_id = pp.deviceID;
    out->driver_version = pp.driverVersion;
    out->flags = RDNA4R.gpuArgumentTables ? 1u : 0u;
    memcpy(out->pipeline_cache_uuid, pp.pipelineCacheUUID, sizeof out->pipeline_cache_uuid);

    uint8_t xlUuid[16] = {0};
    (void)rdna4x_file_uuid(rdna4x_xlib_path().fileSystemRepresentation, xlUuid);
    rdna4x_backend_capabilities qcaps = rdna4_rt_xlate_caps();
    rdna4ba_sha256_ctx sc;
    rdna4ba_sha256_init(&sc);
    rdna4ba_sha256_update(&sc, RDNA4X_KEY_TAG, sizeof RDNA4X_KEY_TAG);
    rdna4ba_sha256_update(&sc, xlUuid, sizeof xlUuid);
    rdna4ba_sha256_update_u32(&sc, RDNA4X_QUERY_POLICY_REVISION);
    rdna4ba_sha256_update_u32(&sc, qcaps.flags);
    rdna4ba_sha256_update_u32(&sc, qcaps.subgroup_size);
    rdna4ba_sha256_update_u32(&sc, qcaps.subgroup_supported_stages);
    rdna4ba_sha256_update_u32(&sc, qcaps.subgroup_supported_operations);
    rdna4ba_sha256_update_u32(&sc, out->flags);
    rdna4ba_sha256_final(&sc, out->translator_hash);
    return YES;
}

GARNET_INTERNAL void rdna4_ba_vkcache_scope_begin(rdna4_ba_vkcache_scope_t *sc, NSData *initialData,
                                         const rdna4ba_fingerprint_t *fp) {
    sc->owned = VK_NULL_HANDLE;
    sc->prev = rdna4_tl_ba_vkcache;
    if (!RDNA4R.ok || !vkCreatePipelineCache) return;
    rdna4_ba_resolve_vk_procs();
    if (!rdna4_ba_vkDestroyPipelineCache) return;

    const void *initPtr = NULL;
    size_t initLen = 0;
    if (initialData.length >= 32 && initialData.length <= RDNA4BA_MAX_ENTRY_PAYLOAD && fp &&
        rdna4pc_vk_header_ok(initialData.bytes, initialData.length,
                             fp->pipeline_cache_uuid, fp->vendor_id, fp->device_id)) {
        initPtr = initialData.bytes;
        initLen = initialData.length;
    }

    VkPipelineCacheCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
        .initialDataSize = initLen,
        .pInitialData = initPtr,
    };
    VkResult vr = vkCreatePipelineCache(RDNA4R.dev, &ci, NULL, &sc->owned);
    if (vr != VK_SUCCESS && initPtr) {
        ci.initialDataSize = 0;
        ci.pInitialData = NULL;
        vr = vkCreatePipelineCache(RDNA4R.dev, &ci, NULL, &sc->owned);
    }
    if (vr != VK_SUCCESS) {
        sc->owned = VK_NULL_HANDLE;
        return;
    }
    rdna4_tl_ba_vkcache = sc->owned;
}

static NSData *rdna4_ba_vkcache_scope_extract(const rdna4_ba_vkcache_scope_t *sc,
                                              const rdna4ba_fingerprint_t *fp) {
    if (!sc->owned || !RDNA4R.ok || !vkGetPipelineCacheData || !fp) return nil;
    size_t sz = 0;
    if (vkGetPipelineCacheData(RDNA4R.dev, sc->owned, &sz, NULL) != VK_SUCCESS ||
        sz < 32 || sz > RDNA4BA_MAX_ENTRY_PAYLOAD)
        return nil;
    NSMutableData *data = [NSMutableData dataWithLength:sz];
    if (!data) return nil;
    if (vkGetPipelineCacheData(RDNA4R.dev, sc->owned, &sz, data.mutableBytes) != VK_SUCCESS ||
        sz < 32 || sz > data.length ||
        !rdna4pc_vk_header_ok(data.bytes, sz, fp->pipeline_cache_uuid, fp->vendor_id, fp->device_id))
        return nil;
    if (sz < data.length) data.length = sz;
    return data;
}

GARNET_INTERNAL void rdna4_ba_vkcache_scope_end(rdna4_ba_vkcache_scope_t *sc) {
    rdna4_tl_ba_vkcache = sc->prev;
    if (sc->owned != VK_NULL_HANDLE && RDNA4R.ok && rdna4_ba_vkDestroyPipelineCache)
        rdna4_ba_vkDestroyPipelineCache(RDNA4R.dev, sc->owned, NULL);
    sc->owned = VK_NULL_HANDLE;
}

// ---- Canonical descriptor key builders ----
static void rdna4_ba_hash_str(rdna4ba_sha256_ctx *c, NSString *s) {
    const char *u = s.UTF8String ?: "";
    size_t n = strlen(u);
    rdna4ba_sha256_update_u64(c, (uint64_t)n);
    if (n) rdna4ba_sha256_update(c, u, n);
}

static void rdna4_ba_hash_stage_input(rdna4ba_sha256_ctx *c, MTLStageInputOutputDescriptor *sd) {
    rdna4ba_sha256_update_u32(c, sd ? 1u : 0u);
    if (!sd) return;
    rdna4ba_sha256_update_u64(c, (uint64_t)sd.indexType);
    rdna4ba_sha256_update_u64(c, (uint64_t)sd.indexBufferIndex);
    for (NSUInteger i = 0; i < 31; i++) {
        MTLAttributeDescriptor *a = sd.attributes[i];
        rdna4ba_sha256_update_u64(c, a ? (uint64_t)a.format : 0u);
        rdna4ba_sha256_update_u64(c, a ? (uint64_t)a.offset : 0u);
        rdna4ba_sha256_update_u64(c, a ? (uint64_t)a.bufferIndex : 0u);
    }
    for (NSUInteger i = 0; i < 31; i++) {
        MTLBufferLayoutDescriptor *l = sd.layouts[i];
        rdna4ba_sha256_update_u64(c, l ? (uint64_t)l.stride : 0u);
        rdna4ba_sha256_update_u64(c, l ? (uint64_t)l.stepFunction : 0u);
        rdna4ba_sha256_update_u64(c, l ? (uint64_t)l.stepRate : 0u);
    }
}

static void rdna4_ba_hash_vertex_desc(rdna4ba_sha256_ctx *c, MTLVertexDescriptor *vd) {
    rdna4ba_sha256_update_u32(c, vd ? 1u : 0u);
    if (!vd) return;
    for (NSUInteger i = 0; i < 31; i++) {
        MTLVertexAttributeDescriptor *a = vd.attributes[i];
        rdna4ba_sha256_update_u64(c, a ? (uint64_t)a.format : 0u);
        rdna4ba_sha256_update_u64(c, a ? (uint64_t)a.offset : 0u);
        rdna4ba_sha256_update_u64(c, a ? (uint64_t)a.bufferIndex : 0u);
    }
    for (NSUInteger i = 0; i < 31; i++) {
        MTLVertexBufferLayoutDescriptor *l = vd.layouts[i];
        rdna4ba_sha256_update_u64(c, l ? (uint64_t)l.stride : 0u);
        rdna4ba_sha256_update_u64(c, l ? (uint64_t)l.stepFunction : 0u);
        rdna4ba_sha256_update_u64(c, l ? (uint64_t)l.stepRate : 0u);
    }
}

static BOOL rdna4_ba_compute_key(MTLComputePipelineDescriptor *d, uint8_t keySha[32],
                                 uint64_t *keyU64, NSError **err) {
    if (!d || !d.computeFunction) {
        if (err) *err = rdna4_ba_err(MTLBinaryArchiveErrorUnexpectedElement,
            @"Compute pipeline descriptor has no computeFunction", nil);
        return NO;
    }
    NSString *fnSha = rdna4_fn_sha(d.computeFunction);
    if (!fnSha.length) {
        if (err) *err = rdna4_ba_err(MTLBinaryArchiveErrorCompilationFailure,
            @"Compute function has no AIR bitcodeData for binary archive key", nil);
        return NO;
    }
    static const char tag[] = "grba-compute-v1";
    rdna4ba_sha256_ctx c;
    rdna4ba_sha256_init(&c);
    rdna4ba_sha256_update(&c, tag, sizeof tag);
    rdna4_ba_hash_str(&c, fnSha);
    rdna4ba_sha256_update_u32(&c, d.threadGroupSizeIsMultipleOfThreadExecutionWidth ? 1u : 0u);
    rdna4ba_sha256_update_u64(&c, (uint64_t)d.maxTotalThreadsPerThreadgroup);
    BOOL icb = [d respondsToSelector:@selector(supportIndirectCommandBuffers)] && d.supportIndirectCommandBuffers;
    rdna4ba_sha256_update_u32(&c, icb ? 1u : 0u);
    NSUInteger stackDepth = [d respondsToSelector:@selector(maxCallStackDepth)] ? d.maxCallStackDepth : 0;
    rdna4ba_sha256_update_u64(&c, (uint64_t)stackDepth);
    rdna4_ba_hash_stage_input(&c, d.stageInputDescriptor);
    rdna4ba_sha256_final(&c, keySha);
    if (keyU64) *keyU64 = rdna4ba_key_u64_from_sha(keySha);
    return YES;
}

static BOOL rdna4_ba_render_key(MTLRenderPipelineDescriptor *d, uint8_t keySha[32],
                                uint64_t *keyU64, NSError **err) {
    if (!d || !d.vertexFunction) {
        if (err) *err = rdna4_ba_err(MTLBinaryArchiveErrorUnexpectedElement,
            @"Render pipeline descriptor has no vertexFunction", nil);
        return NO;
    }
    NSString *vSha = rdna4_fn_sha(d.vertexFunction);
    if (!vSha.length) {
        if (err) *err = rdna4_ba_err(MTLBinaryArchiveErrorCompilationFailure,
            @"Vertex function has no AIR bitcodeData for binary archive key", nil);
        return NO;
    }
    NSString *fSha = nil;
    if (d.fragmentFunction) {
        fSha = rdna4_fn_sha(d.fragmentFunction);
        if (!fSha.length) {
            if (err) *err = rdna4_ba_err(MTLBinaryArchiveErrorCompilationFailure,
                @"Fragment function has no AIR bitcodeData for binary archive key", nil);
            return NO;
        }
    }
    static const char tag[] = "grba-render-v1";
    rdna4ba_sha256_ctx c;
    rdna4ba_sha256_init(&c);
    rdna4ba_sha256_update(&c, tag, sizeof tag);
    rdna4_ba_hash_str(&c, vSha);
    rdna4ba_sha256_update_u32(&c, d.fragmentFunction ? 1u : 0u);
    rdna4_ba_hash_str(&c, fSha ?: @"");
    rdna4ba_sha256_update_u64(&c, (uint64_t)d.rasterSampleCount);
    rdna4ba_sha256_update_u32(&c, d.isAlphaToCoverageEnabled ? 1u : 0u);
    rdna4ba_sha256_update_u32(&c, d.isAlphaToOneEnabled ? 1u : 0u);
    rdna4ba_sha256_update_u32(&c, d.isRasterizationEnabled ? 1u : 0u);
    rdna4ba_sha256_update_u64(&c, (uint64_t)d.inputPrimitiveTopology);
    rdna4ba_sha256_update_u64(&c, (uint64_t)d.depthAttachmentPixelFormat);
    rdna4ba_sha256_update_u64(&c, (uint64_t)d.stencilAttachmentPixelFormat);
    rdna4ba_sha256_update_u32(&c, d.supportIndirectCommandBuffers ? 1u : 0u);
    rdna4ba_sha256_update_u64(&c, (uint64_t)d.maxVertexAmplificationCount);
    rdna4ba_sha256_update_u64(&c, (uint64_t)d.maxTessellationFactor);
    rdna4ba_sha256_update_u32(&c, d.isTessellationFactorScaleEnabled ? 1u : 0u);
    rdna4ba_sha256_update_u64(&c, (uint64_t)d.tessellationFactorFormat);
    rdna4ba_sha256_update_u64(&c, (uint64_t)d.tessellationControlPointIndexType);
    rdna4ba_sha256_update_u64(&c, (uint64_t)d.tessellationFactorStepFunction);
    rdna4ba_sha256_update_u64(&c, (uint64_t)d.tessellationOutputWindingOrder);
    rdna4ba_sha256_update_u64(&c, (uint64_t)d.tessellationPartitionMode);
    for (NSUInteger i = 0; i < 8; i++) {
        MTLRenderPipelineColorAttachmentDescriptor *ca = d.colorAttachments[i];
        rdna4ba_sha256_update_u64(&c, ca ? (uint64_t)ca.pixelFormat : 0u);
        rdna4ba_sha256_update_u64(&c, ca ? (uint64_t)ca.writeMask : 0u);
        rdna4ba_sha256_update_u32(&c, (ca && ca.isBlendingEnabled) ? 1u : 0u);
        rdna4ba_sha256_update_u64(&c, ca ? (uint64_t)ca.rgbBlendOperation : 0u);
        rdna4ba_sha256_update_u64(&c, ca ? (uint64_t)ca.alphaBlendOperation : 0u);
        rdna4ba_sha256_update_u64(&c, ca ? (uint64_t)ca.sourceRGBBlendFactor : 0u);
        rdna4ba_sha256_update_u64(&c, ca ? (uint64_t)ca.destinationRGBBlendFactor : 0u);
        rdna4ba_sha256_update_u64(&c, ca ? (uint64_t)ca.sourceAlphaBlendFactor : 0u);
        rdna4ba_sha256_update_u64(&c, ca ? (uint64_t)ca.destinationAlphaBlendFactor : 0u);
    }
    rdna4_ba_hash_vertex_desc(&c, d.vertexDescriptor);
    rdna4ba_sha256_final(&c, keySha);
    if (keyU64) *keyU64 = rdna4ba_key_u64_from_sha(keySha);
    return YES;
}

static BOOL rdna4_ba_function_key(id<MTLFunction> fn, MTLFunctionDescriptor *fd,
                                  uint8_t keySha[32], uint64_t *keyU64, NSError **err) {
    if (!fn) {
        if (err) *err = rdna4_ba_err(MTLBinaryArchiveErrorUnexpectedElement, @"Function is nil", nil);
        return NO;
    }
    NSString *fnSha = rdna4_fn_sha(fn);
    if (!fnSha.length) {
        if (err) *err = rdna4_ba_err(MTLBinaryArchiveErrorCompilationFailure,
            @"Function has no AIR bitcodeData for binary archive key", nil);
        return NO;
    }
    static const char tag[] = "grba-func-v1";
    rdna4ba_sha256_ctx c;
    rdna4ba_sha256_init(&c);
    rdna4ba_sha256_update(&c, tag, sizeof tag);
    rdna4_ba_hash_str(&c, fnSha);
    rdna4ba_sha256_update_u64(&c, (uint64_t)fn.functionType);
    NSString *nm = (fd && fd.specializedName.length) ? fd.specializedName :
                   ((fd && fd.name.length) ? fd.name : (fn.name ?: @""));
    rdna4_ba_hash_str(&c, nm);
    rdna4ba_sha256_update_u64(&c, fd ? (uint64_t)fd.options : 0u);
    rdna4ba_sha256_final(&c, keySha);
    if (keyU64) *keyU64 = rdna4ba_key_u64_from_sha(keySha);
    return YES;
}

// ---- Stage SPIR-V / reflection JSON helpers ----
static BOOL rdna4_ba_resolve_stage(id fn, const char *role, NSData **spvOut, NSDictionary **metaOut,
                                   NSString **fnameOut, NSError **err) {
    NSString *fname = nil;
    NSDictionary *meta = nil;
    NSError *e = nil, *xe = nil;
    NSData *spv = rdna4_spv_lookup(fn, role, &e, &fname, &meta);
    if (!spv && e.code == 41 && rdna4x_active()) {
        const BOOL isCompute = (role && strcmp(role, "kernel") == 0);
        const uint64_t dl = (isCompute && !rdna4_is_ws())
            ? UINT64_MAX
            : (rdna4_now() + (uint64_t)RDNA4G_SYNC_WAIT_APP_MS * 1000000ull);
        if (rdna4x_translate_fn_with_error(fn, role, dl, &xe)) {
            e = nil; meta = nil;
            spv = rdna4_spv_lookup(fn, role, &e, &fname, &meta);
        }
    }
    if (!spv) {
        if (err) *err = rdna4_ba_err(MTLBinaryArchiveErrorCompilationFailure,
            [NSString stringWithFormat:@"Failed to resolve %s '%@' SPIR-V for binary archive", role, fname ?: @"?"],
            xe ?: e);
        return NO;
    }
    if (!rdna4ba_spv_header_ok(spv.bytes, spv.length)) {
        if (err) *err = rdna4_ba_err(MTLBinaryArchiveErrorCompilationFailure,
            [NSString stringWithFormat:@"Invalid SPIR-V header for %s '%@'", role, fname ?: @"?"], nil);
        return NO;
    }
    if (spvOut) *spvOut = spv;
    if (metaOut) *metaOut = meta;
    if (fnameOut) *fnameOut = fname;
    return YES;
}

static BOOL rdna4_ba_encode_meta(NSDictionary *meta, BOOL required, NSData **outData, NSError **err) {
    if (outData) *outData = nil;
    if (!meta) {
        if (required) {
            if (err) *err = rdna4_ba_err(MTLBinaryArchiveErrorCompilationFailure,
                @"Shader reflection metadata (.meta.json) is required for binary archive entry", nil);
            return NO;
        }
        return YES;
    }
    if (![NSJSONSerialization isValidJSONObject:meta]) {
        if (err) *err = rdna4_ba_err(MTLBinaryArchiveErrorCompilationFailure,
            @"Shader reflection metadata is not JSON-serializable", nil);
        return NO;
    }
    NSError *je = nil;
    NSData *d = [NSJSONSerialization dataWithJSONObject:meta options:NSJSONWritingSortedKeys error:&je];
    if (!d) {
        if (err) *err = rdna4_ba_err(MTLBinaryArchiveErrorCompilationFailure,
            @"Failed to serialize shader reflection metadata", je);
        return NO;
    }
    if (outData) *outData = d;
    return YES;
}

static BOOL rdna4_ba_decode_meta(NSData *raw, NSDictionary **outMeta, NSError **err) {
    if (outMeta) *outMeta = nil;
    if (!raw.length) return YES;
    NSError *je = nil;
    id obj = [NSJSONSerialization JSONObjectWithData:raw options:0 error:&je];
    if (![obj isKindOfClass:[NSDictionary class]]) {
        if (err) *err = rdna4_ba_err(MTLBinaryArchiveErrorInvalidFile,
            @"Corrupt reflection metadata JSON inside binary archive entry", je);
        return NO;
    }
    if (outMeta) *outMeta = (NSDictionary *)obj;
    return YES;
}

// ---- RDNA4BinaryArchive implementation ----

@implementation RDNA4BinaryArchive
- (instancetype)initWithDevice:(id)dev descriptor:(MTLBinaryArchiveDescriptor *)descriptor error:(NSError **)error {
    self = [super init];
    if (!self) return nil;
    if (!dev || !descriptor) {
        if (error) *error = rdna4_ba_err(MTLBinaryArchiveErrorInvalidFile,
            @"Binary archive requires a non-nil device and MTLBinaryArchiveDescriptor", nil);
        return nil;
    }
    _dev = dev;
    _lock = [NSLock new];
    static dispatch_once_t crcOnce;
    dispatch_once(&crcOnce, ^{ (void)rdna4crc32(0, "", 0); });
    if (!rdna4_ba_device_fingerprint(&_fp, error)) return nil;
    rdna4ba_init(&_archive, &_fp);

    if (descriptor.url) {
        if (!descriptor.url.isFileURL || !descriptor.url.path.length) {
            if (error) *error = rdna4_ba_err(MTLBinaryArchiveErrorInvalidFile,
                @"Binary archive URL must be a non-empty file URL", nil);
            return nil;
        }
        rdna4ba_archive_t loaded;
        int ioErr = 0;
        rdna4ba_status_t st = rdna4ba_read_file(descriptor.url.fileSystemRepresentation, &_fp, &loaded, &ioErr);
        if (st != RDNA4BA_OK) {
            NSString *msg = ioErr
                ? [NSString stringWithFormat:@"Failed to load binary archive '%@': %s (%s)",
                   descriptor.url.path, rdna4ba_status_str(st), strerror(ioErr)]
                : [NSString stringWithFormat:@"Rejected binary archive '%@': %s",
                   descriptor.url.path, rdna4ba_status_str(st)];
            RDNA4LOG("RDNA4BinaryArchive load rejected: %s", msg.UTF8String);
            if (error) *error = rdna4_ba_err(MTLBinaryArchiveErrorInvalidFile, msg, nil);
            return nil;
        }
        rdna4ba_destroy(&_archive);
        _archive = loaded;
        RDNA4LOG("RDNA4BinaryArchive %p loaded '%s': %u entries (vendor 0x%04x device 0x%04x)",
                 (__bridge void *)self, descriptor.url.fileSystemRepresentation,
                 _archive.count, _fp.vendor_id, _fp.device_id);
    }
    return self;
}

- (void)dealloc {
    rdna4ba_destroy(&_archive);
}

RDNA4_DNR(RDNA4BinaryArchive)

- (id)device { return _dev; }

- (NSString *)label {
    [_lock lock];
    NSString *l = _lbl;
    [_lock unlock];
    return l;
}

- (void)setLabel:(NSString *)label {
    NSString *copied = [label copy];
    [_lock lock];
    _lbl = copied;
    [_lock unlock];
}

- (NSUInteger)rdna4EntryCount {
    [_lock lock];
    NSUInteger n = (NSUInteger)_archive.count;
    [_lock unlock];
    return n;
}

- (const rdna4ba_fingerprint_t *)rdna4Fingerprint {
    return &_fp;
}

- (BOOL)rdna4StoreEntry:(const rdna4ba_entry_view_t *)view error:(NSError **)error {
    [_lock lock];
    rdna4ba_status_t st = rdna4ba_put(&_archive, view);
    [_lock unlock];
    if (st != RDNA4BA_OK) {
        if (error) *error = rdna4_ba_err(
            st == RDNA4BA_ERR_INVALID ? MTLBinaryArchiveErrorUnexpectedElement : MTLBinaryArchiveErrorCompilationFailure,
            [NSString stringWithFormat:@"Failed to store binary archive entry: %s", rdna4ba_status_str(st)], nil);
        return NO;
    }
    return YES;
}

- (BOOL)rdna4LookupKind:(uint32_t)kind keyU64:(uint64_t)keyU64 keySha:(const uint8_t[32])keySha
                   spv0:(NSData **)spv0 meta0:(NSData **)meta0
                   spv1:(NSData **)spv1 meta1:(NSData **)meta1
                vkCache:(NSData **)vkCache {
    if (spv0) *spv0 = nil;
    if (meta0) *meta0 = nil;
    if (spv1) *spv1 = nil;
    if (meta1) *meta1 = nil;
    if (vkCache) *vkCache = nil;
    [_lock lock];
    const rdna4ba_entry_t *e = rdna4ba_find(&_archive, kind, keyU64, keySha);
    BOOL found = NO;
    if (e) {
        rdna4ba_entry_view_t v = rdna4ba_entry_view(e);
        if (spv0 && v.spv0_size) *spv0 = [NSData dataWithBytes:v.spv0 length:v.spv0_size];
        if (meta0 && v.meta0_size) *meta0 = [NSData dataWithBytes:v.meta0 length:v.meta0_size];
        if (spv1 && v.spv1_size) *spv1 = [NSData dataWithBytes:v.spv1 length:v.spv1_size];
        if (meta1 && v.meta1_size) *meta1 = [NSData dataWithBytes:v.meta1 length:v.meta1_size];
        if (vkCache) {
            if (v.vk_cache_size) *vkCache = [NSData dataWithBytes:v.vk_cache length:v.vk_cache_size];
            else if (_archive.vk_cache_size) *vkCache = [NSData dataWithBytes:_archive.vk_cache length:_archive.vk_cache_size];
        }
        found = YES;
    }
    [_lock unlock];
    return found;
}

- (BOOL)addComputePipelineFunctionsWithDescriptor:(MTLComputePipelineDescriptor *)descriptor error:(NSError **)error {
    if (!descriptor || !descriptor.computeFunction) {
        if (error) *error = rdna4_ba_err(MTLBinaryArchiveErrorUnexpectedElement,
            @"addComputePipelineFunctionsWithDescriptor: requires a descriptor with computeFunction", nil);
        return NO;
    }
    RDNA4_LK_SCOPE;
    rdna4x_linkage_compute(descriptor);

    uint8_t keySha[32];
    uint64_t keyU64 = 0;
    if (!rdna4_ba_compute_key(descriptor, keySha, &keyU64, error)) return NO;

    NSData *spv = nil; NSDictionary *meta = nil; NSString *fname = nil;
    if (!rdna4_ba_resolve_stage(descriptor.computeFunction, "kernel", &spv, &meta, &fname, error)) return NO;

    NSData *metaData = nil;
    if (!rdna4_ba_encode_meta(meta, YES, &metaData, error)) return NO;

    rdna4_ba_vkcache_scope_t vcScope;
    rdna4_ba_vkcache_scope_begin(&vcScope, nil, &_fp);
    NSError *perr = nil;
    id pso = rdna4_ba_compile_compute_state(_dev, descriptor.computeFunction, spv, meta, fname, &perr);
    NSData *vkData = pso ? rdna4_ba_vkcache_scope_extract(&vcScope, &_fp) : nil;
    rdna4_ba_vkcache_scope_end(&vcScope);
    if (!pso) {
        if (error) *error = rdna4_ba_err(MTLBinaryArchiveErrorCompilationFailure,
            [NSString stringWithFormat:@"Failed to compile compute pipeline '%@' for binary archive", fname ?: @"?"], perr);
        return NO;
    }

    if (spv.length > RDNA4BA_MAX_ENTRY_PAYLOAD || metaData.length > RDNA4BA_MAX_ENTRY_PAYLOAD ||
        vkData.length > RDNA4BA_MAX_ENTRY_PAYLOAD) {
        if (error) *error = rdna4_ba_err(MTLBinaryArchiveErrorCompilationFailure,
            @"Compute pipeline archive payload exceeds entry size limit", nil);
        return NO;
    }

    rdna4ba_entry_view_t view;
    memset(&view, 0, sizeof view);
    view.kind = RDNA4BA_KIND_COMPUTE;
    view.key_u64 = keyU64;
    memcpy(view.key_sha, keySha, 32);
    view.spv0 = spv.bytes; view.spv0_size = (uint32_t)spv.length;
    view.meta0 = metaData.bytes; view.meta0_size = (uint32_t)metaData.length;
    if (vkData.length) {
        view.vk_cache = vkData.bytes;
        view.vk_cache_size = (uint32_t)vkData.length;
    }
    if (![self rdna4StoreEntry:&view error:error]) return NO;
    RDNA4LOG("RDNA4BinaryArchive %p added compute '%s' (spv %u B, meta %u B, vkCache %u B)",
             (__bridge void *)self, fname.UTF8String ?: "?", view.spv0_size, view.meta0_size, view.vk_cache_size);
    return YES;
}

- (BOOL)addRenderPipelineFunctionsWithDescriptor:(MTLRenderPipelineDescriptor *)descriptor error:(NSError **)error {
    if (!descriptor || !descriptor.vertexFunction) {
        if (error) *error = rdna4_ba_err(MTLBinaryArchiveErrorUnexpectedElement,
            @"addRenderPipelineFunctionsWithDescriptor: requires a descriptor with vertexFunction", nil);
        return NO;
    }
    RDNA4_LK_SCOPE;
    rdna4x_linkage_render(descriptor);

    uint8_t keySha[32];
    uint64_t keyU64 = 0;
    if (!rdna4_ba_render_key(descriptor, keySha, &keyU64, error)) return NO;

    NSData *vsSpv = nil; NSDictionary *vsMeta = nil; NSString *vname = nil;
    if (!rdna4_ba_resolve_stage(descriptor.vertexFunction, "vertex", &vsSpv, &vsMeta, &vname, error)) return NO;

    NSData *fsSpv = nil; NSDictionary *fsMeta = nil; NSString *fname = nil;
    if (descriptor.fragmentFunction) {
        if (!rdna4_ba_resolve_stage(descriptor.fragmentFunction, "fragment", &fsSpv, &fsMeta, &fname, error)) return NO;
    }

    NSData *vsMetaData = nil, *fsMetaData = nil;
    if (!rdna4_ba_encode_meta(vsMeta, NO, &vsMetaData, error)) return NO;
    if (!rdna4_ba_encode_meta(fsMeta, NO, &fsMetaData, error)) return NO;

    MTLRenderPipelineDescriptor *cleanDesc = [descriptor copy];
    if ([cleanDesc respondsToSelector:@selector(setBinaryArchives:)]) cleanDesc.binaryArchives = nil;

    rdna4_ba_vkcache_scope_t vcScope;
    rdna4_ba_vkcache_scope_begin(&vcScope, nil, &_fp);
    NSError *perr = nil;
    Class rpCls = NSClassFromString(@"RDNA4RenderPipelineState");
    id<RDNA4RenderPipelineArchiveInit> pso = rpCls
        ? [(id<RDNA4RenderPipelineArchiveInit>)[rpCls alloc] initWithDevice:_dev descriptor:cleanDesc noFallback:YES error:&perr]
        : nil;
    VkPipeline vkp = pso ? [pso vkPipeline] : VK_NULL_HANDLE;
    NSData *vkData = (pso && vkp) ? rdna4_ba_vkcache_scope_extract(&vcScope, &_fp) : nil;
    rdna4_ba_vkcache_scope_end(&vcScope);
    if (!pso || !vkp) {
        if (error) *error = rdna4_ba_err(MTLBinaryArchiveErrorCompilationFailure,
            [NSString stringWithFormat:@"Failed to compile render pipeline '%@/%@' for binary archive",
             vname ?: @"?", fname ?: @"(none)"], perr);
        return NO;
    }

    if (vsSpv.length > RDNA4BA_MAX_ENTRY_PAYLOAD || vsMetaData.length > RDNA4BA_MAX_ENTRY_PAYLOAD ||
        fsSpv.length > RDNA4BA_MAX_ENTRY_PAYLOAD || fsMetaData.length > RDNA4BA_MAX_ENTRY_PAYLOAD ||
        vkData.length > RDNA4BA_MAX_ENTRY_PAYLOAD) {
        if (error) *error = rdna4_ba_err(MTLBinaryArchiveErrorCompilationFailure,
            @"Render pipeline archive payload exceeds entry size limit", nil);
        return NO;
    }

    rdna4ba_entry_view_t view;
    memset(&view, 0, sizeof view);
    view.kind = RDNA4BA_KIND_RENDER;
    view.key_u64 = keyU64;
    memcpy(view.key_sha, keySha, 32);
    view.spv0 = vsSpv.bytes; view.spv0_size = (uint32_t)vsSpv.length;
    if (vsMetaData.length) { view.meta0 = vsMetaData.bytes; view.meta0_size = (uint32_t)vsMetaData.length; }
    if (fsSpv.length)      { view.spv1 = fsSpv.bytes; view.spv1_size = (uint32_t)fsSpv.length; }
    if (fsMetaData.length) { view.meta1 = fsMetaData.bytes; view.meta1_size = (uint32_t)fsMetaData.length; }
    if (vkData.length)     { view.vk_cache = vkData.bytes; view.vk_cache_size = (uint32_t)vkData.length; }
    if (![self rdna4StoreEntry:&view error:error]) return NO;
    RDNA4LOG("RDNA4BinaryArchive %p added render '%s/%s' (vs %u B, fs %u B, vkCache %u B)",
             (__bridge void *)self, vname.UTF8String ?: "?", fname.UTF8String ?: "(none)",
             view.spv0_size, view.spv1_size, view.vk_cache_size);
    return YES;
}

- (BOOL)addTileRenderPipelineFunctionsWithDescriptor:(MTLTileRenderPipelineDescriptor *)descriptor error:(NSError **)error {
    (void)descriptor;
    if (error) *error = rdna4_ba_err(MTLBinaryArchiveErrorCompilationFailure,
        @"Tile render pipelines are not supported on discrete AMD RDNA 4 GPUs (Apple Silicon TBDR-only)", nil);
    return NO;
}

- (BOOL)addMeshRenderPipelineFunctionsWithDescriptor:(id)descriptor error:(NSError **)error {
    (void)descriptor;
    if (error) *error = rdna4_ba_err(MTLBinaryArchiveErrorCompilationFailure,
        @"Mesh render pipeline binary archive entries are not supported yet", nil);
    return NO;
}

- (BOOL)addFunctionWithDescriptor:(MTLFunctionDescriptor *)descriptor library:(id<MTLLibrary>)library error:(NSError **)error {
    if (!descriptor || !library || !descriptor.name.length) {
        if (error) *error = rdna4_ba_err(MTLBinaryArchiveErrorUnexpectedElement,
            @"addFunctionWithDescriptor:library:error: requires a non-nil library and named MTLFunctionDescriptor", nil);
        return NO;
    }
    NSError *ferr = nil;
    id<MTLFunction> fn = [library newFunctionWithDescriptor:descriptor error:&ferr];
    if (!fn) {
        if (error) *error = rdna4_ba_err(MTLBinaryArchiveErrorCompilationFailure,
            [NSString stringWithFormat:@"Failed to resolve function '%@' from library", descriptor.name], ferr);
        return NO;
    }
    uint8_t keySha[32];
    uint64_t keyU64 = 0;
    if (!rdna4_ba_function_key(fn, descriptor, keySha, &keyU64, error)) return NO;

    const char *role = fn.functionType == MTLFunctionTypeVertex ? "vertex" :
                       fn.functionType == MTLFunctionTypeFragment ? "fragment" :
                       fn.functionType == MTLFunctionTypeKernel ? "kernel" : "function";
    NSData *spv = nil; NSDictionary *meta = nil; NSString *fname = nil;
    if (!rdna4_ba_resolve_stage(fn, role, &spv, &meta, &fname, error)) return NO;

    NSData *metaData = nil;
    if (!rdna4_ba_encode_meta(meta, NO, &metaData, error)) return NO;

    if (spv.length > RDNA4BA_MAX_ENTRY_PAYLOAD || metaData.length > RDNA4BA_MAX_ENTRY_PAYLOAD) {
        if (error) *error = rdna4_ba_err(MTLBinaryArchiveErrorCompilationFailure,
            @"Function binary archive payload exceeds entry size limit", nil);
        return NO;
    }

    rdna4ba_entry_view_t view;
    memset(&view, 0, sizeof view);
    view.kind = RDNA4BA_KIND_FUNCTION;
    view.key_u64 = keyU64;
    memcpy(view.key_sha, keySha, 32);
    view.spv0 = spv.bytes; view.spv0_size = (uint32_t)spv.length;
    if (metaData.length) { view.meta0 = metaData.bytes; view.meta0_size = (uint32_t)metaData.length; }
    if (![self rdna4StoreEntry:&view error:error]) return NO;
    RDNA4LOG("RDNA4BinaryArchive %p added function '%s' (%s, spv %u B, meta %u B)",
             (__bridge void *)self, fname.UTF8String ?: "?", role, view.spv0_size, view.meta0_size);
    return YES;
}

- (BOOL)serializeToURL:(NSURL *)url error:(NSError **)error {
    if (!url || !url.isFileURL || !url.path.length) {
        if (error) *error = rdna4_ba_err(MTLBinaryArchiveErrorInvalidFile,
            @"serializeToURL: requires a non-empty file URL", nil);
        return NO;
    }
    int ioErr = 0;
    [_lock lock];
    rdna4ba_status_t st = rdna4ba_write_file_atomic(&_archive, url.fileSystemRepresentation, &ioErr);
    uint32_t count = _archive.count;
    [_lock unlock];
    if (st != RDNA4BA_OK) {
        NSString *msg = ioErr
            ? [NSString stringWithFormat:@"Failed to serialize binary archive to '%@': %s (%s)",
               url.path, rdna4ba_status_str(st), strerror(ioErr)]
            : [NSString stringWithFormat:@"Failed to serialize binary archive to '%@': %s",
               url.path, rdna4ba_status_str(st)];
        if (error) *error = rdna4_ba_err(MTLBinaryArchiveErrorInvalidFile, msg, nil);
        return NO;
    }
    RDNA4LOG("RDNA4BinaryArchive %p serialized %u entries to '%s'",
             (__bridge void *)self, count, url.fileSystemRepresentation);
    return YES;
}
@end

// ---- Pipeline lookup helpers (compute + render + standalone function) ----
GARNET_INTERNAL BOOL rdna4_ba_lookup_compute(id dev, MTLComputePipelineDescriptor *d, NSUInteger options,
                                    NSData **spvOut, NSDictionary **metaOut, NSData **vkCacheOut,
                                    rdna4ba_fingerprint_t *fpOut, BOOL *hitOut, NSError **err) {
    if (spvOut) *spvOut = nil;
    if (metaOut) *metaOut = nil;
    if (vkCacheOut) *vkCacheOut = nil;
    if (hitOut) *hitOut = NO;

    NSArray *archives = (d && [d respondsToSelector:@selector(binaryArchives)]) ? d.binaryArchives : nil;
    const BOOL failOnMiss = (options & RDNA4_PIPELINE_OPT_FAIL_ON_ARCHIVE_MISS) != 0;
    if (!archives.count) {
        if (failOnMiss) {
            if (err) *err = rdna4_ba_err(MTLBinaryArchiveErrorCompilationFailure,
                @"Compute pipeline missed binary archive: MTLPipelineOptionFailOnBinaryArchiveMiss set with no binaryArchives", nil);
            return NO;
        }
        return YES;
    }

    uint8_t keySha[32];
    uint64_t keyU64 = 0;
    NSError *kerr = nil;
    if (!rdna4_ba_compute_key(d, keySha, &keyU64, &kerr)) {
        if (failOnMiss) { if (err) *err = kerr; return NO; }
        return YES;
    }

    for (id obj in archives) {
        if (![obj isKindOfClass:[RDNA4BinaryArchive class]]) {
            if (err) *err = rdna4_ba_err(MTLBinaryArchiveErrorUnexpectedElement,
                @"binaryArchives contains an object that is not a Garnet MTLBinaryArchive", nil);
            return NO;
        }
        RDNA4BinaryArchive *ba = (RDNA4BinaryArchive *)obj;
        if ([ba device] != dev) {
            if (err) *err = rdna4_ba_err(MTLBinaryArchiveErrorUnexpectedElement,
                @"binaryArchives contains an archive created for a different MTLDevice", nil);
            return NO;
        }
        NSData *spv = nil, *metaRaw = nil, *vk = nil;
        if (![ba rdna4LookupKind:RDNA4BA_KIND_COMPUTE keyU64:keyU64 keySha:keySha
                            spv0:&spv meta0:&metaRaw spv1:NULL meta1:NULL vkCache:&vk])
            continue;
        if (!rdna4ba_spv_header_ok(spv.bytes, spv.length)) {
            if (err) *err = rdna4_ba_err(MTLBinaryArchiveErrorInvalidFile,
                @"Corrupt compute SPIR-V payload inside binary archive", nil);
            return NO;
        }
        NSDictionary *meta = nil;
        if (!rdna4_ba_decode_meta(metaRaw, &meta, err)) return NO;
        if (spvOut) *spvOut = spv;
        if (metaOut) *metaOut = meta;
        if (vkCacheOut) *vkCacheOut = vk;
        if (fpOut) *fpOut = *[ba rdna4Fingerprint];
        if (hitOut) *hitOut = YES;
        RDNA4LOG("binaryArchive HIT compute '%s' (spv %lu B, vkCache %lu B)",
                 ([d.computeFunction respondsToSelector:@selector(name)] ? d.computeFunction.name.UTF8String : "?") ?: "?",
                 (unsigned long)spv.length, (unsigned long)vk.length);
        return YES;
    }

    if (failOnMiss) {
        NSString *nm = [d.computeFunction respondsToSelector:@selector(name)] ? d.computeFunction.name : @"?";
        if (err) *err = rdna4_ba_err(MTLBinaryArchiveErrorCompilationFailure,
            [NSString stringWithFormat:@"Compute pipeline '%@' not found in binaryArchives (MTLPipelineOptionFailOnBinaryArchiveMiss)", nm ?: @"?"],
            nil);
        return NO;
    }
    return YES;
}

__attribute__((unused))
GARNET_INTERNAL BOOL rdna4_ba_lookup_render(id dev, MTLRenderPipelineDescriptor *d, NSUInteger options,
                                   NSData **vsSpvOut, NSDictionary **vsMetaOut,
                                   NSData **fsSpvOut, NSDictionary **fsMetaOut,
                                   NSData **vkCacheOut, rdna4ba_fingerprint_t *fpOut,
                                   BOOL *hitOut, NSError **err) {
    if (vsSpvOut) *vsSpvOut = nil;
    if (vsMetaOut) *vsMetaOut = nil;
    if (fsSpvOut) *fsSpvOut = nil;
    if (fsMetaOut) *fsMetaOut = nil;
    if (vkCacheOut) *vkCacheOut = nil;
    if (hitOut) *hitOut = NO;

    NSArray *archives = (d && [d respondsToSelector:@selector(binaryArchives)]) ? d.binaryArchives : nil;
    const BOOL failOnMiss = (options & RDNA4_PIPELINE_OPT_FAIL_ON_ARCHIVE_MISS) != 0;
    if (!archives.count) {
        if (failOnMiss) {
            if (err) *err = rdna4_ba_err(MTLBinaryArchiveErrorCompilationFailure,
                @"Render pipeline missed binary archive: MTLPipelineOptionFailOnBinaryArchiveMiss set with no binaryArchives", nil);
            return NO;
        }
        return YES;
    }

    uint8_t keySha[32];
    uint64_t keyU64 = 0;
    NSError *kerr = nil;
    if (!rdna4_ba_render_key(d, keySha, &keyU64, &kerr)) {
        if (failOnMiss) { if (err) *err = kerr; return NO; }
        return YES;
    }

    const BOOL hasFrag = d.fragmentFunction != nil;
    for (id obj in archives) {
        if (![obj isKindOfClass:[RDNA4BinaryArchive class]]) {
            if (err) *err = rdna4_ba_err(MTLBinaryArchiveErrorUnexpectedElement,
                @"binaryArchives contains an object that is not a Garnet MTLBinaryArchive", nil);
            return NO;
        }
        RDNA4BinaryArchive *ba = (RDNA4BinaryArchive *)obj;
        if ([ba device] != dev) {
            if (err) *err = rdna4_ba_err(MTLBinaryArchiveErrorUnexpectedElement,
                @"binaryArchives contains an archive created for a different MTLDevice", nil);
            return NO;
        }
        NSData *vsSpv = nil, *vsMetaRaw = nil, *fsSpv = nil, *fsMetaRaw = nil, *vk = nil;
        if (![ba rdna4LookupKind:RDNA4BA_KIND_RENDER keyU64:keyU64 keySha:keySha
                            spv0:&vsSpv meta0:&vsMetaRaw spv1:&fsSpv meta1:&fsMetaRaw vkCache:&vk])
            continue;
        if (!rdna4ba_spv_header_ok(vsSpv.bytes, vsSpv.length) ||
            (hasFrag && !rdna4ba_spv_header_ok(fsSpv.bytes, fsSpv.length)) ||
            (!hasFrag && fsSpv.length)) {
            if (err) *err = rdna4_ba_err(MTLBinaryArchiveErrorInvalidFile,
                @"Corrupt render SPIR-V payload inside binary archive", nil);
            return NO;
        }
        NSDictionary *vsMeta = nil, *fsMeta = nil;
        if (!rdna4_ba_decode_meta(vsMetaRaw, &vsMeta, err) ||
            !rdna4_ba_decode_meta(fsMetaRaw, &fsMeta, err))
            return NO;
        if (vsSpvOut) *vsSpvOut = vsSpv;
        if (vsMetaOut) *vsMetaOut = vsMeta;
        if (fsSpvOut) *fsSpvOut = fsSpv;
        if (fsMetaOut) *fsMetaOut = fsMeta;
        if (vkCacheOut) *vkCacheOut = vk;
        if (fpOut) *fpOut = *[ba rdna4Fingerprint];
        if (hitOut) *hitOut = YES;
        RDNA4LOG("binaryArchive HIT render '%s/%s' (vs %lu B, fs %lu B, vkCache %lu B)",
                 ([d.vertexFunction respondsToSelector:@selector(name)] ? d.vertexFunction.name.UTF8String : "?") ?: "?",
                 (hasFrag && [d.fragmentFunction respondsToSelector:@selector(name)] ? d.fragmentFunction.name.UTF8String : "(none)") ?: "(none)",
                 (unsigned long)vsSpv.length, (unsigned long)fsSpv.length, (unsigned long)vk.length);
        return YES;
    }

    if (failOnMiss) {
        NSString *vn = [d.vertexFunction respondsToSelector:@selector(name)] ? d.vertexFunction.name : @"?";
        NSString *fn = hasFrag && [d.fragmentFunction respondsToSelector:@selector(name)] ? d.fragmentFunction.name : @"(none)";
        if (err) *err = rdna4_ba_err(MTLBinaryArchiveErrorCompilationFailure,
            [NSString stringWithFormat:@"Render pipeline '%@/%@' not found in binaryArchives (MTLPipelineOptionFailOnBinaryArchiveMiss)",
             vn ?: @"?", fn ?: @"(none)"], nil);
        return NO;
    }
    return YES;
}

__attribute__((unused))
static BOOL rdna4_ba_lookup_function(id dev, id<MTLFunction> fn, MTLFunctionDescriptor *fd,
                                     NSArray *archives, NSData **spvOut, NSDictionary **metaOut,
                                     BOOL *hitOut, NSError **err) {
    if (spvOut) *spvOut = nil;
    if (metaOut) *metaOut = nil;
    if (hitOut) *hitOut = NO;
    if (!fn || !archives.count) return YES;

    uint8_t keySha[32];
    uint64_t keyU64 = 0;
    if (!rdna4_ba_function_key(fn, fd, keySha, &keyU64, NULL)) return YES;

    for (id obj in archives) {
        if (![obj isKindOfClass:[RDNA4BinaryArchive class]]) continue;
        RDNA4BinaryArchive *ba = (RDNA4BinaryArchive *)obj;
        if ([ba device] != dev) continue;
        NSData *spv = nil, *metaRaw = nil;
        if (![ba rdna4LookupKind:RDNA4BA_KIND_FUNCTION keyU64:keyU64 keySha:keySha
                            spv0:&spv meta0:&metaRaw spv1:NULL meta1:NULL vkCache:NULL])
            continue;
        if (!rdna4ba_spv_header_ok(spv.bytes, spv.length)) {
            if (err) *err = rdna4_ba_err(MTLBinaryArchiveErrorInvalidFile,
                @"Corrupt function SPIR-V payload inside binary archive", nil);
            return NO;
        }
        NSDictionary *meta = nil;
        if (!rdna4_ba_decode_meta(metaRaw, &meta, err)) return NO;
        if (spvOut) *spvOut = spv;
        if (metaOut) *metaOut = meta;
        if (hitOut) *hitOut = YES;
        return YES;
    }
    return YES;
}

