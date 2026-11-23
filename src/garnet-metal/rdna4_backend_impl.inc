// ---------------------------------------------------------------------------------------------------------------
// Lazy in-process RADV (NATIVE-S4-M10 route a', F5). Opened on the first resource/queue request, NEVER in
// initWithAcceleratorPort: (every Metal-enumerating process loads this bundle). RDNA4N is root-only and exclusive:
// a refusal (non-root, second client) becomes nil + NSError, never an abort.
// ---------------------------------------------------------------------------------------------------------------
#define RDNA4_VK_FUNCS(X) \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceFeatures) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkCreateDevice) \
    X(vkGetDeviceQueue) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) \
    X(vkCreateBufferView) X(vkDestroyBufferView) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkBindBufferMemory) X(vkMapMemory) X(vkUnmapMemory) \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkFreeCommandBuffers) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkQueueSubmit) X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) \
    X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateRenderPass) X(vkDestroyRenderPass) \
    X(vkCreateFramebuffer) X(vkDestroyFramebuffer) X(vkCreateShaderModule) X(vkDestroyShaderModule) \
    X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkGetDescriptorSetLayoutSupport) X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) \
    X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdBindPipeline) X(vkCmdSetViewport) X(vkCmdSetScissor) \
    X(vkCmdDraw) X(vkCmdDrawIndirect) X(vkCmdDrawIndexedIndirect) X(vkCmdPipelineBarrier) X(vkCmdCopyImageToBuffer) X(vkCmdCopyBuffer) X(vkCmdFillBuffer) \
    X(vkCreateSampler) X(vkDestroySampler) X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) \
    X(vkCmdBindDescriptorSets) X(vkCmdBindVertexBuffers) X(vkCmdBindIndexBuffer) X(vkCmdDrawIndexed) X(vkCmdCopyBufferToImage) X(vkCmdPushConstants) \
    X(vkCmdDispatch) X(vkCmdDispatchIndirect) X(vkCreateComputePipelines) X(vkCmdSetBlendConstants) X(vkCmdCopyImage) X(vkCmdBlitImage) X(vkResetFences) \
    X(vkEnumerateDeviceExtensionProperties) X(vkGetPhysicalDeviceFeatures2) \
    X(vkGetImageSubresourceLayout) X(vkGetPhysicalDeviceProperties2) X(vkGetPhysicalDeviceImageFormatProperties2) X(vkGetDeviceProcAddr) \
    X(vkGetPhysicalDeviceFormatProperties) X(vkGetPhysicalDeviceImageFormatProperties) \
    X(vkCreatePipelineCache) X(vkGetPipelineCacheData) \
    X(vkCreateQueryPool) X(vkDestroyQueryPool) X(vkCmdResetQueryPool) X(vkCmdBeginQuery) X(vkCmdEndQuery) X(vkGetQueryPoolResults) X(vkGetFenceStatus) /* build 18 (P2) */ \
    X(vkCmdResolveImage) X(vkCmdSetStencilReference) X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask) X(vkCmdSetDepthBias)
#define X(n) static PFN_##n n;
RDNA4_VK_FUNCS(X)
#undef X
static PFN_vkCreateInstance rdna4_vkCreateInstance;
static PFN_vkGetInstanceProcAddr rdna4_gipa;
static PFN_vkGetMemoryHostPointerPropertiesEXT rdna4_vkGetMHPP;   // 11h.6: VK_EXT_external_memory_host
static PFN_vkGetBufferDeviceAddress rdna4_vkGetBufferAddress;
static PFN_vkQueueWaitIdle rdna4_vkQueueWaitIdle;
static RDNA4NativeBridge rdna4_native_bridge;

static struct {
    BOOL tried; BOOL ok; NSError *err;
    void *lib; VkInstance inst; VkPhysicalDevice pd; VkDevice dev; VkQueue q; uint32_t qfi;
    VkPhysicalDeviceMemoryProperties mp; pthread_mutex_t qlock; VkPhysicalDeviceLimits lim; VkCommandPool opool; VkPipelineCache pc;   // pc: T2 persisted pipeline cache (VK_NULL_HANDLE = none)
    BOOL occPrecise, occOK;   // build 18 (P2): occlusionQueryPrecise enabled on the device; every query entry point resolved
    BOOL dbClamp;   // bundle 10: depthBiasClamp enabled on the device (a non-zero clamp in setDepthBias is passed on only then)
    BOOL hostExt, drmModExt; uint64_t hostAlign; uint64_t impTotal;   // host pointer import and explicit DRM-modifier linear images
    BOOL orderedColorAccess, indirectFirstInstance, cubeArray, bufferAddress;
    BOOL tessellation; uint32_t maxTessGenLevel, maxTessPatchSize;
    BOOL descriptorIndexing, nonUniformSampledImages, nonUniformStorageImages, nonUniformBuffers;
    BOOL gpuArgumentTables, nullDescriptors, samplerAnisotropy, samplerMirrorClamp;
    BOOL robustTexelBuffers;
    BOOL uniformTexelBuffers, storageTexelBuffers;
    VkPhysicalDeviceVulkan12Properties descriptorLimits;
    uint32_t maxPerSetDescriptors;
} RDNA4R;

/*
 * One immutable pre-open limit source, independent of RDNA4R. Preparing it
 * may dlopen/dlsym the bundle dylib, but never creates a Vulkan object or
 * changes RDNA4R.tried/RDNA4R.ok. Getter and admission code only read the
 * frozen snapshot below.
 */
static rdna4_argument_limit_snapshot RDNA4ALS;
static dispatch_once_t rdna4_argument_limit_once;
static _Atomic int rdna4_argument_limit_full_open_state; /* -1 fail, 0 pending, 1 match */

static const char *rdna4_argument_limit_state_name(rdna4_argument_limit_state state) {
    switch (state) {
    case RDNA4_ARGUMENT_LIMIT_STATE_READY: return "ready";
    case RDNA4_ARGUMENT_LIMIT_STATE_UNAVAILABLE: return "unavailable";
    default: return "uninitialized";
    }
}

static void rdna4_argument_limit_prepare(const rdna4dc_native_memory *nativeMemory) {
    dispatch_once(&rdna4_argument_limit_once, ^{
        NSString *path = [[[NSBundle bundleForClass:[GarnetDevice class]] resourcePath]
            stringByAppendingPathComponent:@"libvulkan_radeon.dylib"];
        rdna4_argument_limit_policy policy = rdna4_argument_limit_default_policy();
        rdna4_radv_darwin_host_facts_v1 facts;
        memset(&facts, 0, sizeof(facts));
        facts.struct_size = sizeof(facts);
        const rdna4_radv_darwin_host_facts_v1 *factsInput = NULL;
        if (rdna4dc_native_memory_valid(nativeMemory) && nativeMemory->physical) {
            facts.valid_flags = RDNA4_RADV_DARWIN_FACT_VRAM_BYTES;
            facts.vram_bytes = nativeMemory->physical;
            factsInput = &facts;
        }
        rdna4_argument_limit_result result = rdna4_argument_limit_snapshot_init_from_path(
            &RDNA4ALS, path.fileSystemRepresentation, factsInput, &policy);
        RDNA4LOG("argument limits: source %s (result %d), export=%u ABI=%u struct=%u flags=0x%x valid=0x%x identity='%s' path='%s'%s%s",
                 rdna4_argument_limit_state_name(RDNA4ALS.state), result,
                 RDNA4ALS.export_present, RDNA4ALS.result_abi_version,
                 RDNA4ALS.result_struct_size, RDNA4ALS.result_flags,
                 RDNA4ALS.result_valid_flags, RDNA4ALS.build_identity,
                 RDNA4ALS.source_path,
                 RDNA4ALS.loader_error[0] ? " loader_error=" : "",
                 RDNA4ALS.loader_error);
        if (result == RDNA4_ARGUMENT_LIMIT_RESULT_OK) {
            RDNA4LOG("argument limits: public sampler count %u = min(table %u, object %u); descriptor C=%llu, sampler objects=%u, dummy/source reserve=%u, handles/config=%u",
                     RDNA4ALS.public_sampler_limit, RDNA4ALS.sampler_plan.table_limit,
                     RDNA4ALS.sampler_plan.object_limit,
                     (unsigned long long)RDNA4ALS.descriptor_capacity,
                     RDNA4ALS.raw.max_sampler_allocations,
                     RDNA4ALS.policy.reserve_sampler_objects,
                     RDNA4ALS.policy.objects_per_configuration);
        } else {
            RDNA4LOG("argument limits: preserving the Tier1 public contract; no 0/1/16/31/96/128 fallback is published");
        }
    });
}

static rdna4_argument_limit_open_observation rdna4_argument_limit_observation(
    const VkPhysicalDeviceProperties *pp,
    const VkPhysicalDeviceVulkan12Properties *v12,
    uint32_t maxPerSetDescriptors, uint64_t maxMemoryAllocationSize,
    uint64_t maxBufferSize) {
    rdna4_argument_limit_open_observation o;
    memset(&o, 0, sizeof(o));
    o.struct_size = sizeof(o);
    o.observed_flags = RDNA4_ARGUMENT_LIMIT_OBSERVED_CORE_MASK |
                       RDNA4_ARGUMENT_LIMIT_OBSERVED_MEMORY_LIMITS;
    o.api_version = pp->apiVersion;
    o.driver_version = pp->driverVersion;
    o.driver_id = v12->driverID;
    o.max_descriptor_set_size = pp->limits.maxFragmentCombinedOutputResources;
    o.max_sampler_allocations = pp->limits.maxSamplerAllocationCount;
    o.max_bound_descriptor_sets = pp->limits.maxBoundDescriptorSets;
    o.max_per_set_descriptors = maxPerSetDescriptors;
    o.max_storage_buffer_range = pp->limits.maxStorageBufferRange;
    o.max_uab_descriptors_all_pools = v12->maxUpdateAfterBindDescriptorsInAllPools;
    o.max_per_stage_descriptor_samplers = pp->limits.maxPerStageDescriptorSamplers;
    o.max_per_stage_descriptor_uniform_buffers = pp->limits.maxPerStageDescriptorUniformBuffers;
    o.max_per_stage_descriptor_storage_buffers = pp->limits.maxPerStageDescriptorStorageBuffers;
    o.max_per_stage_descriptor_sampled_images = pp->limits.maxPerStageDescriptorSampledImages;
    o.max_per_stage_descriptor_storage_images = pp->limits.maxPerStageDescriptorStorageImages;
    o.max_per_stage_descriptor_input_attachments = pp->limits.maxPerStageDescriptorInputAttachments;
    o.max_per_stage_resources = pp->limits.maxPerStageResources;
    o.max_descriptor_set_samplers = pp->limits.maxDescriptorSetSamplers;
    o.max_descriptor_set_uniform_buffers = pp->limits.maxDescriptorSetUniformBuffers;
    o.max_descriptor_set_uniform_buffers_dynamic = pp->limits.maxDescriptorSetUniformBuffersDynamic;
    o.max_descriptor_set_storage_buffers = pp->limits.maxDescriptorSetStorageBuffers;
    o.max_descriptor_set_storage_buffers_dynamic = pp->limits.maxDescriptorSetStorageBuffersDynamic;
    o.max_descriptor_set_sampled_images = pp->limits.maxDescriptorSetSampledImages;
    o.max_descriptor_set_storage_images = pp->limits.maxDescriptorSetStorageImages;
    o.max_descriptor_set_input_attachments = pp->limits.maxDescriptorSetInputAttachments;
    o.max_per_stage_descriptor_update_after_bind_samplers = v12->maxPerStageDescriptorUpdateAfterBindSamplers;
    o.max_per_stage_descriptor_update_after_bind_uniform_buffers = v12->maxPerStageDescriptorUpdateAfterBindUniformBuffers;
    o.max_per_stage_descriptor_update_after_bind_storage_buffers = v12->maxPerStageDescriptorUpdateAfterBindStorageBuffers;
    o.max_per_stage_descriptor_update_after_bind_sampled_images = v12->maxPerStageDescriptorUpdateAfterBindSampledImages;
    o.max_per_stage_descriptor_update_after_bind_storage_images = v12->maxPerStageDescriptorUpdateAfterBindStorageImages;
    o.max_per_stage_descriptor_update_after_bind_input_attachments = v12->maxPerStageDescriptorUpdateAfterBindInputAttachments;
    o.max_per_stage_update_after_bind_resources = v12->maxPerStageUpdateAfterBindResources;
    o.max_descriptor_set_update_after_bind_samplers = v12->maxDescriptorSetUpdateAfterBindSamplers;
    o.max_descriptor_set_update_after_bind_uniform_buffers = v12->maxDescriptorSetUpdateAfterBindUniformBuffers;
    o.max_descriptor_set_update_after_bind_uniform_buffers_dynamic = v12->maxDescriptorSetUpdateAfterBindUniformBuffersDynamic;
    o.max_descriptor_set_update_after_bind_storage_buffers = v12->maxDescriptorSetUpdateAfterBindStorageBuffers;
    o.max_descriptor_set_update_after_bind_storage_buffers_dynamic = v12->maxDescriptorSetUpdateAfterBindStorageBuffersDynamic;
    o.max_descriptor_set_update_after_bind_sampled_images = v12->maxDescriptorSetUpdateAfterBindSampledImages;
    o.max_descriptor_set_update_after_bind_storage_images = v12->maxDescriptorSetUpdateAfterBindStorageImages;
    o.max_descriptor_set_update_after_bind_input_attachments = v12->maxDescriptorSetUpdateAfterBindInputAttachments;
    o.max_update_after_bind_descriptors_in_all_pools = v12->maxUpdateAfterBindDescriptorsInAllPools;
    o.max_fragment_combined_output_resources = pp->limits.maxFragmentCombinedOutputResources;
    o.max_color_attachments = pp->limits.maxColorAttachments;
    o.max_fragment_output_attachments = pp->limits.maxFragmentOutputAttachments;
    o.max_fragment_dual_src_attachments = pp->limits.maxFragmentDualSrcAttachments;
    o.max_memory_allocation_size = maxMemoryAllocationSize;
    o.max_buffer_size = maxBufferSize;
    return o;
}

static int rdna4_argument_limit_verify_open(
    const VkPhysicalDeviceProperties *pp,
    const VkPhysicalDeviceVulkan12Properties *v12,
    uint32_t maxPerSetDescriptors, uint64_t maxMemoryAllocationSize,
    uint64_t maxBufferSize,
    const rdna4_argument_limit_snapshot *seen,
    rdna4_argument_limit_result seenResult, int sourceMatches) {
    if (RDNA4ALS.state != RDNA4_ARGUMENT_LIMIT_STATE_READY) {
        RDNA4LOG("argument limits: full-open crosscheck not applicable (source unavailable)");
        atomic_store(&rdna4_argument_limit_full_open_state, 0);
        return 0;
    }
    if (!sourceMatches || seenResult != RDNA4_ARGUMENT_LIMIT_RESULT_OK ||
        !seen || memcmp(&seen->raw, &RDNA4ALS.raw, sizeof(seen->raw))) {
        RDNA4LOG("argument limits: full-open same-handle identity/raw-limit MISMATCH (source %d, re-export %d); argument tables fail closed, snapshot remains frozen",
                 sourceMatches, seenResult);
        atomic_store(&rdna4_argument_limit_full_open_state, -1);
        return -1;
    }
    rdna4_argument_limit_open_observation o =
        rdna4_argument_limit_observation(
            pp, v12, maxPerSetDescriptors, maxMemoryAllocationSize,
            maxBufferSize);
    rdna4_argument_limit_result result =
        rdna4_argument_limit_snapshot_crosscheck(&RDNA4ALS, &o);
    if (result != RDNA4_ARGUMENT_LIMIT_RESULT_OK) {
        RDNA4LOG("argument limits: full-open identity/raw-limit MISMATCH (%d); argument tables fail closed, snapshot remains frozen", result);
        atomic_store(&rdna4_argument_limit_full_open_state, -1);
        return -1;
    }
    atomic_store(&rdna4_argument_limit_full_open_state, 1);
    RDNA4LOG("argument limits: full-open identity and all raw limits MATCH the pre-open snapshot");
    return 1;
}

static _Atomic int rdna4_submit_fault;
// A failed submit or fence wait does not prove GPU work stopped. Keep its objects
// until process exit and reject new submissions; there is no in-process reset.
static NSMutableArray *rdna4_unresolved_commands;

// Published once, only after a successful lazy open. Getters never open RADV or
// wait for its class lock, and cannot observe a failed/partially initialized try.
static rdna4dc_caps RDNA4Caps;
static _Atomic int rdna4_caps_ready;
static rdna4x_backend_capabilities RDNA4QueryCaps;
static rdna4x_backend_capabilities rdna4_query_caps(void) {
    if (atomic_load_explicit(&rdna4_caps_ready, memory_order_acquire)) return RDNA4QueryCaps;
    return rdna4x_query_caps(0, 0, 0, 0, 0);
}
static rdna4dc_caps rdna4_device_caps(void) {
    if (atomic_load_explicit(&rdna4_caps_ready, memory_order_acquire)) return RDNA4Caps;
    return rdna4dc_fallback();
}
static rdna4_gather_caps RDNA4GatherCaps;
static _Atomic int rdna4_gather_caps_ready;
static rdna4_gather_caps rdna4_device_gather_caps(void) {
    if (atomic_load_explicit(&rdna4_gather_caps_ready, memory_order_acquire)) return RDNA4GatherCaps;
    return (rdna4_gather_caps){0};
}

static NSError *rdna4_err(NSInteger code, NSString *msg) {
    return [NSError errorWithDomain:@"GarnetMetal" code:code userInfo:@{ NSLocalizedDescriptionKey: msg }];
}

static uint64_t rdna4_now(void);

// ---------------------------------------------------------------------------------------------------------------
// native #12 bundle work (lag investigation).
// T1: cheap always-on timing, aggregated per process and drained every 10 s by one timer (no per-event logging). All values ns.
//   spv   = rdna4_spv_lookup (sha256 + spvcache file read + meta)       shm  = vkCreateShaderModule
//   gfx   = vkCreateGraphicsPipelines   cmp = vkCreateComputePipelines   imp = IOSurface / NoCopy import (vkGetMemoryHostPointerProperties..vkAllocateMemory)
//   enc   = command buffer CPU time, creation (begin) to submit entry (includes app think time between encoders)
//   gpu   = submit -> fence signalled, seen by the completion thread (a lower bound on queueing: the completion queue is SERIAL, so it also holds
//           the head-of-line wait behind earlier fences)          wake = dispatch_async enqueue -> block start on the serial completion queue
//   qlk   = time blocked acquiring RDNA4R.qlock (every acquisition is timed, contended or not)
//   fgap  = gap between submits of consecutive "frame" command buffers (those that wrote a display surface: the D-copy identification)
//   fgpu  = submit -> fence for frame command buffers
// T2: a VkPipelineCache created at device open from /private/var/tmp (or /private/tmp) and saved there (WindowServer only).
// ---------------------------------------------------------------------------------------------------------------
static struct { rdna4t_hist spv, shm, gfx, cmp, imp, enc, gpu, wake, qlk, fgap, fgpu; } T1;
static _Atomic uint64_t rdna4_t1_frames, rdna4_t1_lastframe, rdna4_pipes_new;
static inline void rdna4_qlock(void) { uint64_t t_ = rdna4_now(); pthread_mutex_lock(&(RDNA4R.qlock)); rdna4t_add(&T1.qlk, rdna4_now() - t_); }
static void rdna4_quarantine(id resources, VkResult fault) {
    rdna4_qlock();
    atomic_store(&rdna4_submit_fault, (int)fault);
    if (!rdna4_unresolved_commands) rdna4_unresolved_commands = [NSMutableArray array];
    [rdna4_unresolved_commands addObject:resources];
    pthread_mutex_unlock(&RDNA4R.qlock);
    RDNA4LOG("GPU work unresolved (%d): retaining resources until process exit; further submissions disabled", fault);
}

static struct { char path[300]; uint8_t key[32], uuid[16]; uint32_t vendor, device; BOOL save; uint64_t lastHash, savedPipes; pthread_mutex_t mu; } RDNA4PC = { .mu = PTHREAD_MUTEX_INITIALIZER };

static BOOL rdna4_pc_derive_key(void) {
    NSBundle *b = [NSBundle bundleForClass:[GarnetDevice class]];
    NSString *lib = [[b resourcePath] stringByAppendingPathComponent:@"libvulkan_radeon.dylib"];
    NSData *backend = [NSData dataWithContentsOfFile:lib options:NSDataReadingMappedIfSafe error:NULL];
    if (!backend.length || backend.length > UINT32_MAX) return NO;
    // Vulkan keys pipeline inputs itself; a Metal bundle rebuild does not change cached RADV machine code.
    static const char domain[] = "rdna4-vulkan-pipeline-cache-v2";
    uint8_t material[sizeof domain - 1 + CC_SHA256_DIGEST_LENGTH];
    memcpy(material, domain, sizeof domain - 1);
    CC_SHA256(backend.bytes, (CC_LONG)backend.length, material + sizeof domain - 1);
    CC_SHA256(material, sizeof material, RDNA4PC.key);
    return YES;
}
// Called from rdna4_radv_open_once after the device exists (class lock held). Never fails the open: no cache = pipelines compile as before.
static void rdna4_pc_open(const VkPhysicalDeviceProperties *pp) {
    if (!vkCreatePipelineCache || !vkGetPipelineCacheData) { RDNA4LOG("T2 pipeline cache: entry points not exposed by RADV, running without"); return; }
    if (access("/private/tmp/rdna4m-nopcache", F_OK) == 0 || (rdna4_allow() && getenv("RDNA4M_NOPCACHE"))) { RDNA4LOG("T2 pipeline cache: disabled (kill file /private/tmp/rdna4m-nopcache or RDNA4M_NOPCACHE)"); return; }
    memcpy(RDNA4PC.uuid, pp->pipelineCacheUUID, 16); RDNA4PC.vendor = pp->vendorID; RDNA4PC.device = pp->deviceID;
    if (!rdna4_pc_derive_key()) { RDNA4LOG("T2 pipeline cache: backend identity unavailable, running without"); return; }
    // GPU-apps G4: WindowServer and RDNA4M_ALLOW root tools use /private/var/tmp/rdna4m-pipecache.bin as before; any other (admitted application) process uses a file of its OWN under the
    // user's cache directory (confstr _CS_DARWIN_USER_CACHE_DIR, the one cache directory a sandboxed GPU process may write) and has NO cache if that directory is unavailable (rdna4_gate.h).
    const int pcls = rdna4g_class(rdna4_is_ws(), rdna4_allow());
    char ucd[1024]; ucd[0] = 0;
    if (pcls == RDNA4G_CLASS_OTHER) { const size_t cn = confstr(_CS_DARWIN_USER_CACHE_DIR, ucd, sizeof ucd); if (cn == 0 || cn > sizeof ucd) ucd[0] = 0; }
    const char *envdir = (rdna4_allow() && getenv("RDNA4M_PCACHE_DIR")) ? getenv("RDNA4M_PCACHE_DIR") : NULL;
    const int vtw = (pcls != RDNA4G_CLASS_OTHER) ? (access("/private/var/tmp", W_OK) == 0) : 0;
    if (!rdna4g_pcache_path(RDNA4PC.path, sizeof RDNA4PC.path, pcls, envdir, vtw, ucd, getprogname())) { RDNA4LOG("T2 pipeline cache: no cache directory for this process (user cache dir unavailable), running without"); return; }
    RDNA4PC.save = rdna4g_pcache_save(pcls, (rdna4_allow() && getenv("RDNA4M_PCACHE_SAVE")) ? 1 : 0) ? YES : NO;   // WindowServer and an application write (its own file); a root tool only with RDNA4M_PCACHE_SAVE: its file would block WindowServer's rename in the sticky dir
    char marker[sizeof RDNA4PC.path + 16]; snprintf(marker, sizeof marker, "%s.loading", RDNA4PC.path);
    if (access(marker, F_OK) == 0) { unlink(RDNA4PC.path); unlink(marker); RDNA4LOG("T2 pipeline cache: a previous load never finished (%s present): cache file discarded", marker); }
    void *buf = NULL; size_t flen = 0; const void *payload = NULL; size_t plen = 0; int c = -1;
    int fd = open(RDNA4PC.path, O_RDONLY);
    if (fd >= 0) {
        struct stat st;
        if (fstat(fd, &st) == 0 && st.st_size > 0 && (uint64_t)st.st_size <= RDNA4PC_MAX_PAYLOAD + sizeof(rdna4pc_hdr) && (buf = malloc((size_t)st.st_size))) {
            size_t got = 0; while (got < (size_t)st.st_size) { ssize_t n = read(fd, (char *)buf + got, (size_t)st.st_size - got); if (n <= 0) break; got += (size_t)n; }
            flen = got;
        }
        close(fd);
        if (buf) { c = rdna4pc_check(buf, flen, RDNA4PC.key, RDNA4PC.uuid, RDNA4PC.vendor, RDNA4PC.device, &payload, &plen);
                   if (c) { RDNA4LOG("T2 pipeline cache: %s ignored: %s", RDNA4PC.path, rdna4pc_why(c)); payload = NULL; plen = 0; } }
        else RDNA4LOG("T2 pipeline cache: %s unreadable/empty/oversized, ignored", RDNA4PC.path);
    } else RDNA4LOG("T2 pipeline cache: no file %s yet (cold start; saving %s)", RDNA4PC.path, RDNA4PC.save ? "ON" : "OFF in this process");
    VkPipelineCacheCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, .initialDataSize = plen, .pInitialData = payload };
    int mk = -1; if (payload) mk = open(marker, O_WRONLY | O_CREAT, 0644);
    uint64_t t0 = rdna4_now();
    VkResult r = vkCreatePipelineCache(RDNA4R.dev, &ci, NULL, &RDNA4R.pc);
    uint64_t dt = rdna4_now() - t0;
    if (mk >= 0) { close(mk); unlink(marker); }
    if (r != VK_SUCCESS && payload) {   // RADV refused the data: start empty
        RDNA4LOG("T2 pipeline cache: vkCreatePipelineCache(initialData %zu B) = %d, retrying empty", plen, r);
        ci.initialDataSize = 0; ci.pInitialData = NULL; payload = NULL; r = vkCreatePipelineCache(RDNA4R.dev, &ci, NULL, &RDNA4R.pc);
    }
    if (r != VK_SUCCESS) { RDNA4R.pc = VK_NULL_HANDLE; RDNA4LOG("T2 pipeline cache: vkCreatePipelineCache = %d, running without", r); free(buf); return; }
    if (payload) RDNA4PC.lastHash = rdna4pc_fnv(payload, plen);
    RDNA4LOG("T2 pipeline cache: %p created from %s in %.2f ms (initialData %zu B%s); key %02x%02x%02x%02x uuid %02x%02x%02x%02x", (void *)RDNA4R.pc, RDNA4PC.path, (double)dt / 1e6, plen, payload ? ", ACCEPTED" : ", none",
           RDNA4PC.key[0], RDNA4PC.key[1], RDNA4PC.key[2], RDNA4PC.key[3], RDNA4PC.uuid[0], RDNA4PC.uuid[1], RDNA4PC.uuid[2], RDNA4PC.uuid[3]);
    free(buf);
}
static void rdna4_pc_save(const char *why) {
    if (!RDNA4R.ok || !RDNA4R.pc || !RDNA4PC.save || !RDNA4PC.path[0]) return;
    uint64_t np = atomic_load(&rdna4_pipes_new);
    pthread_mutex_lock(&RDNA4PC.mu);
    if (np == RDNA4PC.savedPipes) { pthread_mutex_unlock(&RDNA4PC.mu); return; }
    RDNA4PC.savedPipes = np;   // retried only when another pipeline appears (no log spam on a persistent write failure)
    uint64_t t0 = rdna4_now(); size_t sz = 0; uint8_t *buf = NULL;
    if (vkGetPipelineCacheData(RDNA4R.dev, RDNA4R.pc, &sz, NULL) == VK_SUCCESS && sz >= 32 && sz <= RDNA4PC_MAX_PAYLOAD && (buf = malloc(sizeof(rdna4pc_hdr) + sz))) {
        VkResult r = vkGetPipelineCacheData(RDNA4R.dev, RDNA4R.pc, &sz, buf + sizeof(rdna4pc_hdr));
        if (r == VK_SUCCESS && rdna4pc_vk_header_ok(buf + sizeof(rdna4pc_hdr), sz, RDNA4PC.uuid, RDNA4PC.vendor, RDNA4PC.device)) {
            uint64_t h = rdna4pc_fnv(buf + sizeof(rdna4pc_hdr), sz);
            if (h == RDNA4PC.lastHash) { free(buf); pthread_mutex_unlock(&RDNA4PC.mu); return; }   // nothing new in the cache
            rdna4pc_hdr hd; rdna4pc_fill(&hd, RDNA4PC.key, RDNA4PC.uuid, RDNA4PC.vendor, RDNA4PC.device, buf + sizeof hd, sz); memcpy(buf, &hd, sizeof hd);
            char tmp[sizeof RDNA4PC.path + 24]; snprintf(tmp, sizeof tmp, "%s.tmp.%d", RDNA4PC.path, (int)getpid());
            int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644); BOOL ok = NO; int en = 0;
            if (fd >= 0) {
                size_t put = 0, tot = sizeof hd + sz; while (put < tot) { ssize_t n = write(fd, buf + put, tot - put); if (n <= 0) break; put += (size_t)n; }
                ok = put == tot && fsync(fd) == 0; en = errno; close(fd);
                if (ok && rename(tmp, RDNA4PC.path) != 0) { ok = NO; en = errno; }
                if (!ok) unlink(tmp);
            } else en = errno;
            if (ok) { RDNA4PC.lastHash = h; RDNA4LOG("T2 pipeline cache: SAVED %zu B to %s (%s; %llu pipelines created so far) in %.2f ms", sizeof hd + sz, RDNA4PC.path, why, (unsigned long long)np, (double)(rdna4_now() - t0) / 1e6); }
            else RDNA4LOG("T2 pipeline cache: save to %s FAILED (errno %d)", RDNA4PC.path, en);
        } else RDNA4LOG("T2 pipeline cache: vkGetPipelineCacheData = %d / header check failed, not saved", r);
    }
    free(buf);
    pthread_mutex_unlock(&RDNA4PC.mu);
}
static void rdna4_pc_atexit(void) { rdna4_pc_save("exit"); }

static void rdna4_pool_latch(void);
static void rdna4_m6_report(void);
static void rdna4_imp_latch(void);
static void rdna4_imp_tick(void);
static void rdna4_imp_report(unsigned tick);
static void rdna4_led_report(unsigned tick);
static void rdna4_pool_tick(void);
static void rdna4_pool_report(unsigned tick);
static void rdna4_drawopt_latch(void);
static void rdna4_drawopt_report(unsigned tick);
static void rdna4_t1_report(void) {
    static unsigned tick;
    struct { const char *n; rdna4t_hist *h; rdna4t_snap s; } m[] = { { "spvcache_read", &T1.spv, {0} }, { "shader_module", &T1.shm, {0} }, { "gfx_pipeline", &T1.gfx, {0} }, { "comp_pipeline", &T1.cmp, {0} }, { "import", &T1.imp, {0} },
        { "cb_encode", &T1.enc, {0} }, { "cb_gpu(submit->done)", &T1.gpu, {0} }, { "completion_wake", &T1.wake, {0} }, { "qlock_wait", &T1.qlk, {0} }, { "frame_gap", &T1.fgap, {0} }, { "frame_gpu", &T1.fgpu, {0} } };
    unsigned any = 0; char line[640];
    for (size_t i = 0; i < sizeof m / sizeof m[0]; i++) {
        rdna4t_take(m[i].h, &m[i].s);
        if (rdna4t_fmt(m[i].n, &m[i].s, line, sizeof line)) { any++; RDNA4LOG("%s [%s pid %d, 10 s window]", line, getprogname(), (int)getpid()); }
    }
    uint64_t fr = atomic_exchange(&rdna4_t1_frames, 0);
    if (any || fr) RDNA4LOG("T1 counts/10s [%s pid %d]: pipelines gfx=%llu comp=%llu imports=%llu command_buffers=%llu frames=%llu (frame = command buffer that wrote a display surface)", getprogname(), (int)getpid(),
        (unsigned long long)m[2].s.n, (unsigned long long)m[3].s.n, (unsigned long long)m[4].s.n, (unsigned long long)m[5].s.n, (unsigned long long)fr);
    else if (tick % 6 == 5) RDNA4LOG("T1 idle: no pipeline/import/command buffer in the last 60 s [%s pid %d]", getprogname(), (int)getpid());
    rdna4_pool_report(tick);   // P1: one line when the pool is ON and something happened (nothing when OFF)
    rdna4_drawopt_report(tick);   // P5b: one line when the drawopt switch is ON and something happened (nothing when OFF)
    rdna4_m6_report();   // bundle 11: one line when the kernel latch garnet-m6 is ON (nothing when OFF)
    rdna4_pool_tick();
    rdna4_imp_report(tick); rdna4_imp_tick();   // P4
    rdna4_led_report(tick);   // build 16 (P5 Step 0): the live-import ledger, one line per tick while any surface texture is alive
    if (++tick % 3 == 0) rdna4_pc_save("periodic");
}
static void rdna4_t1_start(void) {
    static dispatch_source_t src;
    if (src) return;
    src = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
    dispatch_source_set_timer(src, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC), 10 * NSEC_PER_SEC, 500 * NSEC_PER_MSEC);
    dispatch_source_set_event_handler(src, ^{ rdna4_t1_report(); });
    dispatch_resume(src);
    atexit(rdna4_pc_atexit);
    RDNA4LOG("T1 timing: summaries every 10 s (pid %d %s)", (int)getpid(), getprogname());
}
// RDNA4M_TEST_OPEN_REFUSE=<n> (needs RDNA4M_ALLOW): the first n open attempts fail like a refused RDNA4N open, without touching Vulkan.
static BOOL rdna4_test_refuse(void) {
    static long left = -1;
    if (left < 0) { const char *e = getenv("RDNA4M_TEST_OPEN_REFUSE"); left = (rdna4_allow() && e) ? atol(e) : 0; }
    if (left > 0) { left--; return YES; }
    return NO;
}
static void rdna4s_prebind_on_radv_open(void);
// One open attempt (#12 R4: the retry policy is in rdna4_radv_open). Returns YES when the RADV device + queue are up; on failure *err
// is set. Called with the class lock held.
static BOOL rdna4_radv_open_once(NSError **err) {
    rdna4_argument_limit_snapshot seen;
    memset(&seen, 0, sizeof(seen));
    rdna4_argument_limit_result seenResult =
        RDNA4_ARGUMENT_LIMIT_RESULT_CROSSCHECK_UNAVAILABLE;
    int sourceMatches = 0;
    {
        NSString *dir = [[NSBundle bundleForClass:[GarnetDevice class]] resourcePath];
        NSString *path = [dir stringByAppendingPathComponent:@"libvulkan_radeon.dylib"];
        RDNA4LOG("radv open: dlopen %s", path.UTF8String);
        void *h = dlopen(path.UTF8String, RTLD_NOW | RTLD_LOCAL);
        if (!h) { RDNA4R.err = rdna4_err(1, [NSString stringWithFormat:@"dlopen %@: %s", path, dlerror()]); goto fail; }
        RDNA4R.lib = h;
        if (RDNA4ALS.state == RDNA4_ARGUMENT_LIMIT_STATE_READY) {
            sourceMatches = rdna4_argument_limit_snapshot_source_matches_path(
                &RDNA4ALS, path.fileSystemRepresentation) ==
                RDNA4_ARGUMENT_LIMIT_RESULT_OK;
            if (sourceMatches) {
                seenResult = rdna4_argument_limit_snapshot_init_from_handle(
                    &seen, h, path.fileSystemRepresentation,
                    RDNA4ALS.facts_present ? &RDNA4ALS.input_facts : NULL,
                    &RDNA4ALS.policy);
            }
        }
        rdna4_gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vk_icdGetInstanceProcAddr");
        if (!rdna4_gipa) { RDNA4R.err = rdna4_err(2, @"no vk_icdGetInstanceProcAddr in RADV"); goto fail; }
        {   // G6: an admitted APPLICATION opens its native connection on the accelerator (a sandboxed app may open user clients only on an IOAccelerator service); WindowServer and root tools keep Mesa's default and no
            // setter is called. Before vkCreateInstance: the connection is opened when the physical devices are enumerated. Fail closed: a RADV without the setter cannot be used for an application.
            const char *svcCls = rdna4g_mesa_service_for_class(rdna4g_class(rdna4_is_ws(), rdna4_allow()));
            if (svcCls) {
                int (*setSvc)(const char *) = (int (*)(const char *))dlsym(h, RDNA4G_MESA_SETTER);
                if (!setSvc) { RDNA4R.err = rdna4_err(7, @"no " RDNA4G_MESA_SETTER " in RADV (an application needs the accelerator route)"); goto fail; }
                const int src = setSvc(svcCls);
                if (src != 0) { RDNA4R.err = rdna4_err(7, [NSString stringWithFormat:@RDNA4G_MESA_SETTER "(%s) = %d", svcCls, src]); goto fail; }
                RDNA4LOG("radv open: application class: RDNA4N is opened on %s", svcCls);
            }
        }
        rdna4_vkCreateInstance = (PFN_vkCreateInstance)rdna4_gipa(NULL, "vkCreateInstance");
        if (!rdna4_vkCreateInstance) { RDNA4R.err = rdna4_err(3, @"no vkCreateInstance"); goto fail; }
        VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "garnetmetal", .apiVersion = VK_API_VERSION_1_3 };
        VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai };
        {   // P5a: /private/tmp/rdna4m-nosam -> RADV_PERFTEST gains "nosam" (read by RADV at instance creation): command buffers and RADV's upload buffer go to GTT
            // (system memory, CPU-cached, GPU-snooped) instead of the BAR, whose uncached stores stalled radv_emit_* during desktop drags (P5 study, ws-p1.txt). Absent = unchanged.
            struct stat sb_;
            if (stat("/private/tmp/rdna4m-nosam", &sb_) == 0 || stat("/private/tmp/rdna4m-nosam-off", &sb_) != 0) {   //: DEFAULT ON (60 fps under drag measured); /private/tmp/rdna4m-nosam-off disables
                const char *pt = getenv("RADV_PERFTEST"); char v[256];
                snprintf(v, sizeof v, "%s%snosam", pt && *pt ? pt : "", pt && *pt ? "," : "");
                setenv("RADV_PERFTEST", v, 1);
                RDNA4LOG("radv open: P5a /private/tmp/rdna4m-nosam present: RADV_PERFTEST=%s (command buffers in GTT)", v);
            }
        }
        VkResult r = rdna4_vkCreateInstance(&ici, NULL, &RDNA4R.inst);
        if (r != VK_SUCCESS) { RDNA4R.err = rdna4_err(4, [NSString stringWithFormat:@"vkCreateInstance = %d", r]); goto fail; }
#define X(n) n = (PFN_##n)rdna4_gipa(RDNA4R.inst, #n); if (!n) RDNA4LOG("radv: %s not exposed", #n);
        RDNA4_VK_FUNCS(X)
#undef X
        rdna4_vkQueueWaitIdle = (PFN_vkQueueWaitIdle)rdna4_gipa(RDNA4R.inst, "vkQueueWaitIdle");
        rdna4_native_bridge.copy = (void *)dlsym(h, "radv_darwin_buffer_copy");
        rdna4_native_bridge.fill = (void *)dlsym(h, "radv_darwin_buffer_fill");
        rdna4_native_bridge.wait = (void *)dlsym(h, "radv_darwin_buffer_wait");
        uint32_t cnt = 1;
        r = vkEnumeratePhysicalDevices(RDNA4R.inst, &cnt, &RDNA4R.pd);
        if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || cnt == 0) {
            RDNA4R.err = rdna4_err(5, [NSString stringWithFormat:@"vkEnumeratePhysicalDevices = %d count %u (RDNA4N refused? root-only, exclusive)", r, cnt]);
            if (vkDestroyInstance && RDNA4R.inst) { vkDestroyInstance(RDNA4R.inst, NULL); RDNA4R.inst = VK_NULL_HANDLE; }   // a retry starts from a fresh instance
            goto fail;
        }
        VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(RDNA4R.pd, &pp);
        RDNA4LOG("radv: physical device %s", pp.deviceName);
        uint64_t maxAllocation = 0, maxBuffer = 0; uint32_t subgroupSize = 0;
        rdna4x_backend_capabilities queryCaps = rdna4x_query_caps(0, 0, 0, 0, 0);
        if (@available(macOS 11.0, *)) queryCaps.flags |= RDNA4X_CAP_QUERY_LANGUAGE;
        if (vkGetPhysicalDeviceProperties2 && pp.apiVersion >= VK_API_VERSION_1_1) {
            VkPhysicalDeviceSubgroupProperties sg = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES };
            VkPhysicalDeviceMaintenance4Properties m4 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_PROPERTIES };
            VkPhysicalDeviceVulkan12Properties indexing = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES };
            if (pp.apiVersion >= VK_API_VERSION_1_2) sg.pNext = &indexing;
            if (pp.apiVersion >= VK_API_VERSION_1_3) indexing.pNext = &m4;
            VkPhysicalDeviceMaintenance3Properties m3 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES, .pNext = &sg };
            VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &m3 };
            vkGetPhysicalDeviceProperties2(RDNA4R.pd, &p2);
            maxAllocation = m3.maxMemoryAllocationSize;
            RDNA4R.maxPerSetDescriptors = m3.maxPerSetDescriptors;
            maxBuffer = m4.maxBufferSize;
            RDNA4R.descriptorLimits = indexing; RDNA4R.descriptorLimits.pNext = NULL;
            if (sg.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) subgroupSize = sg.subgroupSize;
            queryCaps.subgroup_size = sg.subgroupSize;
            queryCaps.subgroup_supported_stages = sg.supportedStages;
            queryCaps.subgroup_supported_operations = sg.supportedOperations;
        }
        uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(RDNA4R.pd, &nq, NULL);
        VkQueueFamilyProperties qf[16]; if (nq > 16) nq = 16;
        vkGetPhysicalDeviceQueueFamilyProperties(RDNA4R.pd, &nq, qf);
        uint32_t qfi = 0; for (; qfi < nq; qfi++) if (qf[qfi].queueFlags & VK_QUEUE_GRAPHICS_BIT) break;
        if (qfi == nq) { RDNA4R.err = rdna4_err(6, @"no graphics queue family"); goto fail; }
        RDNA4R.qfi = qfi;
        float prio = 1.0f;
        VkDeviceQueueCreateInfo dq = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qfi, .queueCount = 1, .pQueuePriorities = &prio };
        VkPhysicalDeviceFeatures pf; vkGetPhysicalDeviceFeatures(RDNA4R.pd, &pf);
        const rdna4_gather_caps gatherCaps = rdna4_gather_caps_from_vulkan(
            pf.shaderImageGatherExtended != VK_FALSE,
            pp.limits.minTexelGatherOffset, pp.limits.maxTexelGatherOffset);
        RDNA4LOG("radv: shaderImageGatherExtended %s, texel gather offsets [%d, %u]",
                 gatherCaps.supported ? "supported" : "unsupported",
                 gatherCaps.min_texel_gather_offset, gatherCaps.max_texel_gather_offset);
        // Translated system AIR uses 8/16-bit arithmetic and fragment demotion.
        // Support alone is insufficient: enable the queried features on our device.
        VkPhysicalDeviceVulkan13Features supported13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
        VkPhysicalDeviceVulkan12Features supported12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &supported13 };
        VkPhysicalDeviceVulkan11Features supported11 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &supported12 };
        VkPhysicalDeviceFeatures2 supported = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &supported11 };
        vkGetPhysicalDeviceFeatures2(RDNA4R.pd, &supported);
        VkPhysicalDeviceVulkan13Features enabled13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
            .shaderDemoteToHelperInvocation = supported13.shaderDemoteToHelperInvocation };
        VkPhysicalDeviceVulkan12Features enabled12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &enabled13,
            .samplerMirrorClampToEdge = supported12.samplerMirrorClampToEdge,
            .shaderFloat16 = supported12.shaderFloat16, .shaderInt8 = supported12.shaderInt8,
            .storageBuffer8BitAccess = supported12.storageBuffer8BitAccess,
            .uniformAndStorageBuffer8BitAccess = supported12.uniformAndStorageBuffer8BitAccess,
            .descriptorIndexing = supported12.descriptorIndexing,
            .runtimeDescriptorArray = supported12.runtimeDescriptorArray,
            .descriptorBindingVariableDescriptorCount = supported12.descriptorBindingVariableDescriptorCount,
            .descriptorBindingPartiallyBound = supported12.descriptorBindingPartiallyBound,
            .descriptorBindingSampledImageUpdateAfterBind = supported12.descriptorBindingSampledImageUpdateAfterBind,
            .descriptorBindingStorageImageUpdateAfterBind = supported12.descriptorBindingStorageImageUpdateAfterBind,
            .descriptorBindingUniformTexelBufferUpdateAfterBind = supported12.descriptorBindingUniformTexelBufferUpdateAfterBind,
            .descriptorBindingStorageTexelBufferUpdateAfterBind = supported12.descriptorBindingStorageTexelBufferUpdateAfterBind,
            .shaderInputAttachmentArrayDynamicIndexing = supported12.shaderInputAttachmentArrayDynamicIndexing,
            .shaderUniformTexelBufferArrayDynamicIndexing = supported12.shaderUniformTexelBufferArrayDynamicIndexing,
            .shaderStorageTexelBufferArrayDynamicIndexing = supported12.shaderStorageTexelBufferArrayDynamicIndexing,
            .shaderUniformBufferArrayNonUniformIndexing = supported12.shaderUniformBufferArrayNonUniformIndexing,
            .shaderSampledImageArrayNonUniformIndexing = supported12.shaderSampledImageArrayNonUniformIndexing,
            .shaderStorageBufferArrayNonUniformIndexing = supported12.shaderStorageBufferArrayNonUniformIndexing,
            .shaderStorageImageArrayNonUniformIndexing = supported12.shaderStorageImageArrayNonUniformIndexing,
            .shaderInputAttachmentArrayNonUniformIndexing = supported12.shaderInputAttachmentArrayNonUniformIndexing,
            .shaderUniformTexelBufferArrayNonUniformIndexing = supported12.shaderUniformTexelBufferArrayNonUniformIndexing,
            .shaderStorageTexelBufferArrayNonUniformIndexing = supported12.shaderStorageTexelBufferArrayNonUniformIndexing,
            .bufferDeviceAddress = supported12.bufferDeviceAddress };
        VkPhysicalDeviceVulkan11Features enabled11 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &enabled12,
            .shaderDrawParameters = supported11.shaderDrawParameters,
            .storageBuffer16BitAccess = supported11.storageBuffer16BitAccess,
            .uniformAndStorageBuffer16BitAccess = supported11.uniformAndStorageBuffer16BitAccess,
            .storageInputOutput16 = supported11.storageInputOutput16 };
        // translated AIR uses Int64 (10a); 11e: storage images are translated with format Unknown + Read/WriteWithoutFormat (add-air.py)
        VkPhysicalDeviceFeatures ef = { .tessellationShader = pf.tessellationShader,
                                        .shaderInt64 = pf.shaderInt64, .shaderInt16 = pf.shaderInt16, .fragmentStoresAndAtomics = pf.fragmentStoresAndAtomics, .shaderStorageImageReadWithoutFormat = pf.shaderStorageImageReadWithoutFormat,
                                        .shaderStorageImageWriteWithoutFormat = pf.shaderStorageImageWriteWithoutFormat,
                                        .independentBlend = pf.independentBlend,
                                        .shaderSampledImageArrayDynamicIndexing = pf.shaderSampledImageArrayDynamicIndexing,
                                        .shaderStorageImageArrayDynamicIndexing = pf.shaderStorageImageArrayDynamicIndexing,
                                        .shaderUniformBufferArrayDynamicIndexing = pf.shaderUniformBufferArrayDynamicIndexing,
                                        .shaderStorageBufferArrayDynamicIndexing = pf.shaderStorageBufferArrayDynamicIndexing,
                                        .depthBiasClamp = pf.depthBiasClamp,   // bundle 10: setDepthBias:slopeScale:clamp:
                                        .drawIndirectFirstInstance = pf.drawIndirectFirstInstance,
                                        .imageCubeArray = pf.imageCubeArray,
                                        .samplerAnisotropy = pf.samplerAnisotropy,
                                        .shaderImageGatherExtended = gatherCaps.supported ? VK_TRUE : VK_FALSE,
                                        .occlusionQueryPrecise = pf.occlusionQueryPrecise };   // build 18 (P2): Counting visibility mode
        RDNA4R.maxTessGenLevel = pp.limits.maxTessellationGenerationLevel;
        RDNA4R.maxTessPatchSize = pp.limits.maxTessellationPatchSize;
        RDNA4R.tessellation = ef.tessellationShader && RDNA4R.maxTessGenLevel >= 64 && RDNA4R.maxTessPatchSize >= 32;
        RDNA4LOG("radv: tessellation %s (feature %u, maxGen %u, maxPatch %u, shaderDrawParameters %u)",
                 RDNA4R.tessellation ? "enabled" : "unavailable", ef.tessellationShader,
                 RDNA4R.maxTessGenLevel, RDNA4R.maxTessPatchSize, enabled11.shaderDrawParameters);
        RDNA4R.uniformTexelBuffers = enabled12.runtimeDescriptorArray &&
            enabled12.descriptorBindingPartiallyBound &&
            enabled12.descriptorBindingUniformTexelBufferUpdateAfterBind &&
            enabled12.shaderUniformTexelBufferArrayDynamicIndexing &&
            enabled12.shaderUniformTexelBufferArrayNonUniformIndexing &&
            vkCreateBufferView && vkDestroyBufferView;
        RDNA4R.storageTexelBuffers = enabled12.runtimeDescriptorArray &&
            enabled12.descriptorBindingPartiallyBound &&
            enabled12.descriptorBindingStorageTexelBufferUpdateAfterBind &&
            enabled12.shaderStorageTexelBufferArrayDynamicIndexing &&
            enabled12.shaderStorageTexelBufferArrayNonUniformIndexing &&
            vkCreateBufferView && vkDestroyBufferView;
        RDNA4R.descriptorIndexing = enabled12.descriptorIndexing;
        RDNA4R.nonUniformSampledImages = enabled12.shaderSampledImageArrayNonUniformIndexing;
        RDNA4R.nonUniformStorageImages = enabled12.shaderStorageImageArrayNonUniformIndexing;
        RDNA4R.nonUniformBuffers = enabled12.shaderUniformBufferArrayNonUniformIndexing &&
            enabled12.shaderStorageBufferArrayNonUniformIndexing;
        RDNA4R.gpuArgumentTables = enabled12.runtimeDescriptorArray &&
            enabled12.descriptorBindingVariableDescriptorCount && enabled12.descriptorBindingPartiallyBound &&
            enabled12.descriptorBindingSampledImageUpdateAfterBind && enabled12.descriptorBindingStorageImageUpdateAfterBind &&
            RDNA4R.nonUniformSampledImages && RDNA4R.nonUniformStorageImages && pp.limits.maxBoundDescriptorSets >= 5;
        RDNA4LOG("radv: descriptor indexing %u, non-uniform sampled/storage images %u/%u, buffers %u, dynamic arrays sampled/storage/buffer %u/%u/%u/%u",
                 RDNA4R.descriptorIndexing, RDNA4R.nonUniformSampledImages, RDNA4R.nonUniformStorageImages,
                 RDNA4R.nonUniformBuffers, ef.shaderSampledImageArrayDynamicIndexing,
                 ef.shaderStorageImageArrayDynamicIndexing, ef.shaderUniformBufferArrayDynamicIndexing,
                 ef.shaderStorageBufferArrayDynamicIndexing);
        RDNA4LOG("radv: apiVersion %u.%u shaderInt64 supported %u, storage image read/write without format %u/%u", VK_API_VERSION_MAJOR(pp.apiVersion), VK_API_VERSION_MINOR(pp.apiVersion), pf.shaderInt64,
               pf.shaderStorageImageReadWithoutFormat, pf.shaderStorageImageWriteWithoutFormat);
        RDNA4R.lim = pp.limits; RDNA4R.dbClamp = pf.depthBiasClamp ? YES : NO;
        if (rdna4_argument_limit_verify_open(
                &pp, &RDNA4R.descriptorLimits, RDNA4R.maxPerSetDescriptors,
                maxAllocation, maxBuffer,
                &seen, seenResult, sourceMatches) < 0)
            RDNA4R.gpuArgumentTables = NO;   // Tier1 remains open; only the argument-table path fails closed.
        RDNA4R.samplerAnisotropy = ef.samplerAnisotropy;
        RDNA4R.samplerMirrorClamp = enabled12.samplerMirrorClampToEdge;
        RDNA4LOG("radv: buffer offset alignments uniform %llu storage %llu", (unsigned long long)pp.limits.minUniformBufferOffsetAlignment,
                 (unsigned long long)pp.limits.minStorageBufferOffsetAlignment);
        RDNA4R.indirectFirstInstance = ef.drawIndirectFirstInstance;
        RDNA4R.cubeArray = ef.imageCubeArray;
        RDNA4R.occPrecise = pf.occlusionQueryPrecise ? YES : NO;
        RDNA4R.occOK = (vkCreateQueryPool && vkDestroyQueryPool && vkCmdResetQueryPool && vkCmdBeginQuery && vkCmdEndQuery && vkGetQueryPoolResults && vkGetFenceStatus) ? YES : NO;   // build 18 (P2)
        {   // bundle 10: what the device offers for depth/stencil and MSAA (each depth texture / pipeline checks these again before it uses a format)
            static const VkFormat dsf[4] = { VK_FORMAT_D16_UNORM, VK_FORMAT_D32_SFLOAT, VK_FORMAT_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT };
            for (int i = 0; i < 4 && vkGetPhysicalDeviceFormatProperties; i++) { VkFormatProperties fp0 = {0}; vkGetPhysicalDeviceFormatProperties(RDNA4R.pd, dsf[i], &fp0);
                RDNA4LOG("radv: depth/stencil format %d optimal features 0x%x (sampled %d, depth/stencil attachment %d)%s", (int)dsf[i], fp0.optimalTilingFeatures, (fp0.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0,
                       (fp0.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0, (fp0.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) ? "" : ": depth textures / pipelines of this format are refused"); }
            RDNA4LOG("radv: framebuffer sample counts color 0x%x depth 0x%x stencil 0x%x; depthBiasClamp %d", pp.limits.framebufferColorSampleCounts, pp.limits.framebufferDepthSampleCounts, pp.limits.framebufferStencilSampleCounts, pf.depthBiasClamp);
        }
        VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT orderedColor = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_FEATURES_EXT };
        VkPhysicalDeviceRobustness2FeaturesEXT supportedNull = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT };
        const char *orderedColorExt = NULL;
        BOOL hasDrmMod = NO, hasMemoryBudget = NO;
        {
            uint32_t ne = 0; vkEnumerateDeviceExtensionProperties(RDNA4R.pd, NULL, &ne, NULL);
            VkExtensionProperties *ex = calloc(ne ? ne : 1, sizeof *ex); vkEnumerateDeviceExtensionProperties(RDNA4R.pd, NULL, &ne, ex);
            NSMutableString *all = [NSMutableString string];
            for (uint32_t i = 0; i < ne; i++) [all appendFormat:@"%s ", ex[i].extensionName];
            RDNA4LOG("radv: %u device extensions (listed in chunks):", ne);
            for (NSUInteger o = 0; o < all.length; o += 600) RDNA4LOG("radv: ext[%lu]: %s", (unsigned long)o, [all substringWithRange:NSMakeRange(o, MIN((NSUInteger)600, all.length - o))].UTF8String);
            BOOL hasRO = NO, hasLR = NO, hasFL = NO, hasDR = NO, hasNull = NO;
            for (uint32_t i = 0; i < ne; i++) {
                if (!strcmp(ex[i].extensionName, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME)) RDNA4R.hostExt = YES;
                hasDrmMod |= !strcmp(ex[i].extensionName, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
                hasMemoryBudget |= !strcmp(ex[i].extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
                hasNull |= !strcmp(ex[i].extensionName, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
                hasRO |= !strcmp(ex[i].extensionName, "VK_EXT_rasterization_order_attachment_access") || !strcmp(ex[i].extensionName, "VK_ARM_rasterization_order_attachment_access");
                if (!strcmp(ex[i].extensionName, VK_EXT_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME)) orderedColorExt = VK_EXT_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME;
                else if (!orderedColorExt && !strcmp(ex[i].extensionName, VK_ARM_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME)) orderedColorExt = VK_ARM_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME;
                hasLR |= !strcmp(ex[i].extensionName, "VK_KHR_dynamic_rendering_local_read");
                hasFL |= !strcmp(ex[i].extensionName, "VK_EXT_attachment_feedback_loop_layout");
                hasDR |= !strcmp(ex[i].extensionName, "VK_KHR_dynamic_rendering");
            }
            free(ex);
            if (hasNull) orderedColor.pNext = &supportedNull;
            VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR flr = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR, .pNext = &orderedColor };
            VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &flr };
            if (vkGetPhysicalDeviceFeatures2) vkGetPhysicalDeviceFeatures2(RDNA4R.pd, &f2);
            RDNA4LOG("radv: framebuffer-fetch support: rasterization_order_attachment_access ext %d (color feature %d, depth %d, stencil %d); dynamic_rendering_local_read ext %d (feature %d); attachment_feedback_loop_layout ext %d; dynamic_rendering ext %d; classic render-pass input attachments: core 1.0",
                   hasRO, orderedColor.rasterizationOrderColorAttachmentAccess, orderedColor.rasterizationOrderDepthAttachmentAccess, orderedColor.rasterizationOrderStencilAttachmentAccess, hasLR, flr.dynamicRenderingLocalRead, hasFL, hasDR);
            orderedColor.pNext = NULL;
        }
        const char *devExts[4]; uint32_t devExtCount = 0;
        if (RDNA4R.hostExt) devExts[devExtCount++] = VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME;
        RDNA4R.orderedColorAccess = orderedColorExt && orderedColor.rasterizationOrderColorAttachmentAccess;
        if (RDNA4R.orderedColorAccess) {
            orderedColor.rasterizationOrderDepthAttachmentAccess = VK_FALSE;
            orderedColor.rasterizationOrderStencilAttachmentAccess = VK_FALSE;
            enabled13.pNext = &orderedColor;
            devExts[devExtCount++] = orderedColorExt;
        }
        VkPhysicalDeviceRobustness2FeaturesEXT enabledNull = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT,
            .robustBufferAccess2 = pf.robustBufferAccess && supportedNull.robustBufferAccess2,
            .nullDescriptor = supportedNull.nullDescriptor };
        RDNA4R.nullDescriptors = enabledNull.nullDescriptor;
        RDNA4R.robustTexelBuffers = enabledNull.robustBufferAccess2;
        ef.robustBufferAccess = RDNA4R.robustTexelBuffers;
        if (RDNA4R.nullDescriptors || RDNA4R.robustTexelBuffers) {
            enabledNull.pNext = enabled13.pNext;
            enabled13.pNext = &enabledNull;
            devExts[devExtCount++] = VK_EXT_ROBUSTNESS_2_EXTENSION_NAME;
        }
        RDNA4LOG("radv: null descriptors %s", RDNA4R.nullDescriptors ? "enabled" : "unavailable (typed dummy images)");
        RDNA4LOG("radv: robust texture-buffer access %s", RDNA4R.robustTexelBuffers ? "enabled" : "unavailable");
        RDNA4LOG("radv: GPU argument tables %s", RDNA4R.gpuArgumentTables ? "enabled" : "unavailable");
        RDNA4R.drmModExt = hasDrmMod && vkGetPhysicalDeviceImageFormatProperties2;
        if (RDNA4R.drmModExt) devExts[devExtCount++] = VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME;
        RDNA4LOG("radv: VK_EXT_image_drm_format_modifier %s (VK_KHR_image_format_list is core at API 1.2; device API is 1.3)", RDNA4R.drmModExt ? "enabled" : "unavailable");
        VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &enabled11, .queueCreateInfoCount = 1, .pQueueCreateInfos = &dq, .pEnabledFeatures = &ef,
                                   .enabledExtensionCount = devExtCount, .ppEnabledExtensionNames = devExts };
        r = vkCreateDevice(RDNA4R.pd, &dci, NULL, &RDNA4R.dev);
        if (r != VK_SUCCESS) { RDNA4R.err = rdna4_err(7, [NSString stringWithFormat:@"vkCreateDevice = %d", r]); goto fail; }
        if (enabled12.bufferDeviceAddress && vkGetDeviceProcAddr) {
            rdna4_vkGetBufferAddress = (PFN_vkGetBufferDeviceAddress)vkGetDeviceProcAddr(RDNA4R.dev, "vkGetBufferDeviceAddress");
            RDNA4R.bufferAddress = rdna4_vkGetBufferAddress != NULL;
        }
        vkGetDeviceQueue(RDNA4R.dev, qfi, 0, &RDNA4R.q);
        RDNA4R.hostAlign = 4096;
        if (RDNA4R.hostExt && vkGetDeviceProcAddr && vkGetPhysicalDeviceProperties2) {
            rdna4_vkGetMHPP = (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(RDNA4R.dev, "vkGetMemoryHostPointerPropertiesEXT");
            VkPhysicalDeviceExternalMemoryHostPropertiesEXT emh = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT };
            VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &emh };
            vkGetPhysicalDeviceProperties2(RDNA4R.pd, &p2);
            if (emh.minImportedHostPointerAlignment) RDNA4R.hostAlign = emh.minImportedHostPointerAlignment;
            RDNA4LOG("radv: VK_EXT_external_memory_host enabled, vkGetMemoryHostPointerPropertiesEXT %s, minImportedHostPointerAlignment %llu", rdna4_vkGetMHPP ? "present" : "MISSING", (unsigned long long)RDNA4R.hostAlign);
        } else RDNA4LOG("radv: VK_EXT_external_memory_host NOT available (IOSurface-backed textures will be refused)");
        vkGetPhysicalDeviceMemoryProperties(RDNA4R.pd, &RDNA4R.mp);
        pthread_mutex_init(&RDNA4R.qlock, NULL);
        VkCommandPoolCreateInfo opc = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qfi, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT };
        r = vkCreateCommandPool(RDNA4R.dev, &opc, NULL, &RDNA4R.opool);   // one-shot uploads (replaceRegion, getBytes)
        if (r != VK_SUCCESS) { RDNA4R.err = rdna4_err(8, [NSString stringWithFormat:@"vkCreateCommandPool(one-shot) = %d", r]); goto fail; }
        rdna4_pc_open(&pp);
        rdna4_pool_latch();   // P1: reads /private/tmp/rdna4m-pool ONCE
        rdna4_imp_latch();    // P4: reads the kill file /private/tmp/rdna4m-noimpcache ONCE (build 16: the cache is ON by default)
        rdna4_drawopt_latch();   // P5b: reads /private/tmp/rdna4m-drawopt ONCE
        RDNA4R.ok = YES;
        rdna4dc_heap heaps[VK_MAX_MEMORY_HEAPS] = {0};
        uint32_t heapCount = MIN(RDNA4R.mp.memoryHeapCount, VK_MAX_MEMORY_HEAPS);
        for (uint32_t i = 0; i < heapCount; i++) {
            heaps[i].size = RDNA4R.mp.memoryHeaps[i].size;
            heaps[i].device_local = (RDNA4R.mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
        }
        rdna4dc_properties caps = { .name = pp.deviceName, .storage_buffer_range = pp.limits.maxStorageBufferRange,
            .allocation_size = maxAllocation, .heaps = heaps, .heap_count = heapCount,
            .workgroup_size = {pp.limits.maxComputeWorkGroupSize[0], pp.limits.maxComputeWorkGroupSize[1], pp.limits.maxComputeWorkGroupSize[2]},
            .invocations = pp.limits.maxComputeWorkGroupInvocations, .shared_memory = pp.limits.maxComputeSharedMemorySize, .subgroup_size = subgroupSize,
            .logical_buffer_size = maxBuffer, .addressing64 = RDNA4R.bufferAddress && ef.shaderInt64 };
        PFN_vkGetPhysicalDeviceMemoryProperties2 memoryProperties2 = (PFN_vkGetPhysicalDeviceMemoryProperties2)
            rdna4_gipa(RDNA4R.inst, "vkGetPhysicalDeviceMemoryProperties2");
        if (hasMemoryBudget && memoryProperties2) {
            VkPhysicalDeviceMemoryBudgetPropertiesEXT budget = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT };
            VkPhysicalDeviceMemoryProperties2 memory = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, .pNext = &budget };
            memoryProperties2(RDNA4R.pd, &memory);
            caps.budget_valid = 1;
            for (uint32_t i = 0; i < heapCount; ++i) {
                if (!heaps[i].device_local) continue;
                uint64_t part = MIN(heaps[i].size, budget.heapBudget[i]);
                caps.allocation_budget = part > UINT64_MAX - caps.allocation_budget ? UINT64_MAX : caps.allocation_budget + part;
            }
        }
        if (pp.apiVersion >= VK_API_VERSION_1_1 && RDNA4R.gpuArgumentTables)
            queryCaps.flags |= RDNA4X_CAP_QUERY_DEVICE;
        RDNA4QueryCaps = queryCaps;
        RDNA4Caps = rdna4dc_from_properties(&caps);
        atomic_store_explicit(&rdna4_caps_ready, 1, memory_order_release);
        RDNA4GatherCaps = gatherCaps;
        atomic_store_explicit(&rdna4_gather_caps_ready, 1, memory_order_release);
        rdna4_t1_start();
        rdna4s_prebind_on_radv_open();
        RDNA4LOG("radv open: OK (queue family %u)", qfi);
        return YES;
    fail:
        RDNA4LOG("radv open: FAILED %s", RDNA4R.err.localizedDescription.UTF8String);
        if (err) *err = RDNA4R.err;
        return NO;
    }
}

// #12 R4: a refused RDNA4N open (code 5: busy / not ready) is transient. The first open of the process retries with backoff
// (0.1, 0.2, 0.4, 0.8, 1.5 s, bounded to 3 s from the first attempt) before giving up, logging every attempt; after that, a later request retries ONCE if the
// last failure is >= 2 s old (so a lazy open that lost a race with a closing client is not lost for the life of the process).
// Any other failure (no dylib, vkCreateDevice, ...) is permanent for the process, as before.
static BOOL rdna4_radv_open(NSError **err) {
    @synchronized ([GarnetDevice class]) {
        if (RDNA4R.ok) return YES;
        static uint64_t lastFail;
        BOOL transient = RDNA4R.tried && RDNA4R.err.code == 5;
        if (RDNA4R.tried && !(transient && rdna4_now() - lastFail >= 2000000000ULL)) { if (err) *err = RDNA4R.err; return NO; }
        static const useconds_t backoff[] = { 100000, 200000, 400000, 800000, 1500000 };
        const uint64_t budget = 3000000000ULL;   // first open of the process: keep trying until 3 s have passed since the first attempt began (an attempt itself can take ~2 s on the PC)
        BOOL first = !RDNA4R.tried; RDNA4R.tried = YES;
        uint64_t t0 = rdna4_now();
        for (int a = 0; a < (first ? 6 : 1); a++) {
            if (a) {
                uint64_t el = rdna4_now() - t0;
                if (el >= budget) { RDNA4LOG("radv open: giving up after %.2f s and %d attempt(s)", (double)el / 1e9, a); break; }
                useconds_t d = backoff[a - 1]; if ((uint64_t)d * 1000ULL > budget - el) d = (useconds_t)((budget - el) / 1000ULL);
                RDNA4LOG("radv open: refused (%s); retry %d after %u ms (%.2f s used of 3 s)", RDNA4R.err.localizedDescription.UTF8String, a, d / 1000, (double)el / 1e9); usleep(d);
            }
            RDNA4LOG("radv open: attempt %d%s", a + 1, first ? "" : " (retry after an earlier refusal)");
            if (rdna4_test_refuse()) { RDNA4R.err = rdna4_err(5, @"vkEnumeratePhysicalDevices = -3 count 0 (RDNA4M_TEST_OPEN_REFUSE: simulated refusal)"); continue; }
            if (rdna4_radv_open_once(err)) { if (a || !first) RDNA4LOG("radv open: succeeded on attempt %d", a + 1); return YES; }
            if (RDNA4R.err.code != 5) break;
        }
        lastFail = rdna4_now();
        if (err) *err = RDNA4R.err;
        return NO;
    }
}

static int rdna4_find_mem(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < RDNA4R.mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (RDNA4R.mp.memoryTypes[i].propertyFlags & want) == want) return (int)i;
    return -1;
}

