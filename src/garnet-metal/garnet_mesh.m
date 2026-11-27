// garnet_mesh.m - GarnetMetal modular compilation unit: Metal 3 Mesh & Object shader pipelines and encoder bindings.
#if !defined(GARNET_INTERNAL) && !defined(GARNET_METAL_MODULAR_BUILD)
#define GARNET_INTERNAL static
#endif
#ifdef GARNET_METAL_MODULAR_BUILD
#include "garnet_metal_internal.h"
#endif
#include "garnet_fragment_output.h"

// ---------------------------------------------------------------------------------------------------------------
// Metal 3 Mesh & Object Shader Runtime (Task 6.2):
//   RDNA4MeshRenderPipelineState (subclass of RDNA4RenderPipelineState conforming to MTLRenderPipelineState)
//   RDNA4RenderEncoder (RDNA4Mesh) (object/mesh bindings + direct/threads/indirect mesh draws)
//   Supports:
//     1. Direct hardware Mesh-only (`VK_SHADER_STAGE_MESH_BIT_EXT`, GFX ring 0 with mesa-patches/0029)
//     2. Native hardware Object+Mesh (`VK_SHADER_STAGE_TASK_BIT_EXT + VK_SHADER_STAGE_MESH_BIT_EXT`)
//     3. GFX-ring compute-emulated Object (`GLCompute` with GARNETX_CAP_MESH_EMULATED_PAYLOAD) +
//        `COMPUTE -> DRAW_INDIRECT | MESH_SHADER` barrier + `vkCmdDrawMeshTasksIndirectEXT` (0078 ABI)
//
// Wave 3E integration checklist (GarnetDevice.m, rdna4_backend_impl.inc, rdna4_render_encoder_impl.inc, rdna4_shader_runtime_impl.inc):
//   1. rdna4_backend_impl.inc (REQUIRED for multi-group / indirect emulated Object+Mesh `drawCount > 1`
//      under VUID-vkCmdDrawMeshTasksIndirectEXT-drawCount-02718):
//        Enable `ef.multiDrawIndirect = pf.multiDrawIndirect;` in `VkPhysicalDeviceFeatures`,
//        store `BOOL multiDrawIndirect;` in `RDNA4R` (`RDNA4R.multiDrawIndirect = ef.multiDrawIndirect != VK_FALSE;`),
//        and add `#define RDNA4_HAS_MULTI_DRAW_INDIRECT 1` right after `RDNA4R`.
//   2. GarnetDevice.m include order: place after `#include "rdna4_rt_accel_impl.m"` (after
//      RDNA4RenderPipelineState, RDNA4RenderEncoder, RDNA4CommandBuffer, and rdna4_shader_runtime_impl.inc):
//        #include "rdna4_mesh_impl.m"
//   3. GarnetDevice.m +load protocol registration (also auto-registered in +initialize):
//        { const char *n[] = { "MTLRenderPipelineState" };
//          rdna4_add_protocols([RDNA4MeshRenderPipelineState class], n, 1, "RDNA4MeshRenderPipelineState"); }
//   4. GarnetDevice.m @implementation GarnetDevice selectors:
//        - (id)newRenderPipelineStateWithMeshDescriptor:(id)d options:(NSUInteger)opts
//                                            reflection:(MTLRenderPipelineReflection **)refl
//                                                 error:(NSError **)error {
//            return rdna4_mesh_device_new_pipeline(self, d, opts, refl, error);
//        }
//        - (void)newRenderPipelineStateWithMeshDescriptor:(id)d options:(NSUInteger)opts
//                                       completionHandler:(void (^)(id, id, NSError *))h {
//            rdna4_mesh_device_new_pipeline_async(self, d, opts, h);
//        }
//   5. rdna4_render_encoder_impl.inc (`rdna4PrepareDraw:`):
//        Add guard right after `_pso` hot-swap:
//          if ([_pso rdna4IsMeshPipeline]) {
//              [_cb rdna4Fail:@"drawPrimitives/drawPatches called with a mesh render pipeline"];
//              return NO;
//          }
//        All object/mesh binding and draw selectors live in `RDNA4RenderEncoder (RDNA4Mesh)` below.
//   6. rdna4_shader_runtime_impl.inc / rdna4_argument_tables.inc (optional):
//        If Object/Mesh stages ever use Tier 2 GPU argument tables, OR
//        `VK_SHADER_STAGE_MESH_BIT_EXT` / `VK_SHADER_STAGE_TASK_BIT_EXT` into
//        `rdna4_argument_stage_flags()` when `RDNA4R.meshShader` / `RDNA4R.objectShader` is YES.
// ---------------------------------------------------------------------------------------------------------------
#ifndef RDNA4_MESH_IMPL_INC
#define RDNA4_MESH_IMPL_INC


// Distinct encoder resource-binding slot kinds (1..6 live in rdna4_encoder_bindings.inc;
// reserve 32..35 for Object/Mesh so Wave 3E integration cannot collide with RT/other bands).
enum {
    RDNA4_RESOURCE_OBJECT_BUFFER  = 32,
    RDNA4_RESOURCE_OBJECT_TEXTURE = 33,
    RDNA4_RESOURCE_MESH_BUFFER    = 34,
    RDNA4_RESOURCE_MESH_TEXTURE   = 35,
};

// =================================================================================================
// 1. Lazy VK_EXT_mesh_shader proc loader & garnetxlate Stage 3 FFI resolver
// =================================================================================================

typedef struct {
    BOOL ready;
    PFN_vkCmdDrawMeshTasksEXT cmdDrawMeshTasksEXT;
    PFN_vkCmdDrawMeshTasksIndirectEXT cmdDrawMeshTasksIndirectEXT;
} RDNA4MeshFnTable;

static RDNA4MeshFnTable RDNA4MeshFn;

static BOOL rdna4_mesh_resolve_vk_funcs(void) {
    if (!RDNA4R.ok || !RDNA4R.dev || !RDNA4R.meshShader) return NO;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        if (vkGetDeviceProcAddr) {
            RDNA4MeshFn.cmdDrawMeshTasksEXT = (PFN_vkCmdDrawMeshTasksEXT)
                vkGetDeviceProcAddr(RDNA4R.dev, "vkCmdDrawMeshTasksEXT");
            RDNA4MeshFn.cmdDrawMeshTasksIndirectEXT = (PFN_vkCmdDrawMeshTasksIndirectEXT)
                vkGetDeviceProcAddr(RDNA4R.dev, "vkCmdDrawMeshTasksIndirectEXT");
        }
        if (!RDNA4MeshFn.cmdDrawMeshTasksEXT && rdna4_gipa && RDNA4R.inst) {
            RDNA4MeshFn.cmdDrawMeshTasksEXT = (PFN_vkCmdDrawMeshTasksEXT)
                rdna4_gipa(RDNA4R.inst, "vkCmdDrawMeshTasksEXT");
        }
        if (!RDNA4MeshFn.cmdDrawMeshTasksIndirectEXT && rdna4_gipa && RDNA4R.inst) {
            RDNA4MeshFn.cmdDrawMeshTasksIndirectEXT = (PFN_vkCmdDrawMeshTasksIndirectEXT)
                rdna4_gipa(RDNA4R.inst, "vkCmdDrawMeshTasksIndirectEXT");
        }
        RDNA4MeshFn.ready = (RDNA4MeshFn.cmdDrawMeshTasksEXT != NULL &&
                             RDNA4MeshFn.cmdDrawMeshTasksIndirectEXT != NULL);
        if (!RDNA4MeshFn.ready) {
            RDNA4LOG_WARN("mesh: VK_EXT_mesh_shader command entry points are missing");
        }
    });
    return RDNA4MeshFn.ready;
}

static uint32_t rdna4_mesh_max_draw_indirect_count(void) {
#ifdef RDNA4_HAS_MULTI_DRAW_INDIRECT
    if (RDNA4R.multiDrawIndirect && RDNA4R.lim.maxDrawIndirectCount > 1u)
        return RDNA4R.lim.maxDrawIndirectCount;
#endif
    return 1u;
}

typedef struct {
    const uint8_t *symbol;
    size_t symbol_len;
    const uint8_t *ll;
    size_t ll_len;
    uint32_t kind;
} rdna4_mesh_cift_fn;

typedef int (*rdna4_mesh_detect_stage_fn)(
    const uint8_t *ll, size_t ll_len,
    uint32_t *out_stage_kind,
    char *err, size_t err_len);

typedef int (*rdna4_mesh_translate_stage3_fn)(
    const uint8_t *ll, size_t ll_len,
    const uint32_t *local_xyz,
    const rdna4x_cdep *deps, size_t ndeps,
    const rdna4x_cdynlib *dynlibs, size_t ndynlibs,
    const rdna4x_cdep *visible_fns, size_t nvisible_fns,
    const rdna4_mesh_cift_fn *intersection_fns, size_t nintersection_fns,
    const rdna4x_backend_capabilities *capabilities,
    uint8_t **spv, size_t *spv_len,
    uint8_t **meta, size_t *meta_len,
    char *err, size_t err_len);

static struct {
    pthread_mutex_t mu;
    BOOL resolved;
    rdna4_mesh_detect_stage_fn detectStage;
    rdna4_mesh_translate_stage3_fn translateStage3;
} RDNA4MeshXL = { .mu = PTHREAD_MUTEX_INITIALIZER };

static BOOL rdna4_mesh_ensure_stage3_xlib(const rdna4x_xlib **xlibOut,
                                          rdna4_mesh_detect_stage_fn *detectOut,
                                          rdna4_mesh_translate_stage3_fn *translateOut,
                                          NSError **err) {
    const rdna4x_xlib *xl = rdna4x_ensure_xlib_raw(err);
    if (!xl || !xl->h) return NO;
    pthread_mutex_lock(&RDNA4MeshXL.mu);
    if (!RDNA4MeshXL.resolved) {
        *(void **)&RDNA4MeshXL.detectStage = dlsym(xl->h, "garnetx_detect_stage");
        *(void **)&RDNA4MeshXL.translateStage3 = dlsym(xl->h, "garnetx_translate_stage3");
        RDNA4MeshXL.resolved = YES;
    }
    rdna4_mesh_detect_stage_fn detFn = RDNA4MeshXL.detectStage;
    rdna4_mesh_translate_stage3_fn xlatFn = RDNA4MeshXL.translateStage3;
    pthread_mutex_unlock(&RDNA4MeshXL.mu);
    if ((detectOut && !detFn) || (translateOut && !xlatFn)) {
        if (err) *err = rdna4_err(42, @"libgarnetxlate.dylib lacks Metal 3 Stage 3 mesh/object entry points");
        return NO;
    }
    if (xlibOut) *xlibOut = xl;
    if (detectOut) *detectOut = detFn;
    if (translateOut) *translateOut = xlatFn;
    return YES;
}

// =================================================================================================
// 2. Availability-safe descriptor accessors & Footnote 10 classifier
// =================================================================================================

static NSUInteger rdna4_mesh_uint_prop(id obj, const char *selName, NSUInteger fallback) {
    if (!obj || !selName) return fallback;
    SEL s = sel_registerName(selName);
    if (![obj respondsToSelector:s]) return fallback;
    return ((NSUInteger (*)(id, SEL))objc_msgSend)(obj, s);
}

static uint32_t rdna4_mesh_linked_fn10_flags(id linked) {
    if (!linked) return 0u;
    uint32_t flags = 0u;
    if (rdna4x_array_nonempty(linked, "functions"))        flags |= RDNA4MESH_FN10_PUBLIC_FUNCTIONS;
    if (rdna4x_array_nonempty(linked, "binaryFunctions"))  flags |= RDNA4MESH_FN10_BINARY_FUNCTIONS;
    if (rdna4x_array_nonempty(linked, "privateFunctions")) flags |= RDNA4MESH_FN10_PRIVATE_FUNCTIONS;
    id gr = rdna4x_prop(linked, "groups");
    if ([gr isKindOfClass:[NSDictionary class]] && [(NSDictionary *)gr count] > 0)
        flags |= RDNA4MESH_FN10_GROUPS;
    return flags;
}

static uint32_t rdna4_mesh_desc_fn10_flags(id desc) {
    uint32_t flags = 0u;
    flags |= rdna4_mesh_linked_fn10_flags(rdna4x_prop(desc, "objectLinkedFunctions"));
    flags |= rdna4_mesh_linked_fn10_flags(rdna4x_prop(desc, "meshLinkedFunctions"));
    flags |= rdna4_mesh_linked_fn10_flags(rdna4x_prop(desc, "fragmentLinkedFunctions"));
    if (rdna4x_bool_prop(desc, "supportAddingObjectBinaryFunctions") ||
        rdna4x_bool_prop(desc, "supportAddingMeshBinaryFunctions") ||
        rdna4x_bool_prop(desc, "supportAddingFragmentBinaryFunctions"))
        flags |= RDNA4MESH_FN10_SUPPORT_ADD_BIN_FNS;
    return flags;
}

static BOOL rdna4_mesh_parse_meta_dict(NSDictionary *meta, rdna4mesh_refl_t *out, NSError **err) {
    memset(out, 0, sizeof *out);
    if (!meta) return YES;
    if (![NSJSONSerialization isValidJSONObject:meta]) {
        if (err) *err = rdna4_err(49, @"mesh/object reflection metadata is not JSON-serializable");
        return NO;
    }
    NSError *je = nil;
    NSData *raw = [NSJSONSerialization dataWithJSONObject:meta options:0 error:&je];
    if (!raw.length) {
        if (err) *err = rdna4_err(49, je.localizedDescription ?: @"failed to serialize mesh reflection metadata");
        return NO;
    }
    rdna4mesh_status_t st = rdna4mesh_parse_reflection_json((const char *)raw.bytes, raw.length, out);
    if (st != RDNA4MESH_OK) {
        if (err) *err = rdna4_err(49, [NSString stringWithFormat:@"mesh reflection metadata invalid: %s",
                                       rdna4mesh_status_str(st)]);
        return NO;
    }
    return YES;
}

// =================================================================================================
// 3. Stage 3 mesh/object translation + cache key helper (64 MiB worker stack)
// =================================================================================================

static NSString *rdna4_mesh_stage_cache_key(const char *role, NSString *entrySha,
                                            BOOL emulatedPayload, const uint32_t local[3],
                                            NSArray<NSDictionary *> *deps,
                                            NSArray<NSDictionary *> *dynlibs) {
    CC_SHA256_CTX ctx;
    CC_SHA256_Init(&ctx);
    char hdr[160];
    int n = snprintf(hdr, sizeof hdr, "rdna4-mesh-s3-v1\n%s\n%s\nemul=%u\nlocal=%u,%u,%u\n",
                     role ?: "mesh", entrySha.UTF8String ?: "",
                     emulatedPayload ? 1u : 0u, local[0], local[1], local[2]);
    if (n > 0) CC_SHA256_Update(&ctx, hdr, (CC_LONG)n);
    NSMutableArray<NSString *> *parts = [NSMutableArray array];
    for (NSDictionary *d in deps) {
        [parts addObject:[NSString stringWithFormat:@"D:%@:%@", d[@"name"] ?: @"", d[@"sha"] ?: @""]];
    }
    for (NSDictionary *d in dynlibs) {
        [parts addObject:[NSString stringWithFormat:@"L:%@:%@", d[@"name"] ?: @"", d[@"sha"] ?: @""]];
    }
    [parts sortUsingSelector:@selector(compare:)];
    NSString *prev = nil;
    for (NSString *p in parts) {
        if (prev && [prev isEqualToString:p]) continue;
        prev = p;
        const char *u = p.UTF8String;
        if (u) CC_SHA256_Update(&ctx, u, (CC_LONG)strlen(u));
        CC_SHA256_Update(&ctx, "\n", 1);
    }
    uint8_t digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(digest, &ctx);
    char hex[65];
    rdna4x_hex(hex, digest, CC_SHA256_DIGEST_LENGTH);
    return [NSString stringWithUTF8String:hex];
}

typedef struct {
    const rdna4x_xlib *xlib;
    rdna4_mesh_translate_stage3_fn translateFn;
    const char *dir;
    const char *sha;
    const uint8_t *entryAir;
    size_t entryAirLen;
    uint32_t local[3];
    const rdna4x_stage2_in_t *deps;
    size_t ndeps;
    const rdna4x_stage2_in_t *dynlibs;
    size_t ndynlibs;
    rdna4x_backend_capabilities caps;
    uint8_t *outSpv;
    size_t outSpvLen;
    uint8_t *outMeta;
    size_t outMetaLen;
    int rc;
    char err[512];
} rdna4_mesh_s3_work_t;

static void *rdna4_mesh_s3_worker(void *opaque) {
    rdna4_mesh_s3_work_t *w = (rdna4_mesh_s3_work_t *)opaque;
    if (!w->entryAir || !rdna4x_air_size_ok(w->entryAirLen)) {
        snprintf(w->err, sizeof w->err, "mesh/object entry bitcode size outside caps");
        w->rc = RDNA4X_E_CAP;
        return NULL;
    }
    if (w->ndeps > RDNA4X_MAX_DEPS || w->ndynlibs > RDNA4X_MAX_DEPS) {
        snprintf(w->err, sizeof w->err, "mesh/object linkage count exceeds cap (%u)", RDNA4X_MAX_DEPS);
        w->rc = RDNA4X_E_CAP;
        return NULL;
    }

    char *entryLl = NULL; size_t entryLlLen = 0;
    char **depTexts = NULL, **libTexts = NULL;
    rdna4x_cdep *cdeps = NULL;
    rdna4x_cdynlib *clibs = NULL;
    uint8_t *spv = NULL, *meta = NULL;
    size_t spvLen = 0, metaLen = 0;
    int rc = RDNA4X_OK;
    size_t totalLl = 0;

    rc = rdna4x_module_to_llvm_text(w->entryAir, w->entryAirLen, RDNA4DL_PAYLOAD_AIR_BITCODE,
                                    &entryLl, &entryLlLen, w->err, sizeof w->err);
    if (rc != RDNA4X_OK) goto done;
    totalLl = entryLlLen;

    if (w->ndeps) {
        depTexts = (char **)calloc(w->ndeps, sizeof *depTexts);
        cdeps = (rdna4x_cdep *)calloc(w->ndeps, sizeof *cdeps);
        if (!depTexts || !cdeps) { snprintf(w->err, sizeof w->err, "out of memory"); rc = RDNA4X_E_IO; goto done; }
        for (size_t i = 0; i < w->ndeps; i++) {
            size_t tlen = 0;
            rc = rdna4x_module_to_llvm_text(w->deps[i].bytes, w->deps[i].len, w->deps[i].kind,
                                            &depTexts[i], &tlen, w->err, sizeof w->err);
            if (rc != RDNA4X_OK) goto done;
            totalLl += tlen;
            if (totalLl > RDNA4X_MAX_LL) {
                snprintf(w->err, sizeof w->err, "mesh/object linked LLVM IR exceeds cap");
                rc = RDNA4X_E_CAP;
                goto done;
            }
            cdeps[i].symbol = (const uint8_t *)w->deps[i].name;
            cdeps[i].symbol_len = strlen(w->deps[i].name);
            cdeps[i].ll = (const uint8_t *)depTexts[i];
            cdeps[i].ll_len = tlen;
        }
    }
    if (w->ndynlibs) {
        libTexts = (char **)calloc(w->ndynlibs, sizeof *libTexts);
        clibs = (rdna4x_cdynlib *)calloc(w->ndynlibs, sizeof *clibs);
        if (!libTexts || !clibs) { snprintf(w->err, sizeof w->err, "out of memory"); rc = RDNA4X_E_IO; goto done; }
        for (size_t i = 0; i < w->ndynlibs; i++) {
            size_t tlen = 0;
            rc = rdna4x_module_to_llvm_text(w->dynlibs[i].bytes, w->dynlibs[i].len, w->dynlibs[i].kind,
                                            &libTexts[i], &tlen, w->err, sizeof w->err);
            if (rc != RDNA4X_OK) goto done;
            totalLl += tlen;
            if (totalLl > RDNA4X_MAX_LL) {
                snprintf(w->err, sizeof w->err, "mesh/object dynamic library LLVM IR exceeds cap");
                rc = RDNA4X_E_CAP;
                goto done;
            }
            clibs[i].name = (const uint8_t *)w->dynlibs[i].name;
            clibs[i].name_len = strlen(w->dynlibs[i].name);
            clibs[i].ll = (const uint8_t *)libTexts[i];
            clibs[i].ll_len = tlen;
        }
    }

    {
        int tr = w->translateFn((const uint8_t *)entryLl, entryLlLen, w->local,
                                cdeps, w->ndeps, clibs, w->ndynlibs,
                                NULL, 0u, NULL, 0u,
                                &w->caps, &spv, &spvLen, &meta, &metaLen,
                                w->err, sizeof w->err);
        if (tr != 0) {
            if (!w->err[0]) snprintf(w->err, sizeof w->err, "garnetx_translate_stage3 returned %d", tr);
            rc = RDNA4X_E_REFUSED;
            goto done;
        }
    }
    if (!spv || !meta || !rdna4x_spv_size_ok(spvLen) || !rdna4x_meta_size_ok(metaLen)) {
        snprintf(w->err, sizeof w->err, "mesh/object SPIR-V (%zu B) / meta (%zu B) outside caps", spvLen, metaLen);
        rc = RDNA4X_E_CAP;
        goto done;
    }

    if (w->dir && w->dir[0] && !rdna4x_is_mem_dir(w->dir) && w->sha && rdna4x_sha_ok(w->sha)) {
        char path[RDNA4X_PATH_MAX];
        if (rdna4x_file_path(path, sizeof path, w->dir, w->sha, RDNA4X_EXT_META))
            (void)rdna4x_write_atomic(path, meta, metaLen);
        if (rdna4x_file_path(path, sizeof path, w->dir, w->sha, RDNA4X_EXT_SPV))
            (void)rdna4x_write_atomic(path, spv, spvLen);
        (void)rdna4x_trim_dir(w->dir, RDNA4X_CACHE_CAP);
    }

    w->outSpv = (uint8_t *)malloc(spvLen);
    w->outMeta = (uint8_t *)malloc(metaLen);
    if (w->outSpv && w->outMeta) {
        memcpy(w->outSpv, spv, spvLen); w->outSpvLen = spvLen;
        memcpy(w->outMeta, meta, metaLen); w->outMetaLen = metaLen;
    } else {
        free(w->outSpv); w->outSpv = NULL; w->outSpvLen = 0;
        free(w->outMeta); w->outMeta = NULL; w->outMetaLen = 0;
        snprintf(w->err, sizeof w->err, "out of memory copying mesh/object outputs");
        rc = RDNA4X_E_IO;
    }

done:
    free(entryLl);
    if (depTexts) { for (size_t i = 0; i < w->ndeps; i++) free(depTexts[i]); free(depTexts); }
    if (libTexts) { for (size_t i = 0; i < w->ndynlibs; i++) free(libTexts[i]); free(libTexts); }
    free(cdeps); free(clibs);
    if (spv) w->xlib->freep(spv, spvLen);
    if (meta) w->xlib->freep(meta, metaLen);
    w->rc = rc;
    return NULL;
}

static BOOL rdna4_mesh_translate_stage(id fn, const char *role, BOOL emulatedPayload,
                                       const uint32_t local[3],
                                       NSArray<NSDictionary *> *deps,
                                       NSArray<NSDictionary *> *dynlibs,
                                       NSData **outSpv, NSDictionary **outMeta,
                                       rdna4mesh_refl_t *outRefl, NSError **err) {
    if (outSpv) *outSpv = nil;
    if (outMeta) *outMeta = nil;
    if (outRefl) memset(outRefl, 0, sizeof *outRefl);
    if (!fn) {
        if (err) *err = rdna4_err(40, [NSString stringWithFormat:@"%s function is nil", role]);
        return NO;
    }
    SEL bcSel = NSSelectorFromString(@"bitcodeData");
    NSData *bc = [fn respondsToSelector:bcSel] ? ((NSData *(*)(id, SEL))objc_msgSend)(fn, bcSel) : nil;
    NSString *fname = [fn respondsToSelector:@selector(name)] ? [fn name] : @"?";
    if (!bc.length) {
        if (err) *err = rdna4_err(40, [NSString stringWithFormat:@"%s function '%@' has no bitcodeData", role, fname]);
        return NO;
    }
    NSString *entrySha = rdna4_sha256hex(bc);
    NSString *cacheKey = rdna4_mesh_stage_cache_key(role, entrySha, emulatedPayload, local, deps, dynlibs);
    NSString *userDir = rdna4x_user_dir();

    // 1. Check disk cache (composite key first; plain entrySha only for non-emulated default-local precompiled spvcache).
    NSMutableArray<NSString *> *tryKeys = [NSMutableArray arrayWithObject:cacheKey];
    if (!emulatedPayload && !deps.count && !dynlibs.count &&
        local[0] == 64u && local[1] == 1u && local[2] == 1u) {
        [tryKeys addObject:entrySha];
    }
    for (NSString *k in tryKeys) {
        for (NSString *dir in rdna4_spv_dirs()) {
            NSString *spvPath = [dir stringByAppendingPathComponent:[k stringByAppendingString:@".spv"]];
            NSData *cachedSpv = [NSData dataWithContentsOfFile:spvPath];
            if (cachedSpv.length < 20 || (cachedSpv.length & 3)) continue;
            NSDictionary *cachedMeta = rdna4_meta_for_spv(spvPath);
            if (!cachedMeta) continue;
            rdna4mesh_refl_t parsed = {0};
            if (rdna4_mesh_parse_meta_dict(cachedMeta, &parsed, NULL)) {
                if (outSpv) *outSpv = cachedSpv;
                if (outMeta) *outMeta = cachedMeta;
                if (outRefl) *outRefl = parsed;
                return YES;
            }
        }
    }

    // 2. Translate in-process via garnetx_translate_stage3 on a 64 MiB worker stack.
    if (!rdna4x_active()) {
        if (err) *err = rdna4_err(41, [NSString stringWithFormat:@"spvcache miss for %s '%@' and in-process translation is disabled", role, fname]);
        return NO;
    }
    const rdna4x_xlib *xl = NULL;
    rdna4_mesh_translate_stage3_fn xlatFn = NULL;
    if (!rdna4_mesh_ensure_stage3_xlib(&xl, NULL, &xlatFn, err)) return NO;

    NSUInteger nd = deps.count, nl = dynlibs.count;
    rdna4x_stage2_in_t *inDeps = nd ? (rdna4x_stage2_in_t *)calloc(nd, sizeof *inDeps) : NULL;
    rdna4x_stage2_in_t *inLibs = nl ? (rdna4x_stage2_in_t *)calloc(nl, sizeof *inLibs) : NULL;
    if ((nd && !inDeps) || (nl && !inLibs)) {
        free(inDeps); free(inLibs);
        if (err) *err = rdna4_err(42, @"out of memory preparing mesh/object linkage inputs");
        return NO;
    }
    for (NSUInteger i = 0; i < nd; i++) {
        NSDictionary *d = deps[i]; NSData *dbc = d[@"bc"];
        inDeps[i] = (rdna4x_stage2_in_t){ [d[@"name"] UTF8String], (const uint8_t *)dbc.bytes, dbc.length, RDNA4DL_PAYLOAD_AIR_BITCODE };
    }
    for (NSUInteger i = 0; i < nl; i++) {
        NSDictionary *d = dynlibs[i]; NSData *mod = d[@"data"];
        inLibs[i] = (rdna4x_stage2_in_t){ [d[@"name"] UTF8String], (const uint8_t *)mod.bytes, mod.length, (uint32_t)[d[@"kind"] unsignedIntValue] };
    }

    rdna4_mesh_s3_work_t work;
    memset(&work, 0, sizeof work);
    work.xlib = xl;
    work.translateFn = xlatFn;
    work.dir = userDir.fileSystemRepresentation;
    work.sha = cacheKey.UTF8String;
    work.entryAir = (const uint8_t *)bc.bytes;
    work.entryAirLen = bc.length;
    work.local[0] = local[0]; work.local[1] = local[1]; work.local[2] = local[2];
    work.deps = inDeps; work.ndeps = nd;
    work.dynlibs = inLibs; work.ndynlibs = nl;
    work.caps = rdna4_query_caps();
    if (emulatedPayload) work.caps.flags |= GARNETX_CAP_MESH_EMULATED_PAYLOAD;
    if (RDNA4R.shaderInt64) work.caps.flags |= GARNETX_CAP_NATIVE_INT64;

    int joinRc = rdna4x_run_on_worker_stack(rdna4_mesh_s3_worker, &work);
    free(inDeps); free(inLibs);
    if (joinRc != 0 || work.rc != RDNA4X_OK || !work.outSpv || work.outSpvLen < 20 || (work.outSpvLen & 3) ||
        !work.outMeta || !work.outMetaLen) {
        free(work.outSpv); free(work.outMeta);
        if (err) {
            *err = rdna4_err(42, [NSString stringWithFormat:@"%s '%@' translation failed: %s",
                                  role, fname, work.err[0] ? work.err : "stage 3 worker failed"]);
        }
        return NO;
    }

    NSData *spvObj = [NSData dataWithBytesNoCopy:work.outSpv length:work.outSpvLen freeWhenDone:YES];
    NSData *metaData = [NSData dataWithBytesNoCopy:work.outMeta length:work.outMetaLen freeWhenDone:YES];
    id obj = [NSJSONSerialization JSONObjectWithData:metaData options:0 error:NULL];
    if (![obj isKindOfClass:[NSDictionary class]]) {
        if (err) *err = rdna4_err(42, [NSString stringWithFormat:@"%s '%@' produced non-object reflection JSON", role, fname]);
        return NO;
    }
    rdna4mesh_refl_t parsed = {0};
    rdna4mesh_status_t pst = rdna4mesh_parse_reflection_json((const char *)metaData.bytes, metaData.length, &parsed);
    if (pst != RDNA4MESH_OK) {
        if (err) *err = rdna4_err(42, [NSString stringWithFormat:@"%s '%@' reflection JSON invalid: %s",
                                       role, fname, rdna4mesh_status_str(pst)]);
        return NO;
    }
    if (outSpv) *outSpv = spvObj;
    if (outMeta) *outMeta = (NSDictionary *)obj;
    if (outRefl) *outRefl = parsed;
    return YES;
}

// Build a stage descriptor set layout reusing rdna4_stage_dsl for user bindings while cleanly
// isolating set-0 emulated Object+Mesh SSBO bindings 673/674/675 (0078 ABI).
static BOOL rdna4_mesh_build_stage_dsl(const RDNA4sRefl *r, int set, VkShaderStageFlags stage,
                                       NSDictionary *meta, const rdna4mesh_refl_t *mrefl,
                                       BOOL allowEmulated, uint32_t *emulMaskOut,
                                       VkDescriptorSetLayout *dslOut, RDNA4PB **pbOut,
                                       uint32_t *npbOut, NSMutableArray *ownedSamplers,
                                       NSError **err) {
    if (emulMaskOut) *emulMaskOut = 0u;
    *dslOut = VK_NULL_HANDLE;
    *pbOut = NULL;
    *npbOut = 0u;

    uint32_t emulMask = 0u;
    RDNA4sRefl *userR = NULL;
    const RDNA4sRefl *passR = r;
    if (r && allowEmulated && set == 0) {
        userR = (RDNA4sRefl *)malloc(sizeof *userR);
        if (!userR) {
            if (err) *err = rdna4_err(49, @"out of memory copying mesh reflection for emulated DSL");
            return NO;
        }
        *userR = *r;
        const struct { uint32_t binding; uint32_t bit; } slots[3] = {
            { GARNETX_MESH_GRID_PARAMS_BINDING, RDNA4MESH_EMUL_BIT_GRID_PARAMS },
            { GARNETX_MESH_PAYLOAD_BINDING,     RDNA4MESH_EMUL_BIT_PAYLOAD },
            { GARNETX_MESH_INDIRECT_BINDING,    RDNA4MESH_EMUL_BIT_INDIRECT },
        };
        for (size_t i = 0; i < 3u; i++) {
            const RDNA4sBind *bd = &r->bind[0][slots[i].binding];
            if (!bd->used) continue;
            if (bd->type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || bd->count != 1u || bd->runtime) {
                free(userR);
                if (err) *err = rdna4_err(49, [NSString stringWithFormat:@"emulated mesh binding %u must be a single StorageBuffer", slots[i].binding]);
                return NO;
            }
            if (slots[i].binding == GARNETX_MESH_PAYLOAD_BINDING && mrefl && mrefl->has_sampler_coord_flags) {
                free(userR);
                if (err) *err = rdna4_err(49, @"emulated mesh payload binding 674 conflicts with SamplerCoordinateFlags");
                return NO;
            }
            if (slots[i].binding == GARNETX_MESH_GRID_PARAMS_BINDING && mrefl && mrefl->has_argument_tables) {
                free(userR);
                if (err) *err = rdna4_err(49, @"emulated mesh grid-params binding 673 conflicts with GPU argument tables");
                return NO;
            }
            emulMask |= slots[i].bit;
            userR->bind[0][slots[i].binding].used = 0;
        }
        passR = userR;
    }

    VkDescriptorSetLayout userDsl = VK_NULL_HANDLE;
    BOOL okDsl = rdna4_stage_dsl(passR, set, stage, meta, &userDsl, pbOut, npbOut, ownedSamplers, err);
    free(userR);
    if (!okDsl)
        return NO;

    if (!emulMask) {
        *dslOut = userDsl;
        if (emulMaskOut) *emulMaskOut = 0u;
        return YES;
    }

    vkDestroyDescriptorSetLayout(RDNA4R.dev, userDsl, NULL);
    uint32_t nb = *npbOut;
    VkDescriptorSetLayoutBinding *bnd = calloc((size_t)nb + 3u, sizeof *bnd);
    if (!bnd) {
        free(*pbOut); *pbOut = NULL; *npbOut = 0u;
        if (err) *err = rdna4_err(49, @"out of memory building emulated mesh descriptor set layout");
        return NO;
    }
    uint32_t pos = 0u;
    for (uint32_t i = 0; i < nb; i++) {
        bnd[pos++] = (VkDescriptorSetLayoutBinding){ (*pbOut)[i].binding, (*pbOut)[i].type, (*pbOut)[i].count, stage, NULL };
    }
    if (emulMask & RDNA4MESH_EMUL_BIT_GRID_PARAMS)
        bnd[pos++] = (VkDescriptorSetLayoutBinding){ GARNETX_MESH_GRID_PARAMS_BINDING, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u, stage, NULL };
    if (emulMask & RDNA4MESH_EMUL_BIT_PAYLOAD)
        bnd[pos++] = (VkDescriptorSetLayoutBinding){ GARNETX_MESH_PAYLOAD_BINDING, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u, stage, NULL };
    if (emulMask & RDNA4MESH_EMUL_BIT_INDIRECT)
        bnd[pos++] = (VkDescriptorSetLayoutBinding){ GARNETX_MESH_INDIRECT_BINDING, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u, stage, NULL };

    VkDescriptorSetLayoutCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = pos,
        .pBindings = bnd,
    };
    VkResult vr = vkCreateDescriptorSetLayout(RDNA4R.dev, &ci, NULL, dslOut);
    free(bnd);
    if (vr != VK_SUCCESS) {
        free(*pbOut); *pbOut = NULL; *npbOut = 0u;
        if (err) *err = rdna4_err(50, [NSString stringWithFormat:@"vkCreateDescriptorSetLayout(mesh emul) = %d", vr]);
        return NO;
    }
    if (emulMaskOut) *emulMaskOut = emulMask;
    return YES;
}

// =================================================================================================
// 4. RDNA4MeshRenderPipelineState (subclass of RDNA4RenderPipelineState)
// =================================================================================================

#ifndef RDNA4_HAS_MESH_PIPELINE_QUERY

@implementation RDNA4RenderPipelineState (RDNA4MeshQuery)
- (BOOL)rdna4IsMeshPipeline { return NO; }
@end
#endif


@implementation RDNA4MeshRenderPipelineState

+ (void)initialize {
    if (self == [RDNA4MeshRenderPipelineState class]) {
        const char *n[] = { "MTLRenderPipelineState" };
        rdna4_add_protocols(self, n, 1, "RDNA4MeshRenderPipelineState");
    }
}

- (BOOL)rdna4IsMeshPipeline { return YES; }
- (rdna4mesh_exec_mode_t)rdna4MeshExecMode { return _meshExecMode; }
- (BOOL)rdna4HasObjectStage { return _hasObject; }
- (const rdna4mesh_linked_info_t *)rdna4MeshLinkedInfo { return &_linkedInfo; }
- (const rdna4mesh_refl_t *)rdna4ObjRefl { return &_objRefl; }
- (const rdna4mesh_refl_t *)rdna4MeshRefl { return &_meshRefl; }
- (uint32_t)rdna4ObjectSet { return _objSet; }
- (VkDescriptorSetLayout)rdna4ObjectDsl { return _dslObj; }
- (VkDescriptorSetLayout)rdna4ObjectComputeDsl { return _objCompDsl; }
- (VkPipelineLayout)rdna4ObjectComputeLayout { return _objCompPl; }
- (const RDNA4PB *)rdna4PbO { return _pbO; }
- (uint32_t)rdna4NpbO { return _npbO; }
- (const RDNA4PB *)rdna4PbM { return _pbM; }
- (uint32_t)rdna4NpbM { return _npbM; }
- (uint32_t)rdna4EmulObjMask { return _emulObjMask; }
- (uint32_t)rdna4EmulMeshMask { return _emulMeshMask; }
- (BOOL)rdna4ObjMultExecWidth { return _objMultExecW; }
- (BOOL)rdna4MeshMultExecWidth { return _meshMultExecW; }
- (NSUInteger)maxTotalThreadsPerObjectThreadgroup { return (NSUInteger)_maxObjThreads; }
- (NSUInteger)maxTotalThreadsPerMeshThreadgroup { return (NSUInteger)_maxMeshThreads; }
- (NSUInteger)objectThreadExecutionWidth { return (NSUInteger)_objExecWidth; }
- (NSUInteger)meshThreadExecutionWidth { return (NSUInteger)_meshExecWidth; }
- (VkPipeline)vkPipeline {
    return [self rdna4MeshPipelineForCull:VK_CULL_MODE_NONE front:VK_FRONT_FACE_CLOCKWISE
                                       ds:[self rdna4DSKey:NULL] rflags:0
                                 objLocal:_initObjLocal meshLocal:_initMeshLocal error:NULL];
}

// Footnote 10: mesh render pipelines never expose function handles / VFT / IFT.
- (id<MTLFunctionHandle>)functionHandleWithFunction:(id<MTLFunction>)function stage:(MTLRenderStages)stage {
    (void)function; (void)stage;
    return nil;
}
- (id<MTLVisibleFunctionTable>)newVisibleFunctionTableWithDescriptor:(MTLVisibleFunctionTableDescriptor *)descriptor stage:(MTLRenderStages)stage {
    (void)descriptor; (void)stage;
    return nil;
}
- (id<MTLIntersectionFunctionTable>)newIntersectionFunctionTableWithDescriptor:(MTLIntersectionFunctionTableDescriptor *)descriptor stage:(MTLRenderStages)stage {
    (void)descriptor; (void)stage;
    return nil;
}

- (VkShaderModule)rdna4ShaderModuleForObjectLocalLocked:(const uint32_t[3])local error:(NSError **)err {
    if (local[0] == _initObjLocal[0] && local[1] == _initObjLocal[1] && local[2] == _initObjLocal[2])
        return _objMod;
    NSString *k = [NSString stringWithFormat:@"%u,%u,%u", local[0], local[1], local[2]];
    NSValue *cached = _objModCache[k];
    if (cached) return (VkShaderModule)cached.pointerValue;

    BOOL emul = (_meshExecMode == RDNA4MESH_MODE_EMULATED_OBJECT_MESH);
    NSData *osd = nil; NSDictionary *mo = nil; rdna4mesh_refl_t r = {0};
    if (!rdna4_mesh_translate_stage(_objFn, "object", emul, local, _objDeps, _objDynlibs,
                                    &osd, &mo, &r, err))
        return VK_NULL_HANDLE;
    rdna4mesh_spv_info_t info = {0};
    if (rdna4mesh_inspect_spv((const uint32_t *)osd.bytes, osd.length / 4u, &info) != RDNA4MESH_OK ||
        info.ep_model != (emul ? RDNA4MESH_SPV_EM_GLCOMPUTE : RDNA4MESH_SPV_EM_TASK_EXT)) {
        if (err) *err = rdna4_err(51, @"specialized object SPIR-V has unexpected execution model");
        return VK_NULL_HANDLE;
    }
    NSData *useSpv = emul ? osd : rdna4_spv_set(osd, _objSet);
    VkShaderModule m = rdna4_spv_module(useSpv.bytes, useSpv.length, "object-spec", err);
    if (!m) return VK_NULL_HANDLE;
    _objModCache[k] = [NSValue valueWithPointer:(void *)m];
    return m;
}

- (VkShaderModule)rdna4ShaderModuleForMeshLocalLocked:(const uint32_t[3])local error:(NSError **)err {
    if (local[0] == _initMeshLocal[0] && local[1] == _initMeshLocal[1] && local[2] == _initMeshLocal[2])
        return _meshMod;
    NSString *k = [NSString stringWithFormat:@"%u,%u,%u", local[0], local[1], local[2]];
    NSValue *cached = _meshModCache[k];
    if (cached) return (VkShaderModule)cached.pointerValue;

    BOOL emul = (_meshExecMode == RDNA4MESH_MODE_EMULATED_OBJECT_MESH);
    NSData *msd = nil; NSDictionary *mm = nil; rdna4mesh_refl_t r = {0};
    if (!rdna4_mesh_translate_stage(_meshFn, "mesh", emul, local, _meshDeps, _meshDynlibs,
                                    &msd, &mm, &r, err))
        return VK_NULL_HANDLE;
    rdna4mesh_spv_info_t info = {0};
    if (rdna4mesh_inspect_spv((const uint32_t *)msd.bytes, msd.length / 4u, &info) != RDNA4MESH_OK ||
        info.ep_model != RDNA4MESH_SPV_EM_MESH_EXT) {
        if (err) *err = rdna4_err(51, @"specialized mesh SPIR-V is not MeshEXT");
        return VK_NULL_HANDLE;
    }
    VkShaderModule m = rdna4_spv_module(msd.bytes, msd.length, "mesh-spec", err);
    if (!m) return VK_NULL_HANDLE;
    _meshModCache[k] = [NSValue valueWithPointer:(void *)m];
    return m;
}

- (VkPipeline)rdna4BuildObjectComputeLocked:(const uint32_t[3])objLocal error:(NSError **)err {
    VkShaderModule mod = [self rdna4ShaderModuleForObjectLocalLocked:objLocal error:err];
    if (!mod) return VK_NULL_HANDLE;
    VkComputePipelineCreateInfo cp = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = mod,
            .pName = _oep[0] ? _oep : "main",
        },
        .layout = _objCompPl,
    };
    VkPipelineCache pc = RDNA4R.pc;
#ifdef RDNA4_BINARY_ARCHIVE_IMPL_INC
    pc = rdna4_ba_vkcache();
#endif
    VkPipeline p = VK_NULL_HANDLE;
    uint64_t t0 = rdna4_now();
    VkResult vr = vkCreateComputePipelines(RDNA4R.dev, pc, 1, &cp, NULL, &p);
    rdna4t_add(&T1.cmp, rdna4_now() - t0);
    atomic_fetch_add(&rdna4_pipes_new, 1);
    if (vr != VK_SUCCESS) {
        if (err) *err = rdna4_err(47, [NSString stringWithFormat:@"vkCreateComputePipelines(object-emul) = %d", vr]);
        return VK_NULL_HANDLE;
    }
    return p;
}

- (VkPipeline)rdna4ObjectComputePipelineForLocal:(const uint32_t[3])objLocal error:(NSError **)err {
    if (_meshExecMode != RDNA4MESH_MODE_EMULATED_OBJECT_MESH || !_objCompPl) {
        if (err) *err = rdna4_err(47, @"object compute pipeline requested on non-emulated mesh pipeline");
        return VK_NULL_HANDLE;
    }
    NSString *k = [NSString stringWithFormat:@"%u,%u,%u", objLocal[0], objLocal[1], objLocal[2]];
    uint64_t compileNs = 0; BOOL builtNew = NO;
    [_lock lock];
    NSValue *v = _objCompVariants[k];
    if (!v) {
        uint64_t bt0 = rdna4_now();
        VkPipeline p = [self rdna4BuildObjectComputeLocked:objLocal error:err];
        compileNs = rdna4_now() - bt0;
        if (p) {
            builtNew = YES;
            v = [NSValue valueWithPointer:(void *)p];
            _objCompVariants[k] = v;
        }
    }
    [_lock unlock];
    if (builtNew) rdna4_pc_maybe_save_after_compile(compileNs, "object-compute");
    return v ? (VkPipeline)v.pointerValue : VK_NULL_HANDLE;
}

- (VkPipeline)rdna4BuildMeshGfxLocked:(VkCullModeFlags)cull front:(VkFrontFace)front
                                   ds:(uint32_t)dk rflags:(uint32_t)rflags
                             objLocal:(const uint32_t[3])objLocal
                            meshLocal:(const uint32_t[3])meshLocal
                                error:(NSError **)err {
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

    VkPipelineShaderStageCreateInfo st[3];
    uint32_t stCount = 0u;
    if (_meshExecMode == RDNA4MESH_MODE_NATIVE_TASK_MESH) {
        VkShaderModule oMod = [self rdna4ShaderModuleForObjectLocalLocked:objLocal error:err];
        if (!oMod) return VK_NULL_HANDLE;
        st[stCount++] = (VkPipelineShaderStageCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_TASK_BIT_EXT,
            .module = oMod,
            .pName = _oep[0] ? _oep : "main",
        };
    }
    VkShaderModule mMod = [self rdna4ShaderModuleForMeshLocalLocked:meshLocal error:err];
    if (!mMod) return VK_NULL_HANDLE;
    st[stCount++] = (VkPipelineShaderStageCreateInfo){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_MESH_BIT_EXT,
        .module = mMod,
        .pName = _mep[0] ? _mep : "main",
    };
    if (_hasFrag && _fs) {
        st[stCount++] = (VkPipelineShaderStageCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = _fs,
            .pName = _fep[0] ? _fep : "main",
        };
    }

    VkPipelineViewportStateCreateInfo vps = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = vpCount,
        .scissorCount = vpCount,
    };
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable = (depthClamp && RDNA4R.depthClamp) ? VK_TRUE : VK_FALSE,
        .polygonMode = fillLine ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL,
        .rasterizerDiscardEnable = _rastOff ? VK_TRUE : VK_FALSE,
        .cullMode = cull,
        .frontFace = front,
        .lineWidth = 1.0f,
        .depthBiasEnable = _pc.depthBias ? VK_TRUE : VK_FALSE,
    };
    VkSampleLocationEXT defSL[8];
    for (uint32_t s = 0; s < _pc.samples && s < 8; s++) defSL[s] = (VkSampleLocationEXT){ 0.5f, 0.5f };
    VkPipelineSampleLocationsStateCreateInfoEXT psli = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SAMPLE_LOCATIONS_STATE_CREATE_INFO_EXT,
        .sampleLocationsEnable = VK_TRUE,
        .sampleLocationsInfo = {
            .sType = VK_STRUCTURE_TYPE_SAMPLE_LOCATIONS_INFO_EXT,
            .sampleLocationsPerPixel = (VkSampleCountFlagBits)_pc.samples,
            .sampleLocationGridSize = { 1, 1 },
            .sampleLocationsCount = _pc.samples,
            .pSampleLocations = defSL,
        },
    };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .pNext = sampleLoc ? &psli : NULL,
        .rasterizationSamples = (VkSampleCountFlagBits)_pc.samples,
        .alphaToCoverageEnable = _pc.alphaToCoverage ? VK_TRUE : VK_FALSE,
        .alphaToOneEnable = _alphaToOne ? VK_TRUE : VK_FALSE,
    };
    rdna4ds_vk_t dv; rdna4ds_decode(dk, &dv);
    VkPipelineDepthStencilStateCreateInfo dss = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = dv.depthTest ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = dv.depthWrite ? VK_TRUE : VK_FALSE,
        .depthCompareOp = (VkCompareOp)dv.depthCompareOp,
        .stencilTestEnable = dv.stencilTest ? VK_TRUE : VK_FALSE,
        .front = { (VkStencilOp)dv.front.failOp, (VkStencilOp)dv.front.passOp, (VkStencilOp)dv.front.depthFailOp, (VkCompareOp)dv.front.compareOp, 0, 0, 0 },
        .back  = { (VkStencilOp)dv.back.failOp,  (VkStencilOp)dv.back.passOp,  (VkStencilOp)dv.back.depthFailOp,  (VkCompareOp)dv.back.compareOp,  0, 0, 0 },
    };
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .flags = _fetch && RDNA4R.orderedColorAccess
            ? VK_PIPELINE_COLOR_BLEND_STATE_CREATE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_BIT_EXT : 0,
        .attachmentCount = _na,
        .pAttachments = _cba,
    };
    VkDynamicState dyn[9]; uint32_t ndyn = 0u;
    for (unsigned i = 0; i < _pc.ndyn && i < 8; i++) dyn[ndyn++] = (VkDynamicState)_pc.dyn[i];
    if (sampleLoc) dyn[ndyn++] = VK_DYNAMIC_STATE_SAMPLE_LOCATIONS_EXT;
    VkPipelineDynamicStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = ndyn,
        .pDynamicStates = dyn,
    };
    // VK_EXT_mesh_shader VUIDs: pVertexInputState, pInputAssemblyState, and pTessellationState MUST be NULL.
    VkGraphicsPipelineCreateInfo gpc = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = stCount,
        .pStages = st,
        .pVertexInputState = NULL,
        .pInputAssemblyState = NULL,
        .pTessellationState = NULL,
        .pViewportState = &vps,
        .pRasterizationState = &rs,
        .pMultisampleState = &ms,
        .pDepthStencilState = _pc.hasDS ? &dss : NULL,
        .pColorBlendState = &cb,
        .pDynamicState = &ds,
        .layout = _pl,
        .renderPass = _rp,
    };
    VkPipelineCache pc = RDNA4R.pc;
#ifdef RDNA4_BINARY_ARCHIVE_IMPL_INC
    pc = rdna4_ba_vkcache();
#endif
    VkPipeline p = VK_NULL_HANDLE;
    uint64_t t0 = rdna4_now();
    VkResult vr = vkCreateGraphicsPipelines(RDNA4R.dev, pc, 1, &gpc, NULL, &p);
    rdna4t_add(&T1.gfx, rdna4_now() - t0);
    atomic_fetch_add(&rdna4_pipes_new, 1);
    if (vr != VK_SUCCESS) {
        if (err) *err = rdna4_err(47, [NSString stringWithFormat:@"vkCreateGraphicsPipelines(mesh) = %d", vr]);
        return VK_NULL_HANDLE;
    }
    return p;
}

- (VkPipeline)rdna4MeshPipelineForCull:(VkCullModeFlags)cull front:(VkFrontFace)front
                                    ds:(uint32_t)dk rflags:(uint32_t)rflags
                              objLocal:(const uint32_t[3])objLocal
                             meshLocal:(const uint32_t[3])meshLocal
                                 error:(NSError **)err {
    uint32_t ol[3] = { 0u, 0u, 0u };
    if (_meshExecMode == RDNA4MESH_MODE_NATIVE_TASK_MESH && objLocal) {
        ol[0] = objLocal[0]; ol[1] = objLocal[1]; ol[2] = objLocal[2];
    }
    NSString *k = [NSString stringWithFormat:@"%u:%u:%u:%u:%u,%u,%u:%u,%u,%u",
                   (unsigned)cull, (unsigned)front, dk, rflags,
                   ol[0], ol[1], ol[2], meshLocal[0], meshLocal[1], meshLocal[2]];
    uint64_t compileNs = 0; BOOL builtNew = NO;
    [_lock lock];
    NSValue *v = _meshVariants[k];
    if (!v) {
        uint64_t bt0 = rdna4_now();
        VkPipeline p = [self rdna4BuildMeshGfxLocked:cull front:front ds:dk rflags:rflags
                                            objLocal:ol meshLocal:meshLocal error:err];
        compileNs = rdna4_now() - bt0;
        if (p) {
            builtNew = YES;
            v = [NSValue valueWithPointer:(void *)p];
            _meshVariants[k] = v;
        }
    }
    [_lock unlock];
    if (builtNew) rdna4_pc_maybe_save_after_compile(compileNs, "mesh-pipeline");
    return v ? (VkPipeline)v.pointerValue : VK_NULL_HANDLE;
}

- (instancetype)initWithDevice:(id)dev meshDescriptor:(id)d options:(NSUInteger)options error:(NSError **)err {
    (void)options;
    self = [super init];
    if (!self) return nil;
    _dev = dev;
    _lock = [NSLock new];
    _variants = [NSMutableDictionary dictionary];
    _icbVariants = [NSMutableDictionary dictionary];
    _owned = [NSMutableArray array];
    _meshVariants = [NSMutableDictionary dictionary];
    _objCompVariants = [NSMutableDictionary dictionary];
    _objModCache = [NSMutableDictionary dictionary];
    _meshModCache = [NSMutableDictionary dictionary];
    if (!d) {
        if (err) *err = rdna4_err(40, @"mesh pipeline descriptor is nil");
        return nil;
    }

    NSString *lbl = rdna4x_prop(d, "label");
    if ([lbl isKindOfClass:[NSString class]]) _lbl = [lbl copy];
    id objFn = rdna4x_prop(d, "objectFunction");
    id meshFn = rdna4x_prop(d, "meshFunction");
    id fragFn = rdna4x_prop(d, "fragmentFunction");
    _hasObject = (objFn != nil);
    _hasFrag = (fragFn != nil);
    _rastOff = [d respondsToSelector:sel_registerName("isRasterizationEnabled")]
        ? !rdna4x_bool_prop(d, "isRasterizationEnabled") : NO;
    _alphaToOne = rdna4x_bool_prop(d, "isAlphaToOneEnabled");
    _icb = rdna4x_bool_prop(d, "supportIndirectCommandBuffers");
    _objFn = objFn;
    _meshFn = meshFn;

    if (!rdna4_radv_open(err)) return nil;
    if (!rdna4_mesh_resolve_vk_funcs()) {
        if (err) *err = rdna4_err(49, @"backend does not expose VK_EXT_mesh_shader command entry points");
        return nil;
    }

    const rdna4dc_caps caps = rdna4_device_caps();
    rdna4mesh_desc_t mdesc;
    memset(&mdesc, 0, sizeof mdesc);
    mdesc.has_object_function = _hasObject ? 1 : 0;
    mdesc.has_mesh_function = meshFn ? 1 : 0;
    mdesc.has_fragment_function = _hasFrag ? 1 : 0;
    mdesc.payload_memory_length = (uint64_t)rdna4_mesh_uint_prop(d, "payloadMemoryLength", 0u);
    mdesc.max_total_threadgroups_per_mesh_grid = (uint64_t)rdna4_mesh_uint_prop(d, "maxTotalThreadgroupsPerMeshGrid", 0u);
    mdesc.max_total_threads_per_object_threadgroup = (uint64_t)rdna4_mesh_uint_prop(d, "maxTotalThreadsPerObjectThreadgroup", 0u);
    mdesc.max_total_threads_per_mesh_threadgroup = (uint64_t)rdna4_mesh_uint_prop(d, "maxTotalThreadsPerMeshThreadgroup", 0u);
    mdesc.object_tg_size_mult_exec_width = rdna4x_bool_prop(d, "objectThreadgroupSizeIsMultipleOfThreadExecutionWidth") ? 1 : 0;
    mdesc.mesh_tg_size_mult_exec_width = rdna4x_bool_prop(d, "meshThreadgroupSizeIsMultipleOfThreadExecutionWidth") ? 1 : 0;
    mdesc.max_vertex_amplification_count = (uint64_t)rdna4_mesh_uint_prop(d, "maxVertexAmplificationCount", 1u);
    mdesc.footnote10_flags = rdna4_mesh_desc_fn10_flags(d);

    const char *why = NULL;
    rdna4mesh_exec_mode_t execMode = RDNA4MESH_MODE_UNSUPPORTED;
    rdna4mesh_status_t dst = rdna4mesh_validate_desc(&mdesc, &caps, 1u, &execMode, &why);
    if (dst != RDNA4MESH_OK) {
        if (err) *err = rdna4_err(49, [NSString stringWithUTF8String:why ? why : rdna4mesh_status_str(dst)]);
        return nil;
    }
    _meshExecMode = execMode;
    _maxAmp = (uint32_t)(mdesc.max_vertex_amplification_count ? mdesc.max_vertex_amplification_count : 1u);
    _objExecWidth = rdna4dc_thread_execution_width(&caps);
    _meshExecWidth = rdna4dc_thread_execution_width(&caps);
    _objMultExecW = mdesc.object_tg_size_mult_exec_width ? YES : NO;
    _meshMultExecW = mdesc.mesh_tg_size_mult_exec_width ? YES : NO;

    uint32_t hwObjInv = rdna4mesh_effective_obj_max_invocations(&caps, execMode);
    uint32_t hwMeshInv = rdna4mesh_effective_mesh_max_invocations(&caps);
    _maxObjThreads = _hasObject
        ? (mdesc.max_total_threads_per_object_threadgroup ? (uint32_t)mdesc.max_total_threads_per_object_threadgroup : hwObjInv)
        : 0u;
    _maxMeshThreads = mdesc.max_total_threads_per_mesh_threadgroup
        ? (uint32_t)mdesc.max_total_threads_per_mesh_threadgroup : hwMeshInv;

    // Collect allowed MTLLinkedFunctions.privateFunctions + preloadedLibraries for each stage.
    NSArray<NSDictionary *> *objDeps = @[], *objLibs = @[], *objVis = @[];
    NSArray<NSDictionary *> *meshDeps = @[], *meshLibs = @[], *meshVis = @[];
    NSArray<NSDictionary *> *fragDeps = @[], *fragLibs = @[], *fragVis = @[];
    NSString *linkWhy = nil;
    if (_hasObject && !rdna4x_stage2_link_collect(rdna4x_prop(d, "objectLinkedFunctions"),
                                                  rdna4x_prop(d, "objectPreloadedLibraries"),
                                                  &objDeps, &objVis, &objLibs, &linkWhy)) {
        if (err) *err = rdna4_err(42, [NSString stringWithFormat:@"object linkage invalid: %@", linkWhy ?: @"unknown"]);
        return nil;
    }
    if (!rdna4x_stage2_link_collect(rdna4x_prop(d, "meshLinkedFunctions"),
                                    rdna4x_prop(d, "meshPreloadedLibraries"),
                                    &meshDeps, &meshVis, &meshLibs, &linkWhy)) {
        if (err) *err = rdna4_err(42, [NSString stringWithFormat:@"mesh linkage invalid: %@", linkWhy ?: @"unknown"]);
        return nil;
    }
    if (_hasFrag && !rdna4x_stage2_link_collect(rdna4x_prop(d, "fragmentLinkedFunctions"),
                                                rdna4x_prop(d, "fragmentPreloadedLibraries"),
                                                &fragDeps, &fragVis, &fragLibs, &linkWhy)) {
        if (err) *err = rdna4_err(42, [NSString stringWithFormat:@"fragment linkage invalid: %@", linkWhy ?: @"unknown"]);
        return nil;
    }
    (void)objVis; (void)meshVis; (void)fragVis;
    _objDeps = objDeps; _objDynlibs = objLibs;
    _meshDeps = meshDeps; _meshDynlibs = meshLibs;

    // Pick legal initial LocalSize for compilation; retry with (1,1,1) if shader declares a tighter air.max_work_group_size.
    rdna4mesh_initial_local_size(_maxObjThreads, hwObjInv, caps.workgroup_size[0], _objExecWidth, _objMultExecW ? 1 : 0, _initObjLocal);
    rdna4mesh_initial_local_size(_maxMeshThreads, hwMeshInv, caps.workgroup_size[0], _meshExecWidth, _meshMultExecW ? 1 : 0, _initMeshLocal);

    const BOOL emul = (execMode == RDNA4MESH_MODE_EMULATED_OBJECT_MESH);
    NSData *osd = nil, *msd = nil, *fsd = nil;
    NSDictionary *mo_ = nil, *mm_ = nil, *mf_ = nil, *preF = nil;
    NSError *te = nil;

    if (_hasObject) {
        if (!rdna4_mesh_translate_stage(objFn, "object", emul, _initObjLocal, objDeps, objLibs,
                                        &osd, &mo_, &_objRefl, &te)) {
            if (te && [te.localizedDescription containsString:@"air.max_work_group_size"] &&
                (_initObjLocal[0] != 1u || _initObjLocal[1] != 1u || _initObjLocal[2] != 1u)) {
                _initObjLocal[0] = _initObjLocal[1] = _initObjLocal[2] = 1u;
                te = nil;
                if (!rdna4_mesh_translate_stage(objFn, "object", emul, _initObjLocal, objDeps, objLibs,
                                                &osd, &mo_, &_objRefl, &te)) {
                    if (err) *err = te;
                    return nil;
                }
            } else {
                if (err) *err = te;
                return nil;
            }
        }
    }
    if (!rdna4_mesh_translate_stage(meshFn, "mesh", emul, _initMeshLocal, meshDeps, meshLibs,
                                    &msd, &mm_, &_meshRefl, &te)) {
        if (te && [te.localizedDescription containsString:@"air.max_work_group_size"] &&
            (_initMeshLocal[0] != 1u || _initMeshLocal[1] != 1u || _initMeshLocal[2] != 1u)) {
            _initMeshLocal[0] = _initMeshLocal[1] = _initMeshLocal[2] = 1u;
            te = nil;
            if (!rdna4_mesh_translate_stage(meshFn, "mesh", emul, _initMeshLocal, meshDeps, meshLibs,
                                            &msd, &mm_, &_meshRefl, &te)) {
                if (err) *err = te;
                return nil;
            }
        } else {
            if (err) *err = te;
            return nil;
        }
    }
    if (_hasFrag) {
        const uint32_t fragLocal[3] = { 1u, 1u, 1u };
        if (!rdna4_mesh_translate_stage(fragFn, "fragment", NO, fragLocal, fragDeps, fragLibs,
                                        &fsd, &mf_, &_fragRefl, &te)) {
            if (err) *err = te;
            return nil;
        }
        preF = mf_;
        fsd = rdna4_gpu_argument_spv(fsd, &mf_, rdna4_now() + 3000000000ull, err);
        if (!fsd) return nil;
        if (mf_ != preF && !rdna4_mesh_parse_meta_dict(mf_, &_fragRefl, err)) return nil;
    }

    // Inspect SPIR-V execution models, mesh modes, and Footnote 10 ray-tracing capabilities.
    rdna4mesh_spv_info_t objSpvInfo = {0}, meshSpvInfo = {0}, fragSpvInfo = {0};
    if (_hasObject && rdna4mesh_inspect_spv((const uint32_t *)osd.bytes, osd.length / 4u, &objSpvInfo) != RDNA4MESH_OK) {
        if (err) *err = rdna4_err(51, @"object SPIR-V header/instruction inspection failed");
        return nil;
    }
    if (rdna4mesh_inspect_spv((const uint32_t *)msd.bytes, msd.length / 4u, &meshSpvInfo) != RDNA4MESH_OK) {
        if (err) *err = rdna4_err(51, @"mesh SPIR-V header/instruction inspection failed");
        return nil;
    }
    if (_hasFrag && rdna4mesh_inspect_spv((const uint32_t *)fsd.bytes, fsd.length / 4u, &fragSpvInfo) != RDNA4MESH_OK) {
        if (err) *err = rdna4_err(52, @"fragment SPIR-V header/instruction inspection failed");
        return nil;
    }

    rdna4mesh_status_t lst = rdna4mesh_validate_stage_linkage(
        execMode, &caps, &mdesc,
        _hasObject ? &_objRefl : NULL, _hasObject ? &objSpvInfo : NULL,
        &_meshRefl, &meshSpvInfo,
        _hasFrag ? &_fragRefl : NULL, _hasFrag ? &fragSpvInfo : NULL,
        &_linkedInfo, &why);
    if (lst != RDNA4MESH_OK) {
        if (err) *err = rdna4_err(49, [NSString stringWithUTF8String:why ? why : rdna4mesh_status_str(lst)]);
        return nil;
    }

    // Reflect SPIR-V descriptors for Object, Mesh, and Fragment stages.
    NSData *fsp = _hasFrag ? rdna4_spv_set(fsd, 1u) : nil;
    if (_hasFrag) {
        uint64_t perPrimMask = (_meshRefl.has_mesh ? _meshRefl.mesh.primitive_out_mask : 0u) |
                               meshSpvInfo.per_prim_out_mask;
        if (perPrimMask != 0u) {
            uint32_t *patched = NULL;
            size_t patchedWords = 0u;
            rdna4mesh_status_t pst = rdna4_mesh_patch_frag_per_primitive(
                (const uint32_t *)fsp.bytes, fsp.length / 4u, perPrimMask,
                &patched, &patchedWords);
            if (pst != RDNA4MESH_OK) {
                if (err) *err = rdna4_err(52, [NSString stringWithFormat:@"fragment SPIR-V PerPrimitiveEXT pairing patch failed: %s",
                                               rdna4mesh_status_str(pst)]);
                return nil;
            }
            if (patched) {
                fsp = [NSData dataWithBytesNoCopy:patched length:patchedWords * 4u freeWhenDone:YES];
            }
        }
    }
    RDNA4sRefl *ro = _hasObject ? calloc(1, sizeof *ro) : NULL;
    RDNA4sRefl *rm = calloc(1, sizeof *rm);
    RDNA4sRefl *rf = _hasFrag ? calloc(1, sizeof *rf) : NULL;
    RDNA4sMod moMod = {0}, mmMod = {0}, mfMod = {0};
    char swhy[256] = "";
    BOOL bad = (!rm || (_hasObject && !ro) || (_hasFrag && !rf));
    if (bad && err) *err = rdna4_err(49, @"out of memory allocating mesh SPIR-V reflection state");

    if (!bad && _hasObject) {
        if (rdna4s_parse((const uint32_t *)osd.bytes, osd.length / 4u, &moMod, swhy, sizeof swhy) ||
            rdna4s_reflect(&moMod, 2, ro)) {
            if (err) *err = rdna4_err(51, [NSString stringWithFormat:@"object SPIR-V reflection: %s", swhy[0] ? swhy : ro->why]);
            bad = YES;
        } else {
            strlcpy(_oep, moMod.ep_name[0] ? moMod.ep_name : "main", sizeof _oep);
        }
    }
    if (!bad) {
        if (rdna4s_parse((const uint32_t *)msd.bytes, msd.length / 4u, &mmMod, swhy, sizeof swhy) ||
            rdna4s_reflect(&mmMod, 2, rm)) {
            if (err) *err = rdna4_err(51, [NSString stringWithFormat:@"mesh SPIR-V reflection: %s", swhy[0] ? swhy : rm->why]);
            bad = YES;
        } else {
            strlcpy(_mep, mmMod.ep_name[0] ? mmMod.ep_name : "main", sizeof _mep);
        }
    }
    if (!bad && _hasFrag) {
        if (rdna4s_parse((const uint32_t *)fsp.bytes, fsp.length / 4u, &mfMod, swhy, sizeof swhy) ||
            !mfMod.has_ep || mfMod.ep_model != 4 || rdna4s_reflect(&mfMod, 1, rf)) {
            if (err) *err = rdna4_err(52, [NSString stringWithFormat:@"fragment SPIR-V reflection: %s", swhy[0] ? swhy : rf->why]);
            bad = YES;
        } else {
            strlcpy(_fep, mfMod.ep_name[0] ? mfMod.ep_name : "main", sizeof _fep);
            size_t count = fsp.length / 4u;
            uint32_t *normalized = NULL;
            if (rdna4s_widen_fragment_output((const uint32_t *)fsp.bytes, &count, &mfMod, &normalized)) {
                if (err) *err = rdna4_err(52, @"fragment SPIR-V: insufficient memory to widen color outputs");
                bad = YES;
            } else if (normalized) {
                fsp = [NSData dataWithBytesNoCopy:normalized length:count * 4u freeWhenDone:YES];
            }
        }
    }
    rdna4s_free(&moMod); rdna4s_free(&mmMod); rdna4s_free(&mfMod);
    if (bad) { free(ro); free(rm); free(rf); return nil; }

    // Set up colour & depth/stencil attachments + render pass (mirrors RDNA4RenderPipelineState).
    MTLPixelFormat dpf = (MTLPixelFormat)rdna4_mesh_uint_prop(d, "depthAttachmentPixelFormat", (NSUInteger)MTLPixelFormatInvalid);
    MTLPixelFormat spf = (MTLPixelFormat)rdna4_mesh_uint_prop(d, "stencilAttachmentPixelFormat", (NSUInteger)MTLPixelFormatInvalid);
    NSUInteger sampleCount = rdna4_mesh_uint_prop(d, "rasterSampleCount", 1u);
    BOOL a2c = rdna4x_bool_prop(d, "isAlphaToCoverageEnabled");
    const char *pcwhy = NULL;
    if (!rdna4pc_config(dpf != MTLPixelFormatInvalid, spf != MTLPixelFormatInvalid,
                        (unsigned long)sampleCount, a2c ? 1 : 0, rdna4_ms_mask(), &_pc, &pcwhy)) {
        if (err) *err = rdna4_err(44, [NSString stringWithFormat:@"rasterSampleCount %lu: %s", (unsigned long)sampleCount, pcwhy ?: "unsupported"]);
        free(ro); free(rm); free(rf); return nil;
    }

    VkAttachmentDescription ads[9];
    VkAttachmentReference refs[8];
    uint32_t na = 0u, dualSrcUsed = 0u;
    id colorAtts = rdna4x_prop(d, "colorAttachments");
    for (NSUInteger i = 0; i < 8; i++) {
        MTLRenderPipelineColorAttachmentDescriptor *ca = colorAtts ? colorAtts[i] : nil;
        if (!ca || ca.pixelFormat == MTLPixelFormatInvalid) continue;
        const RDNA4Fmt *f = rdna4_fmt(ca.pixelFormat);
        if (!f || (f->flags & (RDNA4F_A8 | RDNA4F_BC))) {
            if (err) *err = rdna4_err(43, [NSString stringWithFormat:@"unsupported colour attachment format %lu", (unsigned long)ca.pixelFormat]);
            free(ro); free(rm); free(rf); return nil;
        }
        VkFormatFeatureFlags ff = rdna4_fmt_feats(f);
        VkFormatFeatureFlags needF = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
            (ca.isBlendingEnabled ? VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT : 0);
        if ((ff & needF) != needF) {
            if (err) *err = rdna4_err(43, [NSString stringWithFormat:@"unsupported colour attachment format %lu (RADV lacks attachment/blend support)", (unsigned long)ca.pixelFormat]);
            free(ro); free(rm); free(rf); return nil;
        }
        ads[na] = (VkAttachmentDescription){
            .format = f->vk, .samples = (VkSampleCountFlagBits)_pc.samples,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        };
        refs[na] = (VkAttachmentReference){ na, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkColorComponentFlags wm = 0;
        MTLColorWriteMask m = ca.writeMask;
        if (m & MTLColorWriteMaskRed)   wm |= VK_COLOR_COMPONENT_R_BIT;
        if (m & MTLColorWriteMaskGreen) wm |= VK_COLOR_COMPONENT_G_BIT;
        if (m & MTLColorWriteMaskBlue)  wm |= VK_COLOR_COMPONENT_B_BIT;
        if (m & MTLColorWriteMaskAlpha) wm |= VK_COLOR_COMPONENT_A_BIT;
        if (!_hasFrag) wm = 0;
        else if (rf && !(rf->outmask & (1u << na))) wm = 0;
        _cba[na] = (VkPipelineColorBlendAttachmentState){ .colorWriteMask = wm };
        if (ca.isBlendingEnabled) {
            BOOL blendBad = NO;
            BOOL (^isDual)(MTLBlendFactor) = ^(MTLBlendFactor bf) {
                return (BOOL)(bf == MTLBlendFactorSource1Color || bf == MTLBlendFactorOneMinusSource1Color ||
                              bf == MTLBlendFactorSource1Alpha || bf == MTLBlendFactorOneMinusSource1Alpha);
            };
            if (isDual(ca.sourceRGBBlendFactor) || isDual(ca.destinationRGBBlendFactor) ||
                isDual(ca.sourceAlphaBlendFactor) || isDual(ca.destinationAlphaBlendFactor)) {
                dualSrcUsed++;
                if (!RDNA4R.dualSrcBlend || na >= RDNA4R.lim.maxFragmentDualSrcAttachments ||
                    dualSrcUsed > RDNA4R.lim.maxFragmentDualSrcAttachments)
                    blendBad = YES;
            }
            _cba[na].blendEnable = VK_TRUE;
            _cba[na].srcColorBlendFactor = rdna4_bf(ca.sourceRGBBlendFactor, &blendBad);
            _cba[na].dstColorBlendFactor = rdna4_bf(ca.destinationRGBBlendFactor, &blendBad);
            _cba[na].srcAlphaBlendFactor = rdna4_bf(ca.sourceAlphaBlendFactor, &blendBad);
            _cba[na].dstAlphaBlendFactor = rdna4_bf(ca.destinationAlphaBlendFactor, &blendBad);
            _cba[na].colorBlendOp = rdna4_bo(ca.rgbBlendOperation, &blendBad);
            _cba[na].alphaBlendOp = rdna4_bo(ca.alphaBlendOperation, &blendBad);
            if (blendBad) {
                if (err) *err = rdna4_err(56, @"blend factor/operation or dual-source attachment limit not supported");
                free(ro); free(rm); free(rf); return nil;
            }
        }
        na++;
    }
    _na = na;
    _argumentTables.fragmentColorAttachments = na;
    rdna4_argument_descriptor_usage descriptorUsage;
    if (!rdna4_argument_descriptor_scan(NULL, rf, na, &descriptorUsage) ||
        !rdna4_argument_descriptor_validate(&descriptorUsage, NO, err)) {
        free(ro); free(rm); free(rf); return nil;
    }

    uint32_t nd = 0u;
    VkAttachmentReference dref = { 0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    _dpf = dpf; _spf = spf; _dsVk = VK_FORMAT_UNDEFINED; _dsAsp = 0u;
    if (_dpf != MTLPixelFormatInvalid || _spf != MTLPixelFormatInvalid) {
        MTLPixelFormat dsp = _dpf != MTLPixelFormatInvalid ? _dpf : _spf;
        rdna4dp_fmt_t df; const char *dwhy = NULL;
        if (_dpf != MTLPixelFormatInvalid && _spf != MTLPixelFormatInvalid && _dpf != _spf)
            dwhy = "separate depth and stencil attachment formats are not supported";
        else if (!rdna4dp_fmt((unsigned long)dsp, &df)) dwhy = df.why;
        else if (_dpf != MTLPixelFormatInvalid && !(df.aspects & RDNA4DP_ASP_DEPTH)) dwhy = "depthAttachmentPixelFormat is stencil-only";
        else if (_spf != MTLPixelFormatInvalid && !(df.aspects & RDNA4DP_ASP_STENCIL)) dwhy = "stencilAttachmentPixelFormat lacks stencil";
        const RDNA4Fmt *dsf = dwhy ? NULL : rdna4_fmt_ds(dsp);
        if (!dwhy && !dsf) dwhy = "no Vulkan depth/stencil mapping";
        if (!dwhy && !(rdna4_fmt_feats(dsf) & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
            dwhy = "backend lacks depth/stencil attachment support";
        if (dwhy) {
            if (err) *err = rdna4_err(44, [NSString stringWithFormat:@"unsupported depth/stencil attachment: %s", dwhy]);
            free(ro); free(rm); free(rf); return nil;
        }
        _dsVk = dsf->vk; _dsAsp = df.aspects; nd = 1u; dref.attachment = na;
        ads[na] = (VkAttachmentDescription){
            .format = dsf->vk, .samples = (VkSampleCountFlagBits)_pc.samples,
            .loadOp = (df.aspects & RDNA4DP_ASP_DEPTH) ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .storeOp = (df.aspects & RDNA4DP_ASP_DEPTH) ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .stencilLoadOp = (df.aspects & RDNA4DP_ASP_STENCIL) ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = (df.aspects & RDNA4DP_ASP_STENCIL) ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        };
    }

    for (int b = 192; rf && b < 200; b++) {
        if (rf->bind[1][b].used && rf->bind[1][b].type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) {
            _fetch = YES; _fetchMax = (uint32_t)(b - 192);
        }
    }
    if (_fetch && (_fetchMax >= na || _pc.samples > 1)) {
        if (err) *err = rdna4_err(59, @"mesh pipeline framebuffer fetch attachment index out of range or multisampled");
        free(ro); free(rm); free(rf); return nil;
    }
    if (_fetch) {
        for (uint32_t i = 0; i < na; i++) ads[i].initialLayout = ads[i].finalLayout = VK_IMAGE_LAYOUT_GENERAL;
        _rp = rdna4_mk_rp(ads, na, nd, YES);
        if (!_rp) { if (err) *err = rdna4_err(45, @"vkCreateRenderPass(mesh fetch)"); free(ro); free(rm); free(rf); return nil; }
    } else {
        VkSubpassDescription sp = {
            .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
            .colorAttachmentCount = na, .pColorAttachments = na ? refs : NULL,
            .pDepthStencilAttachment = nd ? &dref : NULL,
        };
        VkRenderPassCreateInfo rpc = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
            .attachmentCount = na + nd, .pAttachments = (na + nd) ? ads : NULL,
            .subpassCount = 1, .pSubpasses = &sp,
        };
        VkResult rpr = vkCreateRenderPass(RDNA4R.dev, &rpc, NULL, &_rp);
        if (rpr != VK_SUCCESS) {
            if (err) *err = rdna4_err(45, [NSString stringWithFormat:@"vkCreateRenderPass(mesh) = %d", rpr]);
            free(ro); free(rm); free(rf); return nil;
        }
    }

    // Build stage descriptor set layouts + pipeline layout(s).
    NSError *le = nil;
    if (!rdna4_mesh_build_stage_dsl(rm, 0, VK_SHADER_STAGE_MESH_BIT_EXT, mm_, &_meshRefl, emul,
                                    &_emulMeshMask, &_dsl[0], &_pbM, &_npbM, _owned, &le) ||
        !rdna4_stage_dsl(rf, 1, VK_SHADER_STAGE_FRAGMENT_BIT, mf_, &_dsl[1], &_pbF, &_npbF, _owned, &le)) {
        if (err) *err = le; free(ro); free(rm); free(rf); return nil;
    }
    if (_hasObject) {
        VkShaderStageFlags objStage = emul ? VK_SHADER_STAGE_COMPUTE_BIT : VK_SHADER_STAGE_TASK_BIT_EXT;
        VkDescriptorSetLayout *objDslPtr = emul ? &_objCompDsl : &_dslObj;
        if (!rdna4_mesh_build_stage_dsl(ro, 0, objStage, mo_, &_objRefl, emul,
                                        &_emulObjMask, objDslPtr, &_pbO, &_npbO, _owned, &le)) {
            if (err) *err = le; free(ro); free(rm); free(rf); return nil;
        }
        if (emul && !(_emulObjMask & RDNA4MESH_EMUL_BIT_INDIRECT)) {
            if (err) *err = rdna4_err(49, @"emulated object shader is missing set-0 binding 675 (indirect command SSBO)");
            free(ro); free(rm); free(rf); return nil;
        }
        if (emul && _objRefl.has_object && _objRefl.obj.has_payload && !(_emulObjMask & RDNA4MESH_EMUL_BIT_PAYLOAD)) {
            if (err) *err = rdna4_err(49, @"emulated object shader with payload is missing set-0 binding 674 (payload SSBO)");
            free(ro); free(rm); free(rf); return nil;
        }
        if (emul && _meshRefl.has_mesh && _meshRefl.mesh.has_payload && !(_emulMeshMask & RDNA4MESH_EMUL_BIT_PAYLOAD)) {
            if (err) *err = rdna4_err(49, @"emulated mesh shader with payload is missing set-0 binding 674 (payload SSBO)");
            free(ro); free(rm); free(rf); return nil;
        }
    }

    if (_hasFrag) {
        _argumentTables.comparisonSamplers = [mf_[@"argument_resource_tables"][@"comparison_sampler"] boolValue];
        _argumentTables.samplerCoordinates = [mf_[@"normalized_sampler_coordinates"] boolValue];
        if (!rdna4_argument_table_metadata_apply(&_argumentTables, rf, 1, mf_, &le) ||
            !rdna4_argument_table_layouts(&_argumentTables, NULL, rf, NO, &le) ||
            !rdna4_argument_stage_remap(&_argumentTables, _pbF, _npbF, &le) ||
            !rdna4_argument_scan_plan_build(&_scanF, preF, mf_, "mesh-fragment", &le)) {
            if (err) *err = le; free(ro); free(rm); free(rf); return nil;
        }
    }

    // Push constants across graphics stages (+ separate compute layout for emulated Object).
    uint32_t gfxPcs = 0u; VkShaderStageFlags gfxPcf = 0;
    if (rm && rm->pc_size) { gfxPcs = rm->pc_size; gfxPcf |= VK_SHADER_STAGE_MESH_BIT_EXT; }
    if (rf && rf->pc_size) { if (rf->pc_size > gfxPcs) gfxPcs = rf->pc_size; gfxPcf |= VK_SHADER_STAGE_FRAGMENT_BIT; }
    if (_hasObject && !emul && ro && ro->pc_size) {
        if (ro->pc_size > gfxPcs) gfxPcs = ro->pc_size;
        gfxPcf |= VK_SHADER_STAGE_TASK_BIT_EXT;
    }
    if (gfxPcs > 256u) {
        if (err) *err = rdna4_err(48, [NSString stringWithFormat:@"mesh pipeline push constant block %u B > 256", gfxPcs]);
        free(ro); free(rm); free(rf); return nil;
    }
    _pcs = gfxPcs; _pcf = gfxPcf;

    _objSet = _argumentTables.active ? 5u : 2u;
    VkDescriptorSetLayout layouts[6] = {
        _dsl[0], _dsl[1],
        _argumentTables.active ? _argumentTables.dsl[0] : _dslObj,
        _argumentTables.dsl[1], _argumentTables.dsl[2], _dslObj,
    };
    BOOL nativeObj = (_meshExecMode == RDNA4MESH_MODE_NATIVE_TASK_MESH);
    uint32_t layoutCount = _argumentTables.active ? (nativeObj ? 6u : 5u) : (nativeObj ? 3u : 2u);
    VkPushConstantRange pcr = { gfxPcf, 0, gfxPcs };
    VkPipelineLayoutCreateInfo plc = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = layoutCount,
        .pSetLayouts = layouts,
        .pushConstantRangeCount = gfxPcs ? 1u : 0u,
        .pPushConstantRanges = &pcr,
    };
    VkResult plr = vkCreatePipelineLayout(RDNA4R.dev, &plc, NULL, &_pl);
    if (plr != VK_SUCCESS) {
        if (err) *err = rdna4_err(46, [NSString stringWithFormat:@"vkCreatePipelineLayout(mesh) = %d", plr]);
        free(ro); free(rm); free(rf); return nil;
    }

    if (emul) {
        uint32_t objPcs = (ro && ro->pc_size) ? ro->pc_size : 0u;
        VkPushConstantRange opcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, objPcs };
        VkPipelineLayoutCreateInfo oplc = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1u,
            .pSetLayouts = &_objCompDsl,
            .pushConstantRangeCount = objPcs ? 1u : 0u,
            .pPushConstantRanges = &opcr,
        };
        VkResult oplr = vkCreatePipelineLayout(RDNA4R.dev, &oplc, NULL, &_objCompPl);
        if (oplr != VK_SUCCESS) {
            if (err) *err = rdna4_err(46, [NSString stringWithFormat:@"vkCreatePipelineLayout(object-emul) = %d", oplr]);
            free(ro); free(rm); free(rf); return nil;
        }
    }
    free(ro); free(rm); free(rf);

    // Create initial VkShaderModules (relocating native TaskEXT to _objSet).
    if (_hasObject) {
        NSData *objUse = emul ? osd : rdna4_spv_set(osd, _objSet);
        _objMod = rdna4_spv_module(objUse.bytes, objUse.length, emul ? "object-emul" : "object-task", err);
        if (!_objMod) return nil;
    }
    _meshMod = rdna4_spv_module(msd.bytes, msd.length, "mesh", err);
    if (!_meshMod) return nil;
    if (_hasFrag) {
        _fs = rdna4_spv_module(fsp.bytes, fsp.length, "mesh-fragment", err);
        if (!_fs) return nil;
    }

    // Build and verify initial pipeline(s) so bad shader/state fails synchronously at creation time.
    if (emul) {
        VkPipeline op0 = [self rdna4ObjectComputePipelineForLocal:_initObjLocal error:err];
        if (!op0) return nil;
    }
    uint32_t dk0 = [self rdna4DSKey:NULL];
    VkPipeline gp0 = [self rdna4MeshPipelineForCull:VK_CULL_MODE_NONE front:VK_FRONT_FACE_CLOCKWISE
                                                 ds:dk0 rflags:0
                                           objLocal:_initObjLocal meshLocal:_initMeshLocal error:err];
    if (!gp0) return nil;

    RDNA4LOG("RDNA4MeshRenderPipelineState %p: mode=%d payload=%u B (stride %u B) maxV=%u maxP=%u topo=%d colorAtt=%u",
             (__bridge void *)self, (int)_meshExecMode, _linkedInfo.effective_payload_bytes,
             _linkedInfo.effective_payload_stride, _linkedInfo.max_vertices, _linkedInfo.max_primitives,
             (int)_linkedInfo.topology, _na);
    return self;
}

- (void)dealloc {
    if (RDNA4R.ok) {
        for (NSValue *v in _meshVariants.allValues) vkDestroyPipeline(RDNA4R.dev, (VkPipeline)v.pointerValue, NULL);
        for (NSValue *v in _objCompVariants.allValues) vkDestroyPipeline(RDNA4R.dev, (VkPipeline)v.pointerValue, NULL);
        for (NSValue *v in _objModCache.allValues) vkDestroyShaderModule(RDNA4R.dev, (VkShaderModule)v.pointerValue, NULL);
        for (NSValue *v in _meshModCache.allValues) vkDestroyShaderModule(RDNA4R.dev, (VkShaderModule)v.pointerValue, NULL);
        if (_objCompPl) vkDestroyPipelineLayout(RDNA4R.dev, _objCompPl, NULL);
        if (_objCompDsl) vkDestroyDescriptorSetLayout(RDNA4R.dev, _objCompDsl, NULL);
        if (_dslObj) vkDestroyDescriptorSetLayout(RDNA4R.dev, _dslObj, NULL);
        if (_objMod) vkDestroyShaderModule(RDNA4R.dev, _objMod, NULL);
        if (_meshMod) vkDestroyShaderModule(RDNA4R.dev, _meshMod, NULL);
    }
    free(_pbO); _pbO = NULL; _npbO = 0u;
    free(_pbM); _pbM = NULL; _npbM = 0u;
}

@end

// =================================================================================================
// 5. Device factory helpers for GarnetDevice.m (Wave 3E)
// =================================================================================================

__attribute__((unused)) GARNET_INTERNAL id rdna4_mesh_device_new_pipeline(
    id dev, id desc, NSUInteger options, MTLRenderPipelineReflection **refl, NSError **error)
{
    if (refl) *refl = nil;
    NSError *e = nil;
    RDNA4MeshRenderPipelineState *p = [[RDNA4MeshRenderPipelineState alloc]
        initWithDevice:dev meshDescriptor:desc options:options error:&e];
    if (!p) {
        if (e) RDNA4LOG_ERR("mesh pipeline creation failed: %s", e.localizedDescription.UTF8String);
        if (error) *error = [e copy];
        return nil;
    }
    return p;
}

__attribute__((unused)) GARNET_INTERNAL void rdna4_mesh_device_new_pipeline_async(
    id dev, id desc, NSUInteger options, void (^handler)(id, id, NSError *))
{
    if (!handler) return;
    id saved = [desc respondsToSelector:@selector(copy)] ? [desc copy] : desc;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ @autoreleasepool {
        NSError *e = nil;
        MTLRenderPipelineReflection *r = nil;
        id p = rdna4_mesh_device_new_pipeline(dev, saved, options, &r, &e);
        handler(p, r, e);
    }});
}

// =================================================================================================
// 6. RDNA4RenderEncoder (RDNA4Mesh): Object & Mesh binding tables + direct/threads/indirect draws
// =================================================================================================

@implementation RDNA4MeshEncoderState @end

static char rdna4_mesh_enc_state_key;

static BOOL rdna4_mesh_update_emul_descriptors(VkDescriptorSet ds, uint32_t emulMask,
                                               RDNA4Buffer *gpBuf, uint64_t gpVkOff,
                                               RDNA4Buffer *scratchBuf,
                                               const rdna4mesh_emul_plan_t *plan) {
    VkWriteDescriptorSet writes[3];
    VkDescriptorBufferInfo infos[3];
    uint32_t nw = 0u;
    uint64_t sBase = [scratchBuf rdna4VkOffset];
    VkBuffer sVk = [scratchBuf vkBuffer];

    if (emulMask & RDNA4MESH_EMUL_BIT_GRID_PARAMS) {
        infos[nw] = (VkDescriptorBufferInfo){ [gpBuf vkBuffer], gpVkOff, RDNA4MESH_GRID_PARAMS_BYTES };
        writes[nw] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = ds, .dstBinding = GARNETX_MESH_GRID_PARAMS_BINDING,
            .descriptorCount = 1u, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &infos[nw],
        };
        nw++;
    }
    if (emulMask & RDNA4MESH_EMUL_BIT_PAYLOAD) {
        infos[nw] = (VkDescriptorBufferInfo){ sVk, sBase + plan->payload_offset, plan->payload_bytes };
        writes[nw] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = ds, .dstBinding = GARNETX_MESH_PAYLOAD_BINDING,
            .descriptorCount = 1u, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &infos[nw],
        };
        nw++;
    }
    if (emulMask & RDNA4MESH_EMUL_BIT_INDIRECT) {
        infos[nw] = (VkDescriptorBufferInfo){ sVk, sBase + plan->indirect_offset, plan->indirect_bytes };
        writes[nw] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = ds, .dstBinding = GARNETX_MESH_INDIRECT_BINDING,
            .descriptorCount = 1u, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &infos[nw],
        };
        nw++;
    }
    if (nw > 0u) vkUpdateDescriptorSets(RDNA4R.dev, nw, writes, 0u, NULL);
    return YES;
}

@implementation RDNA4RenderEncoder (RDNA4Mesh)

- (RDNA4MeshEncoderState *)rdna4MeshState {
    RDNA4MeshEncoderState *st = objc_getAssociatedObject(self, &rdna4_mesh_enc_state_key);
    if (!st) {
        st = [RDNA4MeshEncoderState new];
        objc_setAssociatedObject(self, &rdna4_mesh_enc_state_key, st, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }
    return st;
}

// ---- Object & Mesh stage binding helpers ----
- (void)rdna4SetMeshStageBuffer:(id)buf offset:(NSUInteger)off index:(NSUInteger)i isMesh:(BOOL)isMesh {
    if ([_cb rdna4Error]) return;
    if (i >= RDNA4MESH_MAX_BUFFERS) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"%s buffer index %lu >= 31", isMesh ? "mesh" : "object", (unsigned long)i]];
        return;
    }
    if (buf && ![buf isKindOfClass:[RDNA4Buffer class]]) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"%s setBuffer: not an RDNA4Buffer", isMesh ? "mesh" : "object"]];
        return;
    }
    RDNA4MeshEncoderState *ms = [self rdna4MeshState];
    RDNA4StageState *s = isMesh ? &ms->meshState : &ms->objState;
    rdna4_stage_set_buf(s, i, buf, off);
    [self rdna4OwnResource:buf kind:(isMesh ? RDNA4_RESOURCE_MESH_BUFFER : RDNA4_RESOURCE_OBJECT_BUFFER)
                  fragment:NO index:i];
}

- (void)rdna4SetMeshStageBytes:(const void *)p length:(NSUInteger)n index:(NSUInteger)i isMesh:(BOOL)isMesh {
    if ([_cb rdna4Error]) return;
    if (i >= RDNA4MESH_MAX_BUFFERS || (n && !p)) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"%s setBytes: index %lu / bytes %p", isMesh ? "mesh" : "object", (unsigned long)i, p]];
        return;
    }
    RDNA4Buffer *b = nil; NSUInteger off = 0;
    if ([_cb rdna4Bytes:p length:n buffer:&b offset:&off])
        [self rdna4SetMeshStageBuffer:b offset:off index:i isMesh:isMesh];
}

- (void)rdna4SetMeshStageTexture:(id)t index:(NSUInteger)i isMesh:(BOOL)isMesh {
    if ([_cb rdna4Error]) return;
    if (i >= RDNA4MESH_MAX_TEXTURES) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"%s texture index %lu >= 128", isMesh ? "mesh" : "object", (unsigned long)i]];
        return;
    }
    if (t && ![t isKindOfClass:[RDNA4Texture class]]) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"%s setTexture: not an RDNA4Texture", isMesh ? "mesh" : "object"]];
        return;
    }
    RDNA4MeshEncoderState *ms = [self rdna4MeshState];
    RDNA4StageState *s = isMesh ? &ms->meshState : &ms->objState;
    s->tex[i] = t;
    [self rdna4OwnResource:t kind:(isMesh ? RDNA4_RESOURCE_MESH_TEXTURE : RDNA4_RESOURCE_OBJECT_TEXTURE)
                  fragment:NO index:i];
}

- (void)rdna4SetMeshStageSampler:(id)s lodMinClamp:(float)a lodMaxClamp:(float)b
                        override:(BOOL)override index:(NSUInteger)i isMesh:(BOOL)isMesh {
    if ([_cb rdna4Error]) return;
    if (i >= RDNA4MESH_MAX_SAMPLERS) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"%s sampler index %lu >= 16", isMesh ? "mesh" : "object", (unsigned long)i]];
        return;
    }
    if (s && ![s isKindOfClass:[RDNA4SamplerState class]]) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"%s setSamplerState: not an RDNA4SamplerState", isMesh ? "mesh" : "object"]];
        return;
    }
    RDNA4SamplerBinding *binding = nil;
    if (s) {
        NSError *error = nil;
        RDNA4SamplerState *sampler = (RDNA4SamplerState *)s;
        binding = override
            ? [sampler rdna4BindingWithLODMinClamp:a lodMaxClamp:b error:&error]
            : [sampler rdna4Binding:&error];
        if (!binding) {
            [_cb rdna4Fail:error.localizedDescription ?: @"mesh/object sampler binding unavailable"];
            return;
        }
        [_cb rdna4RetainArgument:binding];
        if ([_cb rdna4Error]) return;
    }
    RDNA4MeshEncoderState *ms = [self rdna4MeshState];
    RDNA4StageState *st = isMesh ? &ms->meshState : &ms->objState;
    RDNA4SamplerBinding *old = st->smp[i];
    st->smp[i] = binding;
    if (binding) {
        if (!ms->samplers) ms->samplers = [NSMutableSet set];
        [ms->samplers addObject:binding];
    }
    if (old && old != binding) {
        BOOL used = NO;
        for (NSUInteger k = 0; k < RDNA4MESH_MAX_SAMPLERS && !used; k++)
            used = (ms->objState.smp[k] == old || ms->meshState.smp[k] == old);
        if (!used) [ms->samplers removeObject:old];
    }
}

// ---- Object stage bindings ----
- (void)setObjectBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o atIndex:(NSUInteger)i { [self rdna4SetMeshStageBuffer:b offset:o index:i isMesh:NO]; }
- (void)setObjectBufferOffset:(NSUInteger)o atIndex:(NSUInteger)i {
    if (i < RDNA4MESH_MAX_BUFFERS) [self rdna4MeshState]->objState.buf[i].off = o;
}
- (void)setObjectBuffers:(const id<MTLBuffer> __unsafe_unretained *)b offsets:(const NSUInteger *)o withRange:(NSRange)r {
    if (r.length && (!b || !o || r.location > RDNA4MESH_MAX_BUFFERS || r.length > RDNA4MESH_MAX_BUFFERS - r.location)) {
        [_cb rdna4Fail:@"setObjectBuffers: invalid range or NULL array"]; return;
    }
    for (NSUInteger k = 0; k < r.length; k++) [self rdna4SetMeshStageBuffer:b[k] offset:o[k] index:r.location + k isMesh:NO];
}
- (void)setObjectBytes:(const void *)p length:(NSUInteger)n atIndex:(NSUInteger)i { [self rdna4SetMeshStageBytes:p length:n index:i isMesh:NO]; }
- (void)setObjectTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i { [self rdna4SetMeshStageTexture:t index:i isMesh:NO]; }
- (void)setObjectTextures:(const id<MTLTexture> __unsafe_unretained *)t withRange:(NSRange)r {
    if (r.length && (!t || r.location > RDNA4MESH_MAX_TEXTURES || r.length > RDNA4MESH_MAX_TEXTURES - r.location)) {
        [_cb rdna4Fail:@"setObjectTextures: invalid range or NULL array"]; return;
    }
    for (NSUInteger k = 0; k < r.length; k++) [self rdna4SetMeshStageTexture:t[k] index:r.location + k isMesh:NO];
}
- (void)setObjectSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)i {
    [self rdna4SetMeshStageSampler:s lodMinClamp:0 lodMaxClamp:0 override:NO index:i isMesh:NO];
}
- (void)setObjectSamplerState:(id<MTLSamplerState>)s lodMinClamp:(float)lo lodMaxClamp:(float)hi atIndex:(NSUInteger)i {
    [self rdna4SetMeshStageSampler:s lodMinClamp:lo lodMaxClamp:hi override:YES index:i isMesh:NO];
}
- (void)setObjectSamplerStates:(const id<MTLSamplerState> __unsafe_unretained *)s withRange:(NSRange)r {
    if (r.length && (!s || r.location > RDNA4MESH_MAX_SAMPLERS || r.length > RDNA4MESH_MAX_SAMPLERS - r.location)) {
        [_cb rdna4Fail:@"setObjectSamplerStates: invalid range or NULL array"]; return;
    }
    for (NSUInteger k = 0; k < r.length; k++) [self rdna4SetMeshStageSampler:s[k] lodMinClamp:0 lodMaxClamp:0 override:NO index:r.location + k isMesh:NO];
}
- (void)setObjectSamplerStates:(const id<MTLSamplerState> __unsafe_unretained *)s
                  lodMinClamps:(const float *)lo lodMaxClamps:(const float *)hi withRange:(NSRange)r {
    if (r.length && (!s || !lo || !hi || r.location > RDNA4MESH_MAX_SAMPLERS || r.length > RDNA4MESH_MAX_SAMPLERS - r.location)) {
        [_cb rdna4Fail:@"setObjectSamplerStates:lodMinClamps: invalid range or NULL array"]; return;
    }
    for (NSUInteger k = 0; k < r.length; k++) [self rdna4SetMeshStageSampler:s[k] lodMinClamp:lo[k] lodMaxClamp:hi[k] override:YES index:r.location + k isMesh:NO];
}
- (void)setObjectThreadgroupMemoryLength:(NSUInteger)length atIndex:(NSUInteger)index {
    if ([_cb rdna4Error]) return;
    RDNA4MeshEncoderState *ms = [self rdna4MeshState];
    const char *why = NULL;
    uint64_t newTotal = 0u;
    rdna4mesh_status_t st = rdna4mesh_validate_tg_memory(
        ms->objTgMem, index <= UINT32_MAX ? (uint32_t)index : UINT32_MAX,
        (uint64_t)length, (uint64_t)rdna4_device_caps().shared_memory, &newTotal, &why);
    if (st != RDNA4MESH_OK) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"setObjectThreadgroupMemoryLength:%lu atIndex:%lu: %s",
                        (unsigned long)length, (unsigned long)index, why ? why : rdna4mesh_status_str(st)]];
        return;
    }
    ms->objTgMem[index] = (uint64_t)length;
}

// ---- Mesh stage bindings ----
- (void)setMeshBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o atIndex:(NSUInteger)i { [self rdna4SetMeshStageBuffer:b offset:o index:i isMesh:YES]; }
- (void)setMeshBufferOffset:(NSUInteger)o atIndex:(NSUInteger)i {
    if (i < RDNA4MESH_MAX_BUFFERS) [self rdna4MeshState]->meshState.buf[i].off = o;
}
- (void)setMeshBuffers:(const id<MTLBuffer> __unsafe_unretained *)b offsets:(const NSUInteger *)o withRange:(NSRange)r {
    if (r.length && (!b || !o || r.location > RDNA4MESH_MAX_BUFFERS || r.length > RDNA4MESH_MAX_BUFFERS - r.location)) {
        [_cb rdna4Fail:@"setMeshBuffers: invalid range or NULL array"]; return;
    }
    for (NSUInteger k = 0; k < r.length; k++) [self rdna4SetMeshStageBuffer:b[k] offset:o[k] index:r.location + k isMesh:YES];
}
- (void)setMeshBytes:(const void *)p length:(NSUInteger)n atIndex:(NSUInteger)i { [self rdna4SetMeshStageBytes:p length:n index:i isMesh:YES]; }
- (void)setMeshTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i { [self rdna4SetMeshStageTexture:t index:i isMesh:YES]; }
- (void)setMeshTextures:(const id<MTLTexture> __unsafe_unretained *)t withRange:(NSRange)r {
    if (r.length && (!t || r.location > RDNA4MESH_MAX_TEXTURES || r.length > RDNA4MESH_MAX_TEXTURES - r.location)) {
        [_cb rdna4Fail:@"setMeshTextures: invalid range or NULL array"]; return;
    }
    for (NSUInteger k = 0; k < r.length; k++) [self rdna4SetMeshStageTexture:t[k] index:r.location + k isMesh:YES];
}
- (void)setMeshSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)i {
    [self rdna4SetMeshStageSampler:s lodMinClamp:0 lodMaxClamp:0 override:NO index:i isMesh:YES];
}
- (void)setMeshSamplerState:(id<MTLSamplerState>)s lodMinClamp:(float)lo lodMaxClamp:(float)hi atIndex:(NSUInteger)i {
    [self rdna4SetMeshStageSampler:s lodMinClamp:lo lodMaxClamp:hi override:YES index:i isMesh:YES];
}
- (void)setMeshSamplerStates:(const id<MTLSamplerState> __unsafe_unretained *)s withRange:(NSRange)r {
    if (r.length && (!s || r.location > RDNA4MESH_MAX_SAMPLERS || r.length > RDNA4MESH_MAX_SAMPLERS - r.location)) {
        [_cb rdna4Fail:@"setMeshSamplerStates: invalid range or NULL array"]; return;
    }
    for (NSUInteger k = 0; k < r.length; k++) [self rdna4SetMeshStageSampler:s[k] lodMinClamp:0 lodMaxClamp:0 override:NO index:r.location + k isMesh:YES];
}
- (void)setMeshSamplerStates:(const id<MTLSamplerState> __unsafe_unretained *)s
                lodMinClamps:(const float *)lo lodMaxClamps:(const float *)hi withRange:(NSRange)r {
    if (r.length && (!s || !lo || !hi || r.location > RDNA4MESH_MAX_SAMPLERS || r.length > RDNA4MESH_MAX_SAMPLERS - r.location)) {
        [_cb rdna4Fail:@"setMeshSamplerStates:lodMinClamps: invalid range or NULL array"]; return;
    }
    for (NSUInteger k = 0; k < r.length; k++) [self rdna4SetMeshStageSampler:s[k] lodMinClamp:lo[k] lodMaxClamp:hi[k] override:YES index:r.location + k isMesh:YES];
}

// Footnote 10 encoder guards: VFT / IFT / AS cannot be bound to Object or Mesh stages.
- (void)setObjectVisibleFunctionTable:(id)t atBufferIndex:(NSUInteger)i {
    (void)i; if (t) [_cb rdna4Fail:@(rdna4mesh_status_str(RDNA4MESH_ERR_FOOTNOTE10_FN_PTR))];
}
- (void)setObjectVisibleFunctionTables:(const id __unsafe_unretained *)t withBufferRange:(NSRange)r {
    for (NSUInteger k = 0; t && k < r.length; k++) if (t[k]) { [_cb rdna4Fail:@(rdna4mesh_status_str(RDNA4MESH_ERR_FOOTNOTE10_FN_PTR))]; return; }
}
- (void)setMeshVisibleFunctionTable:(id)t atBufferIndex:(NSUInteger)i {
    (void)i; if (t) [_cb rdna4Fail:@(rdna4mesh_status_str(RDNA4MESH_ERR_FOOTNOTE10_FN_PTR))];
}
- (void)setMeshVisibleFunctionTables:(const id __unsafe_unretained *)t withBufferRange:(NSRange)r {
    for (NSUInteger k = 0; t && k < r.length; k++) if (t[k]) { [_cb rdna4Fail:@(rdna4mesh_status_str(RDNA4MESH_ERR_FOOTNOTE10_FN_PTR))]; return; }
}
- (void)setObjectIntersectionFunctionTable:(id)t atBufferIndex:(NSUInteger)i {
    (void)i; if (t) [_cb rdna4Fail:@(rdna4mesh_status_str(RDNA4MESH_ERR_FOOTNOTE10_RT))];
}
- (void)setObjectIntersectionFunctionTables:(const id __unsafe_unretained *)t withBufferRange:(NSRange)r {
    for (NSUInteger k = 0; t && k < r.length; k++) if (t[k]) { [_cb rdna4Fail:@(rdna4mesh_status_str(RDNA4MESH_ERR_FOOTNOTE10_RT))]; return; }
}
- (void)setMeshIntersectionFunctionTable:(id)t atBufferIndex:(NSUInteger)i {
    (void)i; if (t) [_cb rdna4Fail:@(rdna4mesh_status_str(RDNA4MESH_ERR_FOOTNOTE10_RT))];
}
- (void)setMeshIntersectionFunctionTables:(const id __unsafe_unretained *)t withBufferRange:(NSRange)r {
    for (NSUInteger k = 0; t && k < r.length; k++) if (t[k]) { [_cb rdna4Fail:@(rdna4mesh_status_str(RDNA4MESH_ERR_FOOTNOTE10_RT))]; return; }
}
- (void)setObjectAccelerationStructure:(id)as atBufferIndex:(NSUInteger)i {
    (void)i; if (as) [_cb rdna4Fail:@(rdna4mesh_status_str(RDNA4MESH_ERR_FOOTNOTE10_RT))];
}
- (void)setMeshAccelerationStructure:(id)as atBufferIndex:(NSUInteger)i {
    (void)i; if (as) [_cb rdna4Fail:@(rdna4mesh_status_str(RDNA4MESH_ERR_FOOTNOTE10_RT))];
}

// ---- Core mesh draw implementation (direct, threads, and indirect) ----
- (void)rdna4DrawMeshGroups:(MTLSize)grid
                  objLocal:(MTLSize)objTg
                 meshLocal:(MTLSize)meshTg
                  indirect:(BOOL)isIndirect
            indirectBuffer:(id)indirectBuf
            indirectOffset:(NSUInteger)indirectOff {
    if (_hostWriteDropped) return;
    if (_ended) { [_cb rdna4Fail:@"mesh draw after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    if (!RDNA4MeshFn.ready && !rdna4_mesh_resolve_vk_funcs()) {
        [_cb rdna4Fail:@"VK_EXT_mesh_shader command entry points are unavailable"];
        return;
    }
    if (!_pso || ![_pso rdna4IsMeshPipeline]) {
        [_cb rdna4Fail:@"drawMesh* requires a mesh render pipeline state"];
        return;
    }

    RDNA4MeshRenderPipelineState *mpso = (RDNA4MeshRenderPipelineState *)_pso;
    const rdna4mesh_exec_mode_t mode = [mpso rdna4MeshExecMode];
    const BOOL hasObj = [mpso rdna4HasObjectStage];
    const BOOL hasFrag = [mpso rdna4HasFragment];
    const rdna4dc_caps caps = rdna4_device_caps();
    const char *why = NULL;

    // 1. Validate threadgroup dimensions for Mesh (always) and Object (when present).
    uint32_t meshLocal3[3] = { 0u, 0u, 0u }, objLocal3[3] = { 1u, 1u, 1u };
    {
        const rdna4mesh_refl_t *mr = [mpso rdna4MeshRefl];
        rdna4mesh_status_t mst = rdna4mesh_validate_tg_size(
            (uint64_t)meshTg.width, (uint64_t)meshTg.height, (uint64_t)meshTg.depth,
            caps.workgroup_size, (uint32_t)[mpso maxTotalThreadsPerMeshThreadgroup],
            mr->has_max_work_group_size ? mr->max_work_group_size : 0u,
            (uint32_t)[mpso meshThreadExecutionWidth], [mpso rdna4MeshMultExecWidth] ? 1 : 0, &why);
        if (mst != RDNA4MESH_OK) {
            [_cb rdna4Fail:[NSString stringWithFormat:@"threadsPerMeshThreadgroup (%lu,%lu,%lu): %s",
                            (unsigned long)meshTg.width, (unsigned long)meshTg.height, (unsigned long)meshTg.depth,
                            why ? why : rdna4mesh_status_str(mst)]];
            return;
        }
        meshLocal3[0] = (uint32_t)meshTg.width;
        meshLocal3[1] = (uint32_t)meshTg.height;
        meshLocal3[2] = (uint32_t)meshTg.depth;
    }
    if (hasObj) {
        const rdna4mesh_refl_t *or_ = [mpso rdna4ObjRefl];
        rdna4mesh_status_t ost = rdna4mesh_validate_tg_size(
            (uint64_t)objTg.width, (uint64_t)objTg.height, (uint64_t)objTg.depth,
            caps.workgroup_size, (uint32_t)[mpso maxTotalThreadsPerObjectThreadgroup],
            or_->has_max_work_group_size ? or_->max_work_group_size : 0u,
            (uint32_t)[mpso objectThreadExecutionWidth], [mpso rdna4ObjMultExecWidth] ? 1 : 0, &why);
        if (ost != RDNA4MESH_OK) {
            [_cb rdna4Fail:[NSString stringWithFormat:@"threadsPerObjectThreadgroup (%lu,%lu,%lu): %s",
                            (unsigned long)objTg.width, (unsigned long)objTg.height, (unsigned long)objTg.depth,
                            why ? why : rdna4mesh_status_str(ost)]];
            return;
        }
        objLocal3[0] = (uint32_t)objTg.width;
        objLocal3[1] = (uint32_t)objTg.height;
        objLocal3[2] = (uint32_t)objTg.depth;
    }

    // 2. Validate grid (direct) or indirect buffer bounds & alignment (indirect).
    uint64_t userIndVkOff = 0u;
    if (isIndirect) {
        if (![indirectBuf isKindOfClass:[RDNA4Buffer class]] || [(RDNA4Buffer *)indirectBuf device] != [_cb device]) {
            [_cb rdna4Fail:@"drawMeshThreadgroupsWithIndirectBuffer: buffer is nil, wrong class, or wrong device"];
            return;
        }
        rdna4mesh_status_t ist = rdna4mesh_validate_indirect_buffer(
            (uint64_t)[(RDNA4Buffer *)indirectBuf length], (uint64_t)indirectOff,
            [(RDNA4Buffer *)indirectBuf rdna4VkOffset], &userIndVkOff, &why);
        if (ist != RDNA4MESH_OK) {
            [_cb rdna4Fail:[NSString stringWithFormat:@"drawMeshThreadgroupsWithIndirectBuffer: %s",
                            why ? why : rdna4mesh_status_str(ist)]];
            return;
        }
    } else {
        int isNoop = 0;
        uint64_t totalGroups = 0u;
        const uint32_t *axisLim = (mode == RDNA4MESH_MODE_NATIVE_TASK_MESH)
            ? caps.metal3.object_max_workgroup_count
            : (mode == RDNA4MESH_MODE_EMULATED_OBJECT_MESH)
                ? RDNA4R.lim.maxComputeWorkGroupCount
                : caps.metal3.mesh_max_workgroup_count;
        uint32_t totalLim = (mode == RDNA4MESH_MODE_NATIVE_TASK_MESH)
            ? caps.metal3.object_max_workgroup_total_count
            : (mode == RDNA4MESH_MODE_EMULATED_OBJECT_MESH)
                ? rdna4_mesh_max_draw_indirect_count()
                : [mpso rdna4MeshLinkedInfo]->effective_max_mesh_grid;
        rdna4mesh_status_t gst = rdna4mesh_validate_grid(
            (uint64_t)grid.width, (uint64_t)grid.height, (uint64_t)grid.depth,
            axisLim, totalLim, &isNoop, &totalGroups, &why);
        if (gst != RDNA4MESH_OK) {
            [_cb rdna4Fail:[NSString stringWithFormat:@"drawMeshThreadgroups (%lu,%lu,%lu): %s",
                            (unsigned long)grid.width, (unsigned long)grid.height, (unsigned long)grid.depth,
                            why ? why : rdna4mesh_status_str(gst)]];
            return;
        }
        if (isNoop) return;
    }

    // 3. Validate pass compatibility & build/fetch Vulkan pipeline(s).
    if ([mpso rdna4DSVk] != _dsVkFmt || [mpso rdna4SampleCount] != _encSamples) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"mesh pipeline depth/stencil (vk %d) or sample count (%u) differs from pass (vk %d, %u)",
                        (int)[mpso rdna4DSVk], [mpso rdna4SampleCount], (int)_dsVkFmt, _encSamples]];
        return;
    }
    if ([_cb rdna4HasArgumentWrites]) {
        if (_inPass) [self rdna4EndPass];
        [_cb rdna4ArgumentCheckpoint];
        if ([_cb rdna4Error]) return;
    }
    [self rdna4SyncResourceSegment];
    [self rdna4SnapshotResources];

    VkCullModeFlags cull = _cull == MTLCullModeFront ? VK_CULL_MODE_FRONT_BIT
        : _cull == MTLCullModeBack ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    VkFrontFace front = _wind == MTLWindingCounterClockwise ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    uint32_t dk = [mpso rdna4DSKey:&_dsi];
    uint32_t vpOff = _ampHasMappings ? _ampMappings[0].viewport_offset : 0u;
    uint32_t vpCount = rdna4amp_viewport_count(_nvp, _nsr, vpOff, RDNA4R.maxViewports ?: 1u);
    uint32_t rflags = ((_fillMode == MTLTriangleFillModeLines) ? 1u : 0u) |
                      ((_depthClipMode == MTLDepthClipModeClamp) ? 2u : 0u) |
                      (_hasSamplePos ? 4u : 0u) | ((vpCount - 1u) << 3);

    NSError *pe = nil;
    VkPipeline objCompPipe = VK_NULL_HANDLE;
    if (mode == RDNA4MESH_MODE_EMULATED_OBJECT_MESH) {
        objCompPipe = [mpso rdna4ObjectComputePipelineForLocal:objLocal3 error:&pe];
        if (!objCompPipe) {
            [_cb rdna4Fail:pe.localizedDescription ?: @"failed to build emulated object compute pipeline"];
            return;
        }
    }
    VkPipeline gfxPipe = [mpso rdna4MeshPipelineForCull:cull front:front ds:dk rflags:rflags
                                               objLocal:objLocal3 meshLocal:meshLocal3 error:&pe];
    if (!gfxPipe) {
        [_cb rdna4Fail:pe.localizedDescription ?: @"failed to build mesh graphics pipeline variant"];
        return;
    }

    // 4. Plan emulated Object+Mesh scratch buffer (if emulated) and execute Object compute pass outside render pass.
    RDNA4MeshEncoderState *ms = [self rdna4MeshState];
    id dev = [_cb device];
    VkCommandBuffer cmd = [_cb vk];
    RDNA4Buffer *emulScratch = nil;
    RDNA4Buffer *emulGpBuf = nil;
    uint64_t emulGpVkOff = 0u;
    rdna4mesh_emul_plan_t emulPlan;
    memset(&emulPlan, 0, sizeof emulPlan);

    void (^needTex)(RDNA4Texture *, VkImageLayout, BOOL) = ^(RDNA4Texture *t, VkImageLayout l, BOOL writable) {
        if ([t rdna4IsIOS]) (void)rdna4_ios_touch(self->_cb, t, YES, writable, ^{
            if (self->_inPass) [self rdna4EndPass];
        });
        [self->_cb rdna4CBRead:t];
        if ([t rdna4DispImgOnly]) (void)[self->_cb rdna4IOFirstTouch:t];
        if (([t rdna4IsIOS] || [t rdna4DispImgOnly]) && writable) {
            [self->_cb rdna4IOWritten:t];
            [self->_cb rdna4DispNote:t bits:RDNA4DF_W_COMPUTE pso:nil];
        }
        [self rdna4Need:t layout:l];
    };

    if (mode == RDNA4MESH_MODE_EMULATED_OBJECT_MESH) {
        const uint32_t payloadStride = [mpso rdna4MeshLinkedInfo]->effective_payload_stride;
        const uint32_t maxDrawInd = rdna4_mesh_max_draw_indirect_count();
        rdna4mesh_status_t pst;
        if (isIndirect) {
            if (maxDrawInd <= 1u) {
                [_cb rdna4Fail:@"indirect emulated object+mesh requires VkPhysicalDeviceFeatures.multiDrawIndirect"];
                return;
            }
            // shortcut: emulated indirect Object+Mesh caps GPU-written object groups to
            // RDNA4MESH_EMUL_INDIRECT_MAX_GROUPS (1024) until kext MEC gang submit enables taskShader=true.
            pst = rdna4mesh_plan_emulated_indirect_draw(
                payloadStride, caps.buffer_length, RDNA4R.lim.maxStorageBufferRange,
                maxDrawInd, RDNA4R.lim.minStorageBufferOffsetAlignment,
                RDNA4MESH_EMUL_INDIRECT_MAX_GROUPS, &emulPlan, &why);
        } else {
            pst = rdna4mesh_plan_emulated_draw(
                (uint64_t)grid.width, (uint64_t)grid.height, (uint64_t)grid.depth,
                payloadStride, caps.buffer_length, RDNA4R.lim.maxStorageBufferRange,
                maxDrawInd, RDNA4R.lim.minStorageBufferOffsetAlignment,
                &emulPlan, &why);
        }
        if (pst != RDNA4MESH_OK) {
            [_cb rdna4Fail:[NSString stringWithFormat:@"emulated object+mesh scratch planning failed: %s",
                            why ? why : rdna4mesh_status_str(pst)]];
            return;
        }
        if (emulPlan.is_noop) return;

        NSError *be = nil;
        emulScratch = [[RDNA4Buffer alloc] initWithDevice:dev length:(NSUInteger)emulPlan.total_scratch_bytes
                                                  options:MTLResourceStorageModePrivate error:&be];
        if (!emulScratch) {
            [_cb rdna4Fail:[NSString stringWithFormat:@"emulated object+mesh scratch buffer allocation failed: %@",
                            be.localizedDescription ?: @"OOM"]];
            return;
        }
        [_cb rdna4RetainEncoded:emulScratch];

        // Object compute must execute outside any Vulkan render pass; continuation pass reloads attachments (LOAD_OP_LOAD).
        if (_inPass) [self rdna4EndPass];
        if ([mpso rdna4NpbO]) rdna4_each_tex([mpso rdna4PbO], [mpso rdna4NpbO], &ms->objState, dev, needTex);

        VkBuffer scratchVk = [emulScratch vkBuffer];
        uint64_t scratchBase = [emulScratch rdna4VkOffset];
        if (isIndirect) {
            [_cb rdna4RetainEncoded:indirectBuf];
            rdna4_full_barrier(cmd);
            VkBufferCopy copyReg = { .srcOffset = userIndVkOff, .dstOffset = scratchBase + emulPlan.grid_params_offset, .size = RDNA4MESH_INDIRECT_CMD_BYTES };
            vkCmdCopyBuffer(cmd, [(RDNA4Buffer *)indirectBuf vkBuffer], scratchVk, 1u, &copyReg);
            vkCmdFillBuffer(cmd, scratchVk, scratchBase + emulPlan.grid_params_offset + 12u, 4u, emulPlan.grid_params.payload_stride_bytes);
            emulGpBuf = emulScratch;
            emulGpVkOff = scratchBase + emulPlan.grid_params_offset;
        } else {
            NSUInteger gpRingOff = 0;
            if (![_cb rdna4Bytes:&emulPlan.grid_params length:sizeof(emulPlan.grid_params) buffer:&emulGpBuf offset:&gpRingOff])
                return;
            emulGpVkOff = [emulGpBuf rdna4VkOffset] + gpRingOff;
        }
        vkCmdFillBuffer(cmd, scratchVk, scratchBase + emulPlan.indirect_offset, emulPlan.indirect_fill_bytes, 0u);

        // Barrier: transfer fills/copies -> Object compute + indirect dispatch read.
        VkMemoryBarrier t2c = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT,
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                             0, 1u, &t2c, 0u, NULL, 0u, NULL);

        VkDescriptorSet objDs = [_cb rdna4AllocSet:[mpso rdna4ObjectComputeDsl]];
        if (!objDs) return;
        if ([mpso rdna4NpbO] &&
            ![_cb rdna4FillSet:objDs bindings:[mpso rdna4PbO] count:[mpso rdna4NpbO] state:&ms->objState stage:"object"])
            return;
        rdna4_mesh_update_emul_descriptors(objDs, [mpso rdna4EmulObjMask], emulGpBuf, emulGpVkOff, emulScratch, &emulPlan);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, objCompPipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, [mpso rdna4ObjectComputeLayout], 0u, 1u, &objDs, 0u, NULL);
        if (isIndirect) {
            vkCmdDispatchIndirect(cmd, [(RDNA4Buffer *)indirectBuf vkBuffer], userIndVkOff);
        } else {
            vkCmdDispatch(cmd, (uint32_t)grid.width, (uint32_t)grid.height, (uint32_t)grid.depth);
        }

        // Barrier: Object compute writes (payload + indirect commands + user buffers) -> Mesh indirect draw & shader reads.
        VkMemoryBarrier c2m = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_MESH_SHADER_BIT_EXT |
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 1u, &c2m, 0u, NULL, 0u, NULL);
    } else if (isIndirect) {
        if (_inPass) [self rdna4EndPass];
        rdna4_full_barrier(cmd);
    }

    // 5. Transition textures for native Object, Mesh, and Fragment stages outside the render pass.
    RDNA4RenderScratch *encSc = [self rdna4EnsureScratch];
    if (mode == RDNA4MESH_MODE_NATIVE_TASK_MESH && [mpso rdna4NpbO])
        rdna4_each_tex([mpso rdna4PbO], [mpso rdna4NpbO], &ms->objState, dev, needTex);
    if ([mpso rdna4NpbM])
        rdna4_each_tex([mpso rdna4PbM], [mpso rdna4NpbM], &ms->meshState, dev, needTex);
    if (hasFrag && [mpso npbF])
        rdna4_each_tex([mpso pbF], [mpso npbF], &encSc->sf, dev, needTex);

    if (hasFrag && [mpso argumentTables]->active) {
        NSMutableSet *argRes = [_argumentResources mutableCopy] ?: [NSMutableSet set];
        NSMutableSet *argW = [_argumentWritable mutableCopy] ?: [NSMutableSet set];
        rdna4_argument_stage_resources(argRes, argW, [mpso pbF], [mpso npbF], &encSc->sf, nil);
        RDNA4ArgumentScanPrepared scan = {0};
        const RDNA4StageState *scanStages[1] = { &encSc->sf };
        NSString *scanErr = nil;
        if (!rdna4_argument_scan_prepare(&scan, scanStages, 1u, argW, dev, &scanErr)) {
            [_cb rdna4Fail:scanErr ?: @"mesh fragment argument scan failed"];
            return;
        }
        BOOL ready = [_cb rdna4PrepareArguments:[mpso argumentTables] resources:argRes writable:argW scan:&scan textures:needTex];
        rdna4_argument_scan_prepared_destroy(&scan);
        if (!ready) return;
    }

    if ([mpso fetch] != _fetch) {
        if ([mpso fetch] && [mpso fetchMax] >= _na) {
            [_cb rdna4Fail:[NSString stringWithFormat:@"mesh pipeline reads [[color(%u)]] past %u attachments", [mpso fetchMax], _na]];
            return;
        }
        if (_inPass) [self rdna4EndPass];
        _fetch = [mpso fetch];
    }

    // 6. Begin (or resume with LOAD_OP_LOAD) the render pass and bind mesh graphics state.
    [self rdna4BeginPass];
    if ([_cb rdna4Error]) return;
    [self rdna4OccDraw];
    if (_fetch) {
        VkMemoryBarrier mb = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_DEPENDENCY_BY_REGION_BIT, 1u, &mb, 0u, NULL, 0u, NULL);
    }

    if (_dcOn) rdna4dc_enc_reset_bound(&encSc->dc);
    _drawDirty = GARNET_DRAW_DIRTY_ALL;
    _cleanFastOk = NO;
    _vbDirtyMask = ~0u;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gfxPipe);

    VkViewport vps[16]; VkRect2D scs[16];
    for (uint32_t i = 0; i < vpCount; i++) {
        uint32_t vi = i + vpOff, si = i + vpOff;
        MTLViewport mv = (_nvp <= 1u || !_extraVps) ? _vp : (vi < _nvp ? _extraVps[vi] : _extraVps[_nvp - 1u]);
        MTLScissorRect sc = si < _nsr ? ((_nsr <= 1u || !_extraSrs) ? _sr : _extraSrs[si]) : (MTLScissorRect){ 0, 0, _w, _h };
        vps[i] = (VkViewport){ (float)mv.originX, (float)(mv.originY + mv.height), (float)mv.width, -(float)mv.height, (float)mv.znear, (float)mv.zfar };
        scs[i] = (VkRect2D){ { (int32_t)sc.x, (int32_t)sc.y }, { (uint32_t)sc.width, (uint32_t)sc.height } };
    }
    vkCmdSetViewport(cmd, 0u, vpCount, vps);
    vkCmdSetScissor(cmd, 0u, vpCount, scs);
    vkCmdSetBlendConstants(cmd, _blend);
    if (_hasSamplePos && rdna4_vkCmdSetSampleLocationsEXT) {
        VkSampleLocationEXT vkSL[8];
        for (uint32_t s = 0; s < _samplePosCount && s < 8; s++) vkSL[s] = (VkSampleLocationEXT){ _samplePos[s].x, _samplePos[s].y };
        VkSampleLocationsInfoEXT slInfo = {
            .sType = VK_STRUCTURE_TYPE_SAMPLE_LOCATIONS_INFO_EXT,
            .sampleLocationsPerPixel = (VkSampleCountFlagBits)_encSamples,
            .sampleLocationGridSize = { 1, 1 },
            .sampleLocationsCount = _samplePosCount,
            .pSampleLocations = vkSL,
        };
        rdna4_vkCmdSetSampleLocationsEXT(cmd, &slInfo);
    }
    if ([mpso rdna4HasDS]) {
        vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_BIT, _dsi.fRead);
        vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_BACK_BIT, _dsi.bRead);
        vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_BIT, _dsi.fWrite);
        vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_BACK_BIT, _dsi.bWrite);
        vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_BIT, _sref[0]);
        vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_BACK_BIT, _sref[1]);
        if ([mpso rdna4HasDepth]) vkCmdSetDepthBias(cmd, _bias[0], _bias[1], _bias[2]);
    }

    [_cb rdna4BindArguments:[mpso argumentTables] point:VK_PIPELINE_BIND_POINT_GRAPHICS layout:[mpso layout]];
    if ([mpso rdna4NpbM] || [mpso rdna4EmulMeshMask]) {
        VkDescriptorSet meshDs = [_cb rdna4AllocSet:[mpso dsl:0]];
        if (!meshDs) return;
        if ([mpso rdna4NpbM] &&
            ![_cb rdna4FillSet:meshDs bindings:[mpso rdna4PbM] count:[mpso rdna4NpbM] state:&ms->meshState stage:"mesh"])
            return;
        if ([mpso rdna4EmulMeshMask]) {
            rdna4_mesh_update_emul_descriptors(meshDs, [mpso rdna4EmulMeshMask], emulGpBuf, emulGpVkOff, emulScratch, &emulPlan);
        }
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, [mpso layout], 0u, 1u, &meshDs, 0u, NULL);
    }
    if (hasFrag && [mpso npbF]) {
        VkDescriptorSet fragDs = [_cb rdna4AllocSet:[mpso dsl:1]];
        if (!fragDs || ![_cb rdna4FillSet:fragDs bindings:[mpso pbF] count:[mpso npbF] state:&encSc->sf stage:"mesh-fragment"])
            return;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, [mpso layout], 1u, 1u, &fragDs, 0u, NULL);
    }
    if (mode == RDNA4MESH_MODE_NATIVE_TASK_MESH && [mpso rdna4NpbO]) {
        VkDescriptorSet objDs = [_cb rdna4AllocSet:[mpso rdna4ObjectDsl]];
        if (!objDs || ![_cb rdna4FillSet:objDs bindings:[mpso rdna4PbO] count:[mpso rdna4NpbO] state:&ms->objState stage:"object"])
            return;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, [mpso layout], [mpso rdna4ObjectSet], 1u, &objDs, 0u, NULL);
    }

    // 7. Issue VK_EXT_mesh_shader draw.
    if (mode == RDNA4MESH_MODE_EMULATED_OBJECT_MESH) {
        RDNA4MeshFn.cmdDrawMeshTasksIndirectEXT(
            cmd, [emulScratch vkBuffer],
            [emulScratch rdna4VkOffset] + emulPlan.indirect_offset,
            emulPlan.total_object_groups,
            RDNA4MESH_INDIRECT_CMD_BYTES);
    } else if (isIndirect) {
        [_cb rdna4RetainEncoded:indirectBuf];
        RDNA4MeshFn.cmdDrawMeshTasksIndirectEXT(
            cmd, [(RDNA4Buffer *)indirectBuf vkBuffer], userIndVkOff, 1u, RDNA4MESH_INDIRECT_CMD_BYTES);
    } else {
        RDNA4MeshFn.cmdDrawMeshTasksEXT(cmd, (uint32_t)grid.width, (uint32_t)grid.height, (uint32_t)grid.depth);
    }
}

- (void)drawMeshThreadgroups:(MTLSize)threadgroupsPerGrid
 threadsPerObjectThreadgroup:(MTLSize)threadsPerObjectThreadgroup
   threadsPerMeshThreadgroup:(MTLSize)threadsPerMeshThreadgroup {
    [self rdna4DrawMeshGroups:threadgroupsPerGrid
                     objLocal:threadsPerObjectThreadgroup
                    meshLocal:threadsPerMeshThreadgroup
                     indirect:NO indirectBuffer:nil indirectOffset:0];
}

- (void)drawMeshThreads:(MTLSize)threadsPerGrid
threadsPerObjectThreadgroup:(MTLSize)threadsPerObjectThreadgroup
threadsPerMeshThreadgroup:(MTLSize)threadsPerMeshThreadgroup {
    if (_hostWriteDropped) return;
    if (_ended) { [_cb rdna4Fail:@"drawMeshThreads after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    if (!_pso || ![_pso rdna4IsMeshPipeline]) {
        [_cb rdna4Fail:@"drawMeshThreads: requires a mesh render pipeline state"];
        return;
    }
    RDNA4MeshRenderPipelineState *mpso = (RDNA4MeshRenderPipelineState *)_pso;
    MTLSize activeTg = [mpso rdna4HasObjectStage] ? threadsPerObjectThreadgroup : threadsPerMeshThreadgroup;
    uint64_t groups[3] = { 0u, 0u, 0u };
    int isNoop = 0;
    const char *why = NULL;
    rdna4mesh_status_t st = rdna4mesh_threads_to_threadgroups(
        (uint64_t)threadsPerGrid.width, (uint64_t)threadsPerGrid.height, (uint64_t)threadsPerGrid.depth,
        (uint64_t)activeTg.width, (uint64_t)activeTg.height, (uint64_t)activeTg.depth,
        groups, &isNoop, &why);
    if (st != RDNA4MESH_OK) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"drawMeshThreads: %s", why ? why : rdna4mesh_status_str(st)]];
        return;
    }
    if (isNoop) return;
    [self rdna4DrawMeshGroups:MTLSizeMake((NSUInteger)groups[0], (NSUInteger)groups[1], (NSUInteger)groups[2])
                     objLocal:threadsPerObjectThreadgroup
                    meshLocal:threadsPerMeshThreadgroup
                     indirect:NO indirectBuffer:nil indirectOffset:0];
}

- (void)drawMeshThreadgroupsWithIndirectBuffer:(id<MTLBuffer>)indirectBuffer
                          indirectBufferOffset:(NSUInteger)indirectBufferOffset
                   threadsPerObjectThreadgroup:(MTLSize)threadsPerObjectThreadgroup
                     threadsPerMeshThreadgroup:(MTLSize)threadsPerMeshThreadgroup {
    [self rdna4DrawMeshGroups:MTLSizeMake(0, 0, 0)
                     objLocal:threadsPerObjectThreadgroup
                    meshLocal:threadsPerMeshThreadgroup
                     indirect:YES indirectBuffer:indirectBuffer indirectOffset:indirectBufferOffset];
}

@end

#endif // RDNA4_MESH_IMPL_INC
