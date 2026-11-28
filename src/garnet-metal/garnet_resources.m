// garnet_resources.m - GarnetMetal modular compilation unit.
#if !defined(GARNET_INTERNAL) && !defined(GARNET_METAL_MODULAR_BUILD)
#define GARNET_INTERNAL static
#endif
#ifdef GARNET_METAL_MODULAR_BUILD
#include "garnet_metal_internal.h"
#endif

// ---------------------------------------------------------------------------------------------------------------
// P1 "memory pooling" (garnet_pool.h decides, this block does the Vulkan calls). ON only when /private/tmp/garnet-pool exists, latched ONCE when the device is
// created; OFF = every call site below runs the code that was there before (the `else` branches are the old lines, unchanged).
//  * RDNA4Buffer / RDNA4Texture memory: <= 256 KiB from 4 MiB slabs (bound at the slab offset; host-visible slabs mapped once), 256 KiB..64 MiB from a recycle
//    cache keyed by (memory type, size), the rest (and anything VkMemoryDedicatedRequirements marks dedicated) on its own allocation as before.
//  * Nothing freed is reused until every command buffer that existed at the free has completed (device-wide fence, see RDNA4CommandBuffer: serial taken at creation
//    in -initWithQueue:, closed in the completion block of -submitCommandBuffers:count: or at dealloc if it was never submitted).
//  * Descriptor pools of a command buffer go to a fence-gated free list (cap 32) instead of being destroyed.
// IOSurface / host-pointer imports, the scanout slots (RDNA4S.mem) and heaps' budgets are untouched.
// ---------------------------------------------------------------------------------------------------------------
#define RDNA4_POOL_FILE "/private/tmp/garnet-pool"
// The heap needs retirement serials even when the memory pool is OFF. Keep a second
// fence-only timeline under the same mutex, while the pool timeline remains gated by
// its own switch.
static struct { rdna4p_pool p; BOOL ready; } RDNA4HT;
GARNET_INTERNAL RDNA4PoolState RDNA4P = { .mu = PTHREAD_MUTEX_INITIALIZER };
GARNET_INTERNAL void rdna4_pool_latch(void) {
    static int done; if (done) return; done = 1;
    RDNA4P.on = rdna4_sb_access_exists(RDNA4_POOL_FILE) || !rdna4_sb_access_exists(RDNA4_POOL_FILE "-off");   //: DEFAULT ON; /private/tmp/garnet-pool-off disables
    RDNA4P.auditSubmit = getenv("RDNA4_AUDIT_SUBMIT") != NULL;
    rdna4p_init(&RDNA4P.p, RDNA4P.on);
    garnet_vkpool_init(&RDNA4P.fences, GARNET_VKPOOL_FENCE_CAP);
    garnet_vkpool_init(&RDNA4P.oneshotCmds, GARNET_VKPOOL_ONESHOT_CAP);
    RDNA4P.bmr2 = (PFN_vkGetBufferMemoryRequirements2)vkGetDeviceProcAddr(RDNA4R.dev, "vkGetBufferMemoryRequirements2");
    RDNA4P.imr2 = (PFN_vkGetImageMemoryRequirements2)vkGetDeviceProcAddr(RDNA4R.dev, "vkGetImageMemoryRequirements2");
    RDNA4P.rdp = (PFN_vkResetDescriptorPool)vkGetDeviceProcAddr(RDNA4R.dev, "vkResetDescriptorPool");
    RDNA4P.reqOK = RDNA4P.bmr2 && RDNA4P.imr2;
    if (!RDNA4P.on) { RDNA4LOG("T1 pool: OFF (no %s at device creation; latched for the life of this process); heap requirements2 %s", RDNA4_POOL_FILE, RDNA4P.reqOK ? "available" : "UNAVAILABLE"); return; }
    if (!RDNA4P.rdp) RDNA4LOG("T1 pool: vkResetDescriptorPool not exposed: descriptor pools will not be reused");
    RDNA4LOG("T1 pool: ON (%s present at device creation): slabs %llu KiB (<= %llu KiB), recycle %llu KiB..%llu MiB cap %llu MiB, idle %llu ms, dedicated query %s, granularity %llu",
           RDNA4_POOL_FILE, (unsigned long long)(RDNA4P_SLAB_SIZE >> 10), (unsigned long long)(RDNA4P_SUB_MAX >> 10), (unsigned long long)(RDNA4P_SUB_MAX >> 10), (unsigned long long)(RDNA4P_REC_MAX >> 20),
           (unsigned long long)(RDNA4P_REC_CAP >> 20), (unsigned long long)(RDNA4P_IDLE_NS / 1000000ULL), RDNA4P.reqOK ? "yes" : "NO (everything own-allocation)", (unsigned long long)RDNA4R.lim.bufferImageGranularity);
}
// Frees what the pool has given up (called with the pool mutex NOT held: vkFreeMemory is the slow kernel call).
static unsigned rdna4_pool_drain(void) {
    unsigned freed = 0;
    if (!RDNA4R.ok) return 0;
    for (;;) {
        rdna4p_rel o; pthread_mutex_lock(&RDNA4P.mu); int got = rdna4p_pop_release(&RDNA4P.p, &o); pthread_mutex_unlock(&RDNA4P.mu);
        if (!got) return freed;
        vkFreeMemory(RDNA4R.dev, (VkDeviceMemory)o.mem, NULL);
        freed++;
    }
}
static _Atomic int rdna4_pool_drain_scheduled;
static dispatch_queue_t rdna4_pool_drain_queue(void) {
    static dispatch_queue_t q;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        dispatch_queue_attr_t attr = dispatch_queue_attr_make_with_qos_class(
            DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0);
        q = dispatch_queue_create("rdna4.pool-drain", attr);
    });
    return q;
}
static void rdna4_pool_drain_async(void) {
    if (!RDNA4P.on || RDNA4P.auditSubmit || !RDNA4R.ok ||
        RDNA4R.realFreeMem == NULL || vkFreeMemory != RDNA4R.realFreeMem) {
        rdna4_pool_drain();
        return;
    }
    pthread_mutex_lock(&RDNA4P.mu);
    const size_t pending = RDNA4P.p.nrl;
    pthread_mutex_unlock(&RDNA4P.mu);
    if (!pending) return;
    if (atomic_exchange(&rdna4_pool_drain_scheduled, 1) != 0) return;
    dispatch_async(rdna4_pool_drain_queue(), ^{
        for (;;) {
            atomic_store(&rdna4_pool_drain_scheduled, 0);
            (void)rdna4_pool_drain();
            pthread_mutex_lock(&RDNA4P.mu);
            const size_t more = RDNA4P.p.nrl;
            pthread_mutex_unlock(&RDNA4P.mu);
            if (!more || atomic_exchange(&rdna4_pool_drain_scheduled, 1) != 0) break;
        }
    });
}
GARNET_INTERNAL void rdna4_pool_tick(void) {
    if (!RDNA4P.on) return;
    pthread_mutex_lock(&RDNA4P.mu); rdna4p_trim(&RDNA4P.p, rdna4_now()); pthread_mutex_unlock(&RDNA4P.mu);
    rdna4_pool_drain();
}
GARNET_INTERNAL void rdna4_pool_report(unsigned tick) {
    if (!RDNA4P.on) return;
    char line[640]; uint64_t sig;
    pthread_mutex_lock(&RDNA4P.mu); rdna4p_fmt(&RDNA4P.p, line, sizeof line); sig = RDNA4P.p.subAlloc + RDNA4P.p.subFree + RDNA4P.p.recHit + RDNA4P.p.recMiss + RDNA4P.p.dpReuse + RDNA4P.p.dpCreate + RDNA4P.p.slabNew + RDNA4P.p.slabRel + RDNA4P.p.directAlloc; pthread_mutex_unlock(&RDNA4P.mu);
    if (sig != RDNA4P.lastSig || tick % 6 == 5) { RDNA4P.lastSig = sig; RDNA4LOG("%s [%s pid %d, cumulative]", line, getprogname(), (int)getpid()); }
}
static BOOL rdna4_pool_dedicated(VkBuffer b, VkImage i) {
    if (!RDNA4P.reqOK) return YES;   // cannot ask: keep the old behaviour (own allocation)
    VkMemoryDedicatedRequirements dr = { .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS };
    VkMemoryRequirements2 r2 = { .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = &dr };
    if (b) { VkBufferMemoryRequirementsInfo2 bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2, .buffer = b }; RDNA4P.bmr2(RDNA4R.dev, &bi, &r2); }
    else { VkImageMemoryRequirementsInfo2 ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, .image = i }; RDNA4P.imr2(RDNA4R.dev, &ii, &r2); }
    return dr.prefersDedicatedAllocation || dr.requiresDedicatedAllocation;
}
static VkResult rdna4_pool_vkalloc(uint32_t mt, uint64_t size, BOOL map, VkDeviceMemory *mem, uint8_t **mp) {
    // All pool slabs use the same flags so buffers can share them with images.
    VkMemoryAllocateFlagsInfo flags = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT };
    VkMemoryAllocateInfo ma = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = RDNA4R.bufferAddress ? &flags : NULL,
        .allocationSize = size, .memoryTypeIndex = mt };
    VkResult r = vkAllocateMemory(RDNA4R.dev, &ma, NULL, mem);
    if (r != VK_SUCCESS) {   // out of kernel BOs / VRAM: give back what the pool is holding that the fence has cleared, once
        pthread_mutex_lock(&RDNA4P.mu); rdna4p_flush(&RDNA4P.p, rdna4_now()); pthread_mutex_unlock(&RDNA4P.mu);
        if (rdna4_pool_drain() > 0)
            r = vkAllocateMemory(RDNA4R.dev, &ma, NULL, mem);
    }
    if (r != VK_SUCCESS) { *mem = VK_NULL_HANDLE; return r; }
    *mp = NULL;
    if (map) {
        void *m = NULL; r = vkMapMemory(RDNA4R.dev, *mem, 0, VK_WHOLE_SIZE, 0, &m);
        if (r != VK_SUCCESS) { vkFreeMemory(RDNA4R.dev, *mem, NULL); *mem = VK_NULL_HANDLE; return r; }
        *mp = (uint8_t *)m;
    }
    return VK_SUCCESS;
}
// Memory for one resource whose requirements are `mr` (memory type `mt`). Fills *m; the caller binds at m->off and, for a mapped buffer, uses m->map.
static VkResult rdna4_mem_alloc(uint32_t mt, VkMemoryRequirements mr, BOOL map, BOOL zero, VkBuffer b, VkImage i, RDNA4Mem *m) {
    memset(m, 0, sizeof *m);
    uint64_t gran = RDNA4R.lim.bufferImageGranularity ? RDNA4R.lim.bufferImageGranularity : 1;
    uint64_t align = mr.alignment > gran ? mr.alignment : gran, sz = rdna4p_alup(mr.size, gran), sub = sz;
    uint32_t key = mt * 2u + (map ? 1u : 0u);
    const int devLocal = (mt < RDNA4R.mp.memoryTypeCount &&
                          (RDNA4R.mp.memoryTypes[mt].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) ? 1 : 0;
    BOOL ded = rdna4_pool_dedicated(b, i), newSlab = NO;
    if (RDNA4P.on && !ded && devLocal && !map && sz > RDNA4P_SUB_MAX && sz <= RDNA4P_REC_MAX)
        sz = rdna4p_alup(rdna4p_bucket_size(sz), gran);
    for (int tries = 0; tries < 4 && !ded; tries++) {
        const uint64_t now = rdna4_now();
        rdna4p_res r; pthread_mutex_lock(&RDNA4P.mu); int k = rdna4p_alloc(&RDNA4P.p, key, sz, align, now, &r);
        if (k == RDNA4P_R_SUB && newSlab) rdna4p_note_new_slab_use(&RDNA4P.p);
        uint64_t slabTarget = 0;
        if (k == RDNA4P_R_NEED_SLAB) {
            const uint64_t need = sub > align ? sub : align;
            slabTarget = rdna4p_slab_target(&RDNA4P.p, key, devLocal, need, now);
        }
        pthread_mutex_unlock(&RDNA4P.mu);
        if (k == RDNA4P_R_SUB) {
            *m = (RDNA4Mem){ 1, (VkDeviceMemory)r.mem, r.map ? (uint8_t *)r.map + r.off : NULL, r.slab, r.off, sub, key };
            if (zero && m->map) memset(m->map, 0, mr.size);
            return VK_SUCCESS;
        }
        if (k == RDNA4P_R_REC) {
            *m = (RDNA4Mem){ 2, (VkDeviceMemory)r.mem, (uint8_t *)r.map, 0, 0, r.size, key };
            if (zero && m->map) memset(m->map, 0, mr.size);
            return VK_SUCCESS;
        }
        if (k == RDNA4P_R_NEED_SLAB) {
            if (!slabTarget) break;   // backed off after contiguous-RAM exhaustion or oversized alignment: use direct alloc
            const uint64_t need = sub > align ? sub : align;
            VkDeviceMemory sm = VK_NULL_HANDLE; uint8_t *smap = NULL; VkResult vr = VK_ERROR_OUT_OF_HOST_MEMORY;
            uint64_t trySize = slabTarget;
            while (trySize) {
                vr = rdna4_pool_vkalloc(mt, trySize, map, &sm, &smap);
                if (vr == VK_SUCCESS) break;
                trySize = rdna4p_slab_stepdown(trySize, need);
            }
            if (vr != VK_SUCCESS) {
                pthread_mutex_lock(&RDNA4P.mu);
                rdna4p_slab_note_fail(&RDNA4P.p, key, rdna4_now());
                pthread_mutex_unlock(&RDNA4P.mu);
                RDNA4LOGR("T1 pool: key %u slab alloc failed (tried down from %llu KiB); backing off %llu ms and using direct alloc",
                          key, (unsigned long long)(slabTarget >> 10), (unsigned long long)(RDNA4P_SLAB_BACKOFF_NS / 1000000ULL));
                break;   // no room for a slab: try an own allocation of the exact size
            }
            pthread_mutex_lock(&RDNA4P.mu);
            rdna4p_slab_note_ok(&RDNA4P.p, key, trySize);
            rdna4p_add_slab(&RDNA4P.p, key, trySize, (void *)sm, smap, rdna4_now());
            pthread_mutex_unlock(&RDNA4P.mu);
            if (trySize < slabTarget)
                RDNA4LOG("T1 pool: key %u slab size stepped down %llu KiB -> %llu KiB",
                         key, (unsigned long long)(slabTarget >> 10), (unsigned long long)(trySize >> 10));
            newSlab = YES; continue;
        }
        if (k == RDNA4P_R_NEED_WHOLE) {
            VkDeviceMemory wm; uint8_t *wmap; VkResult vr = rdna4_pool_vkalloc(mt, sz, map, &wm, &wmap);
            if (vr != VK_SUCCESS) return vr;
            *m = (RDNA4Mem){ 2, wm, wmap, 0, 0, sz, key };   // fresh from the kernel: already zero; size matches rdna4p_alloc's granularity-rounded sz
            return VK_SUCCESS;
        }
        break;   // DIRECT
    }
    VkDeviceMemory dm; uint8_t *dmap; VkResult vr = rdna4_pool_vkalloc(mt, mr.size, map, &dm, &dmap);
    if (vr != VK_SUCCESS) return vr;
    *m = (RDNA4Mem){ 0, dm, dmap, 0, 0, mr.size, key };
    return VK_SUCCESS;
}
static void rdna4_mem_free(RDNA4Mem *m) {
    if (!m->mem) return;
    uint64_t now = rdna4_now(); int own = 0;
    pthread_mutex_lock(&RDNA4P.mu);
    if (m->kind == 1) rdna4p_free_sub(&RDNA4P.p, m->slab, m->off, m->size, now);
    else if (m->kind == 2) own = !rdna4p_free_whole(&RDNA4P.p, m->key, m->size, (void *)m->mem, m->map, now);
    else own = 1;
    if (now - RDNA4P.lastTrim > 250000000ULL) { RDNA4P.lastTrim = now; rdna4p_trim(&RDNA4P.p, now); }
    pthread_mutex_unlock(&RDNA4P.mu);
    if (own) vkFreeMemory(RDNA4R.dev, m->mem, NULL);   // an own allocation, or one the pool declined (over the deferred-bytes bound)
    rdna4_pool_drain_async();
    memset(m, 0, sizeof *m);
}
GARNET_INTERNAL uint64_t rdna4_fence_open(void) {
    pthread_mutex_lock(&RDNA4P.mu);
    if (!RDNA4HT.ready) { rdna4p_init(&RDNA4HT.p, 1); RDNA4HT.ready = YES; }
    uint64_t s = rdna4p_submit(&RDNA4HT.p);
    if (!s) {
        pthread_mutex_unlock(&RDNA4P.mu);
        RDNA4LOG_ERR("heap retirement timeline unavailable (pending allocation failed); command buffer refused");
        return 0;
    }
    if (RDNA4P.on) {
        uint64_t pooled = rdna4p_submit(&RDNA4P.p);
        if (!pooled) {
            BOOL cancelled = rdna4p_cancel_last(&RDNA4HT.p, s);
            pthread_mutex_unlock(&RDNA4P.mu);
            RDNA4LOG_ERR("pool timeline unavailable (pending allocation failed); heap serial %llu %s",
                         (unsigned long long)s, cancelled ? "rolled back" : "ROLLBACK FAILED");
            return 0;
        }
        if (pooled != s) {
            BOOL poolCancelled = rdna4p_cancel_last(&RDNA4P.p, pooled);
            BOOL heapCancelled = rdna4p_cancel_last(&RDNA4HT.p, s);
            pthread_mutex_unlock(&RDNA4P.mu);
            RDNA4LOG_ERR("timeline serial mismatch: heap %llu pool %llu; rollback %s/%s",
                         (unsigned long long)s, (unsigned long long)pooled,
                         heapCancelled ? "ok" : "FAILED", poolCancelled ? "ok" : "FAILED");
            return 0;
        }
    }
    pthread_mutex_unlock(&RDNA4P.mu);
    return s;
}
GARNET_INTERNAL void rdna4_fence_close(uint64_t s) {
    if (!s) return;
    pthread_mutex_lock(&RDNA4P.mu);
    if (RDNA4P.on) rdna4p_done(&RDNA4P.p, s);
    if (RDNA4HT.ready) rdna4p_done(&RDNA4HT.p, s);
    pthread_mutex_unlock(&RDNA4P.mu);
}
// Descriptor pools: a segment's pools go back here at that segment's fence instead of being destroyed.
GARNET_INTERNAL BOOL rdna4_dp_give(VkDescriptorPool dp) {
    if (!RDNA4P.on || !RDNA4P.rdp) return NO;
    pthread_mutex_lock(&RDNA4P.mu); int ok = rdna4p_h_push(&RDNA4P.p, (uint64_t)(uintptr_t)dp, 32); pthread_mutex_unlock(&RDNA4P.mu); return ok ? YES : NO;
}
GARNET_INTERNAL void rdna4_dp_created(void) { if (!RDNA4P.on) return; pthread_mutex_lock(&RDNA4P.mu); RDNA4P.p.dpCreate++; pthread_mutex_unlock(&RDNA4P.mu); }
GARNET_INTERNAL VkDescriptorPool rdna4_dp_take(void) {   // VK_NULL_HANDLE: none cleared by the fence; the caller creates one
    if (!RDNA4P.on || !RDNA4P.rdp) return VK_NULL_HANDLE;
    pthread_mutex_lock(&RDNA4P.mu); VkDescriptorPool dp = (VkDescriptorPool)(uintptr_t)rdna4p_h_pop(&RDNA4P.p); pthread_mutex_unlock(&RDNA4P.mu);
    if (!dp) return VK_NULL_HANDLE;
    if (RDNA4P.rdp(RDNA4R.dev, dp, 0) != VK_SUCCESS) { vkDestroyDescriptorPool(RDNA4R.dev, dp, NULL); return VK_NULL_HANDLE; }
    return dp;
}

static BOOL rdna4_fence_pool_active(void) {
    return RDNA4P.on && !RDNA4P.auditSubmit && RDNA4R.ok && RDNA4R.realCreateFence != NULL &&
        vkCreateFence == RDNA4R.realCreateFence &&
        vkDestroyFence == RDNA4R.realDestroyFence &&
        vkResetFences == RDNA4R.realResetFences;
}

GARNET_INTERNAL BOOL rdna4_cmd_pool_active(void) {
    return RDNA4P.on && !RDNA4P.auditSubmit && RDNA4R.ok && RDNA4R.realAllocCmd != NULL &&
        vkAllocateCommandBuffers == RDNA4R.realAllocCmd &&
        vkFreeCommandBuffers == RDNA4R.realFreeCmd &&
        RDNA4R.realResetCmd != NULL &&
        vkResetCommandBuffer == RDNA4R.realResetCmd;
}

GARNET_INTERNAL VkResult rdna4_fence_take(VkFence *out) {
    if (rdna4_fence_pool_active()) {
        pthread_mutex_lock(&RDNA4P.mu);
        VkFence cached = (VkFence)(uintptr_t)garnet_vkpool_pop(&RDNA4P.fences);
        pthread_mutex_unlock(&RDNA4P.mu);
        if (cached != VK_NULL_HANDLE) {
            if (vkResetFences(RDNA4R.dev, 1, &cached) == VK_SUCCESS) {
                *out = cached;
                return VK_SUCCESS;
            }
            vkDestroyFence(RDNA4R.dev, cached, NULL);
        }
    }
    VkFenceCreateInfo fc = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    return vkCreateFence(RDNA4R.dev, &fc, NULL, out);
}

GARNET_INTERNAL void rdna4_fence_give(VkFence f) {
    if (!f || !RDNA4R.ok) return;
    if (rdna4_fence_pool_active()) {
        pthread_mutex_lock(&RDNA4P.mu);
        int kept = garnet_vkpool_push(&RDNA4P.fences, (uint64_t)(uintptr_t)f);
        pthread_mutex_unlock(&RDNA4P.mu);
        if (kept) return;
    }
    vkDestroyFence(RDNA4R.dev, f, NULL);
}

GARNET_INTERNAL VkResult rdna4_oneshot_cmd_take(VkCommandBuffer *out) {
    if (rdna4_cmd_pool_active()) {
        pthread_mutex_lock(&RDNA4P.mu);
        VkCommandBuffer cached = (VkCommandBuffer)(uintptr_t)garnet_vkpool_pop(&RDNA4P.oneshotCmds);
        pthread_mutex_unlock(&RDNA4P.mu);
        if (cached != VK_NULL_HANDLE) {
            *out = cached;
            return VK_SUCCESS;
        }
    }
    VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = RDNA4R.opool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    rdna4_qlock();
    VkResult r = vkAllocateCommandBuffers(RDNA4R.dev, &ai, out);
    pthread_mutex_unlock(&RDNA4R.qlock);
    return r;
}

GARNET_INTERNAL void rdna4_oneshot_cmd_give(VkCommandBuffer cmd) {
    if (!cmd || !RDNA4R.ok) return;
    if (rdna4_cmd_pool_active()) {
        rdna4_qlock();
        VkResult rr = vkResetCommandBuffer(cmd, 0);
        pthread_mutex_unlock(&RDNA4R.qlock);
        if (rr == VK_SUCCESS) {
            pthread_mutex_lock(&RDNA4P.mu);
            int kept = garnet_vkpool_push(&RDNA4P.oneshotCmds, (uint64_t)(uintptr_t)cmd);
            pthread_mutex_unlock(&RDNA4P.mu);
            if (kept) return;
        }
    }
    rdna4_qlock();
    vkFreeCommandBuffers(RDNA4R.dev, RDNA4R.opool, 1, &cmd);
    pthread_mutex_unlock(&RDNA4R.qlock);
}

static _Atomic uint32_t rdna4nc_ro_buffers_seen;
GARNET_INTERNAL void rdna4nc_note_readonly_buffer(void) {
    atomic_store_explicit(&rdna4nc_ro_buffers_seen, 1u, memory_order_release);
}
GARNET_INTERNAL BOOL rdna4nc_any_readonly_buffer(void) {
    return atomic_load_explicit(&rdna4nc_ro_buffers_seen, memory_order_acquire) != 0u;
}

// ---------------------------------------------------------------------------------------------------------------
// P5b "per-draw redundancy" (garnet_drawcache.h decides, this block holds the latch, the device-wide render-pass cache and the counters). ON only when
// /private/tmp/garnet-drawopt exists, latched ONCE at device creation; OFF = every call site runs the code that was there before.
//  * RDNA4RenderEncoder: re-uses a stage's VkDescriptorSet while the exact slot contents its set layout reads are unchanged; skips vkCmdBindPipeline for the pipeline
//    already bound in the current render pass; remembers the last (topology, cull, front, pso) -> VkPipeline answer.
//  * Render passes are cached device-wide by their attachment descriptions (<= 64, never destroyed while the device lives; beyond the cap created / destroyed as before).
// ---------------------------------------------------------------------------------------------------------------
#define RDNA4_DRAWOPT_FILE "/private/tmp/garnet-drawopt"
GARNET_INTERNAL RDNA4DrawOptState RDNA4DO = { .mu = PTHREAD_MUTEX_INITIALIZER };
GARNET_INTERNAL _Atomic int rdna4_dw_logged;   // P5b: how many "scanout: display write" lines rdna4s_disp_account has printed (it prints the pipeline-name list of the first 20 only)
GARNET_INTERNAL void rdna4_drawopt_latch(void) {
    static int done; if (done) return; done = 1;
    RDNA4DO.on = rdna4_sb_access_exists(RDNA4_DRAWOPT_FILE) || !rdna4_sb_access_exists(RDNA4_DRAWOPT_FILE "-off");   //: DEFAULT ON; /private/tmp/garnet-drawopt-off disables
    if (RDNA4DO.on) { RDNA4DO.rp = (rdna4dc_rpcache *)calloc(1, sizeof *RDNA4DO.rp); if (!RDNA4DO.rp) RDNA4DO.on = NO; }
    if (!RDNA4DO.on) { RDNA4LOG("T1 drawopt: OFF (no %s at device creation; latched for the life of this process)", RDNA4_DRAWOPT_FILE); return; }
    RDNA4LOG("T1 drawopt: ON (%s present at device creation; latched for the life of this process): descriptor-set reuse, pipeline-bind skip, variant cache, render-pass cache (<= %d)", RDNA4_DRAWOPT_FILE, RDNA4DC_RP_CAP);
}
GARNET_INTERNAL void rdna4_drawopt_report(unsigned tick) {
    if (!RDNA4DO.on) return;
    uint64_t d = atomic_load(&RDNA4DO.draws), sr = atomic_load(&RDNA4DO.setsReused), sa = atomic_load(&RDNA4DO.setsAlloc), ps = atomic_load(&RDNA4DO.pipesSkipped), pb = atomic_load(&RDNA4DO.pipesBound),
             pc = atomic_load(&RDNA4DO.pcHits), rh = atomic_load(&RDNA4DO.rpHits), rc = atomic_load(&RDNA4DO.rpCreated), ru = atomic_load(&RDNA4DO.rpUncached), sig = d + sr + sa + ps + pb + pc + rh + rc + ru;
    if (sig != RDNA4DO.lastSig || tick % 6 == 5) {
        RDNA4DO.lastSig = sig;
        RDNA4LOG("T1 drawopt: draws %llu, descriptor sets reused %llu / allocated %llu, pipeline binds skipped %llu / done %llu, pipelineForTopology cache hits %llu, render passes cached(hit) %llu / created %llu / uncached(cap) %llu [%s pid %d, cumulative]",
               (unsigned long long)d, (unsigned long long)sr, (unsigned long long)sa, (unsigned long long)ps, (unsigned long long)pb, (unsigned long long)pc, (unsigned long long)rh, (unsigned long long)rc, (unsigned long long)ru, getprogname(), (int)getpid());
    }
}

// ---------------------------------------------------------------------------------------------------------------
// P4 import cache (garnet_impcache.h decides, this block does the Vulkan / CF calls). ON by default since build 16 (F2); the kill file /private/tmp/garnet-noimpcache switches it OFF, latched ONCE at device creation;
// OFF = -initWithDevice:descriptor:iosurface:plane: and -dealloc run the per-texture import exactly as before (`_impShared` stays NO).
//  * One VkDeviceMemory import per live IOSurface (key id + base + import size), shared by all its textures; kept 2 s after the last texture died, <= 128 MiB unused (512 MiB in WindowServer).
//  * The cache holds its own CFRetain on the IOSurface for as long as it keeps the import (NOT IOSurfaceIncrementUseCount: that stays per texture).
//  * An import given up is vkFreeMemory'd only once the P1 fence has cleared the serial taken when it was given up (stamp 0 / completed 0 when the pool is OFF = as today).
// ---------------------------------------------------------------------------------------------------------------
#define RDNA4_IMPCACHE_KILL_FILE "/private/tmp/garnet-noimpcache"   // build 16 (F2): the cache is ON by default; this file switches it OFF (latched once per process).
static struct { BOOL on; pthread_mutex_t mu; rdna4ic_cache c; rdna4li_cache li; _Atomic uint64_t baseless; uint64_t lastSig; } RDNA4IC = { .mu = PTHREAD_MUTEX_INITIALIZER };
GARNET_INTERNAL void rdna4_imp_latch(void) {
    static int done; if (done) return; done = 1;
    RDNA4IC.on = rdna4ic_default_on(rdna4_sb_access_exists(RDNA4_IMPCACHE_KILL_FILE) ? 1 : 0) ? YES : NO;   // build 16 (F2): default ON
    rdna4ic_init(&RDNA4IC.c, RDNA4IC.on);
    rdna4ic_set_cap(&RDNA4IC.c, rdna4ic_cap_for_process(rdna4_is_ws() ? 1 : 0));
    rdna4li_init(&RDNA4IC.li, RDNA4IC.on ? 1 : 0, rdna4_is_ws() ? 128 : 64);
    if (RDNA4IC.on) RDNA4LOG("P4 impcache: ON (default since build 16; kill file %s absent at device creation; latched for the life of this process): one shared import per IOSurface, kept %llu ms after its last texture, <= %llu MiB unused, linear VkImage reuse <= %zu unused, frees fence-gated (%s)",
        RDNA4_IMPCACHE_KILL_FILE, (unsigned long long)(RDNA4IC_IDLE_NS / 1000000ULL), (unsigned long long)(RDNA4IC.c.capBytes >> 20), RDNA4IC.li.maxUnused, RDNA4P.on ? "P1 fence" : "pool OFF: kernel idle wait as today");
    else RDNA4LOG("P4 impcache: OFF (kill file %s present at device creation; latched for the life of this process)", RDNA4_IMPCACHE_KILL_FILE);
}
static void rdna4_imp_fence(uint64_t *stamp, uint64_t *completed) {
    *stamp = 0; *completed = 0;
    if (!RDNA4P.on) return;
    pthread_mutex_lock(&RDNA4P.mu); *stamp = RDNA4P.p.submitted; *completed = RDNA4P.p.completed; pthread_mutex_unlock(&RDNA4P.mu);
}
// Frees every released linear VkImage FIRST, then released import memory, that the fence allows (no mutex held across Vulkan calls).
static BOOL rdna4_imp_drain(void) {
    if (!RDNA4IC.on) return NO;
    uint64_t st, comp; rdna4_imp_fence(&st, &comp);
    for (;;) {
        void *relImg = NULL;
        pthread_mutex_lock(&RDNA4IC.mu);
        int gotImg = rdna4li_pop(&RDNA4IC.li, comp, &relImg);
        pthread_mutex_unlock(&RDNA4IC.mu);
        if (!gotImg) break;
        if (relImg) vkDestroyImage(RDNA4R.dev, (VkImage)relImg, NULL);
    }
    BOOL reclaimed = NO;
    for (;;) {
        rdna4ic_rel o; pthread_mutex_lock(&RDNA4IC.mu); int got = rdna4ic_pop(&RDNA4IC.c, comp, &o); pthread_mutex_unlock(&RDNA4IC.mu);
        if (!got) return reclaimed;
        vkFreeMemory(RDNA4R.dev, (VkDeviceMemory)o.mem, NULL);
        @synchronized ([GarnetDevice class]) { RDNA4R.impTotal -= o.size; }
        if (o.owner) CFRelease((IOSurfaceRef)o.owner);
        reclaimed = YES;
    }
}
GARNET_INTERNAL void rdna4_imp_tick(void) {
    if (!RDNA4IC.on) return;
    uint64_t st, comp; rdna4_imp_fence(&st, &comp);
    uint64_t now = rdna4_now();
    pthread_mutex_lock(&RDNA4IC.mu);
    rdna4ic_trim(&RDNA4IC.c, now, st);
    rdna4li_trim(&RDNA4IC.li, now, st);
    rdna4li_retire_orphans(&RDNA4IC.li, &RDNA4IC.c, st);
    pthread_mutex_unlock(&RDNA4IC.mu);
    rdna4_imp_drain();
}
// Every import allocation goes through here so the F1 test hook can refuse it: RDNA4M_TEST_IMPORT_FAIL_EVERY=<n> (root + RDNA4M_ALLOW=1) fails every n-th attempt like a kernel that is out of import budget.
static unsigned long rdna4f1_ctr; static unsigned rdna4f1_every = (unsigned)-1;
static VkResult rdna4_import_alloc(const VkMemoryAllocateInfo *ma, VkDeviceMemory *out) {
    if (rdna4f1_every == (unsigned)-1) { const char *e = garnet_getenv("GARNET_TEST_IMPORT_FAIL_EVERY", "RDNA4M_TEST_IMPORT_FAIL_EVERY"); rdna4f1_every = (rdna4_allow() && e) ? (unsigned)atoi(e) : 0; }
    if (rdna4f1_inject(rdna4f1_every, &rdna4f1_ctr)) { *out = VK_NULL_HANDLE; return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
    return vkAllocateMemory(RDNA4R.dev, ma, NULL, out);
}
// The shared import of `s` (ma = the import allocate info). *hit YES = an existing import was reused.
static VkResult rdna4_imp_get(IOSurfaceRef s, void *base, size_t ialloc, const VkMemoryAllocateInfo *ma, VkDeviceMemory *out, BOOL *hit) {
    uint64_t st, comp; rdna4_imp_fence(&st, &comp); *hit = NO;
    uint32_t id = IOSurfaceGetID(s); void *m = NULL;
    pthread_mutex_lock(&RDNA4IC.mu);
    int h = rdna4ic_acquire(&RDNA4IC.c, id, (uint64_t)(uintptr_t)base, ialloc, rdna4_now(), st, &m);
    rdna4li_retire_orphans(&RDNA4IC.li, &RDNA4IC.c, st);
    pthread_mutex_unlock(&RDNA4IC.mu);
    if (h) { *out = (VkDeviceMemory)m; *hit = YES; return VK_SUCCESS; }
    VkDeviceMemory mem = VK_NULL_HANDLE; VkResult r = rdna4_import_alloc(ma, &mem);
    if (r != VK_SUCCESS) {   // out of kernel imports (256 MiB per client): give back the unused cached ones the fence has cleared, once (F1: the caller then falls back to a GPU-only texture, never nil)
        rdna4_imp_fence(&st, &comp);
        pthread_mutex_lock(&RDNA4IC.mu);
        rdna4ic_flush(&RDNA4IC.c, st);
        rdna4li_retire_orphans(&RDNA4IC.li, &RDNA4IC.c, st);
        pthread_mutex_unlock(&RDNA4IC.mu);
        if (rdna4_imp_drain()) r = rdna4_import_alloc(ma, &mem);
    }
    if (r != VK_SUCCESS) { *out = VK_NULL_HANDLE; return r; }
    // Another wrapper may have imported the same surface while allocation ran without the mutex.
    rdna4_imp_fence(&st, &comp);
    pthread_mutex_lock(&RDNA4IC.mu);
    h = rdna4ic_acquire(&RDNA4IC.c, id, (uint64_t)(uintptr_t)base, ialloc, rdna4_now(), st, &m);
    if (!h) rdna4ic_add(&RDNA4IC.c, id, (uint64_t)(uintptr_t)base, ialloc, (void *)mem, (void *)CFRetain(s));
    rdna4li_retire_orphans(&RDNA4IC.li, &RDNA4IC.c, st);
    pthread_mutex_unlock(&RDNA4IC.mu);
    if (h) { vkFreeMemory(RDNA4R.dev, mem, NULL); *out = (VkDeviceMemory)m; *hit = YES; return VK_SUCCESS; }
    *out = mem; return VK_SUCCESS;
}
static void rdna4_imp_put(VkDeviceMemory mem) {
    uint64_t st, comp; rdna4_imp_fence(&st, &comp);
    pthread_mutex_lock(&RDNA4IC.mu);
    rdna4ic_unref(&RDNA4IC.c, (void *)mem, rdna4_now(), st);
    rdna4li_retire_orphans(&RDNA4IC.li, &RDNA4IC.c, st);
    pthread_mutex_unlock(&RDNA4IC.mu);
    rdna4_imp_drain();
}
static BOOL rdna4_linimg_get(const rdna4li_key *k, VkImage *outImg, int *outInited) {
    if (!RDNA4IC.on) return NO;
    uint64_t st, comp; rdna4_imp_fence(&st, &comp);
    void *img = NULL; int inited = 0;
    pthread_mutex_lock(&RDNA4IC.mu);
    int hit = rdna4li_acquire(&RDNA4IC.li, k, rdna4_now(), st, &img, &inited);
    pthread_mutex_unlock(&RDNA4IC.mu);
    if (!hit) return NO;
    *outImg = (VkImage)img;
    if (outInited) *outInited = inited;
    return YES;
}
static VkImage rdna4_linimg_add(const rdna4li_key *k, VkImage freshImg, int *outInited) {
    uint64_t st, comp; rdna4_imp_fence(&st, &comp);
    void *existing = NULL; int inited = 0;
    pthread_mutex_lock(&RDNA4IC.mu);
    int hit = rdna4li_acquire(&RDNA4IC.li, k, rdna4_now(), st, &existing, &inited);
    if (!hit) rdna4li_add(&RDNA4IC.li, k, (void *)freshImg, 0);
    pthread_mutex_unlock(&RDNA4IC.mu);
    if (hit) {
        vkDestroyImage(RDNA4R.dev, freshImg, NULL);
        if (outInited) *outInited = inited;
        return (VkImage)existing;
    }
    if (outInited) *outInited = 0;
    return freshImg;
}
static void rdna4_linimg_mark_inited(VkImage img) {
    if (!RDNA4IC.on || !img) return;
    pthread_mutex_lock(&RDNA4IC.mu);
    rdna4li_mark_inited(&RDNA4IC.li, (void *)img);
    pthread_mutex_unlock(&RDNA4IC.mu);
}
static BOOL rdna4_linimg_is_inited(VkImage img) {
    if (!RDNA4IC.on || !img) return NO;
    pthread_mutex_lock(&RDNA4IC.mu);
    int v = rdna4li_is_inited(&RDNA4IC.li, (void *)img);
    pthread_mutex_unlock(&RDNA4IC.mu);
    return v != 0;
}

static void rdna4_linimg_put(VkImage img) {
    if (!img) return;
    uint64_t st, comp; rdna4_imp_fence(&st, &comp);
    pthread_mutex_lock(&RDNA4IC.mu);
    int found = rdna4li_unref(&RDNA4IC.li, (void *)img, rdna4_now(), st);
    pthread_mutex_unlock(&RDNA4IC.mu);
    if (!found) vkDestroyImage(RDNA4R.dev, img, NULL);
    else rdna4_imp_drain();
}
// Base-less (protected) IOSurfaces: log once per id (a ring of the last 64 ids), count every texture.
static void rdna4_baseless_note(uint32_t id) {
    static uint32_t ring[64]; static unsigned pos; BOOL seen = NO;
    atomic_fetch_add(&RDNA4IC.baseless, 1);
    @synchronized ([GarnetDevice class]) {
        for (unsigned i = 0; i < 64; i++) if (ring[i] == id) { seen = YES; break; }
        if (!seen) { ring[pos++ & 63] = id; }
    }
    if (!seen) RDNA4LOG("IOSurface %u has no CPU mapping (protected): GPU-only texture; its contents are not shared with other users of the surface", id);
}
// One line per 10 s tick when something changed (and every 60 s): import cache, base-less textures, classify cache.

GARNET_INTERNAL void rdna4_imp_report(unsigned tick) {
    uint64_t ch, cm, cmm; rdna4_scan_cc_counters(&ch, &cm, &cmm);
    uint64_t bl = atomic_load(&RDNA4IC.baseless), sig;
    pthread_mutex_lock(&RDNA4IC.mu);
    rdna4ic_cache *c = &RDNA4IC.c; sig = c->created + c->reused + c->evIdle + c->evCap + c->evStale + c->evFlush + bl + ch + cm;
    if (sig != RDNA4IC.lastSig || tick % 6 == 5) {
        RDNA4IC.lastSig = sig;
        if (RDNA4IC.on) RDNA4LOG("T1 impcache ON: imports created=%llu reused=%llu live=%llu KiB (%zu entries) cached_unused=%llu KiB (%zu) evicted idle/cap/stale/flush=%llu/%llu/%llu/%llu id_mismatch=%llu; baseless_textures=%llu; classify cache hit/miss/mismatch=%llu/%llu/%llu [%s pid %d, cumulative]",
            (unsigned long long)c->created, (unsigned long long)c->reused, (unsigned long long)(c->liveBytes >> 10), c->n, (unsigned long long)(c->cachedBytes >> 10), rdna4ic_unused(c), (unsigned long long)c->evIdle, (unsigned long long)c->evCap,
            (unsigned long long)c->evStale, (unsigned long long)c->evFlush, (unsigned long long)c->mismatches, (unsigned long long)bl, (unsigned long long)ch, (unsigned long long)cm, (unsigned long long)cmm, getprogname(), (int)getpid());
        else if (bl || ch || cm) RDNA4LOG("T1 impcache OFF; baseless_textures=%llu; classify cache hit/miss/mismatch=%llu/%llu/%llu [%s pid %d, cumulative]", (unsigned long long)bl, (unsigned long long)ch, (unsigned long long)cm, (unsigned long long)cmm, getprogname(), (int)getpid());
    }
    pthread_mutex_unlock(&RDNA4IC.mu);
}

// ---------------------------------------------------------------------------------------------------------------
// Phase B: WindowServer 320 GB/s OPTIMAL VRAM surface cache (garnet_vram_surf.h decides; this block owns
// Vulkan hi-pool allocations, GFX12 DCC-preserving VkImageFormatListCreateInfo, 50 ms idle quiescence writeback,
// synchronous flush fallbacks, and capture-stack exclusion). Kill file: /private/tmp/garnet-no-vram-surf.
// Lock order: RDNA4Q._qmu -> RDNA4R.qlock -> RDNA4VS.mu (leaf O(1) lock; NEVER held across Vulkan/IOSurface/RDNA4IC calls).
// ---------------------------------------------------------------------------------------------------------------
_Static_assert((uint32_t)VK_FORMAT_R8G8B8A8_UNORM == GARNET_VS_VK_RGBA8_UNORM &&
               (uint32_t)VK_FORMAT_R8G8B8A8_SRGB == GARNET_VS_VK_RGBA8_SRGB &&
               (uint32_t)VK_FORMAT_B8G8R8A8_UNORM == GARNET_VS_VK_BGRA8_UNORM &&
               (uint32_t)VK_FORMAT_B8G8R8A8_SRGB == GARNET_VS_VK_BGRA8_SRGB &&
               (uint32_t)VK_FORMAT_A2B10G10R10_UNORM_PACK32 == GARNET_VS_VK_A2B10G10R10_UNORM &&
               (uint32_t)VK_FORMAT_A2R10G10B10_UNORM_PACK32 == GARNET_VS_VK_A2R10G10B10_UNORM &&
               (uint32_t)VK_FORMAT_R16G16B16A16_SFLOAT == GARNET_VS_VK_RGBA16_SFLOAT,
               "GARNET_VS_VK_* format values must match Vulkan VkFormat");

static struct {
    BOOL on;
    BOOL canStatSyncWb;
    pthread_mutex_t mu;
    pthread_cond_t flushCond;
    dispatch_queue_t flushQ;
    dispatch_source_t timer;
    BOOL timerArmed;
    _Atomic int syncWb;
    _Atomic uint64_t pathbUploads;
    _Atomic uint64_t pathbSkipped;
    garnet_vs_cache_t c;
    uint64_t lastSig;
} RDNA4VS = { .mu = PTHREAD_MUTEX_INITIALIZER, .flushCond = PTHREAD_COND_INITIALIZER };

// Prefer pure DEVICE_LOCAL hi-pool (non-HOST_VISIBLE) on Small-BAR so 256 MiB BAR stays free for scanout slots;
// fall back to any DEVICE_LOCAL type (covers Full-BAR where the entire 16 GiB VRAM is DEVICE_LOCAL|HOST_VISIBLE).
static int rdna4_find_vram_hipool_mem(uint32_t bits) {
    for (uint32_t i = 0; i < RDNA4R.mp.memoryTypeCount; i++) {
        const VkMemoryPropertyFlags f = RDNA4R.mp.memoryTypes[i].propertyFlags;
        if ((bits & (1u << i)) && (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) && !(f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
            return (int)i;
    }
    return rdna4_find_mem(bits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
}

static void rdna4_vs_idle_timer_fire(void);

GARNET_INTERNAL void rdna4_vs_latch(void) {
    static int done; if (done) return; done = 1;
    const int killPresent = rdna4_sb_access_exists(GARNET_VS_KILL_FILE) ? 1 : 0;
    const char *testEnv = garnet_getenv("GARNET_TEST_VRAM_SURF", NULL);
    const int testOptIn = (rdna4_allow() && testEnv && !strcmp(testEnv, "1")) ? 1 : 0;
    const int on = garnet_vs_should_enable(rdna4_is_ws() ? 1 : 0, testOptIn, killPresent);
    const uint64_t cap = RDNA4R.barPolicy.vram_surf_cap_bytes ? RDNA4R.barPolicy.vram_surf_cap_bytes : GARNET_VRAM_SURF_CAP_SMALL_BAR;
    RDNA4VS.on = on ? YES : NO;
    RDNA4VS.canStatSyncWb = rdna4_sb_can_stat(GARNET_VS_SYNC_WB_FILE);
    atomic_store_explicit(&RDNA4VS.syncWb, (RDNA4VS.canStatSyncWb && access(GARNET_VS_SYNC_WB_FILE, F_OK) == 0) ? 1 : 0, memory_order_relaxed);
    garnet_vs_init(&RDNA4VS.c, on, cap);
    if (RDNA4VS.on) {
        RDNA4VS.flushQ = dispatch_queue_create("rdna4.vs-flush", DISPATCH_QUEUE_SERIAL);
        RDNA4VS.timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, RDNA4VS.flushQ);
        dispatch_source_set_event_handler(RDNA4VS.timer, ^{ rdna4_vs_idle_timer_fire(); });
        dispatch_source_set_timer(RDNA4VS.timer, DISPATCH_TIME_FOREVER, DISPATCH_TIME_FOREVER, 0);
        dispatch_resume(RDNA4VS.timer);
        RDNA4LOG("Phase B vram_surf: ON (%s, cap=%llu MiB, idle_flush=%llu ms, sync_wb=%d; kill file %s absent)",
                 RDNA4R.barPolicy.full_bar ? "FULL-BAR" : "SMALL-BAR", (unsigned long long)(cap >> 20),
                 (unsigned long long)(GARNET_VS_IDLE_FLUSH_NS / 1000000ULL),
                 atomic_load_explicit(&RDNA4VS.syncWb, memory_order_relaxed), GARNET_VS_KILL_FILE);
    } else {
        RDNA4LOG("Phase B vram_surf: OFF (ws=%d kill=%d)", rdna4_is_ws() ? 1 : 0, killPresent);
    }
}

GARNET_INTERNAL BOOL rdna4_vs_enabled(void) {
    return RDNA4VS.on;
}

GARNET_INTERNAL void rdna4_vs_note_pathb(BOOL skipped) {
    if (skipped) atomic_fetch_add_explicit(&RDNA4VS.pathbSkipped, 1, memory_order_relaxed);
    else atomic_fetch_add_explicit(&RDNA4VS.pathbUploads, 1, memory_order_relaxed);
}

GARNET_INTERNAL BOOL rdna4_vs_sync_wb_on(void) {
    return atomic_load_explicit(&RDNA4VS.syncWb, memory_order_relaxed) != 0;
}

static void rdna4_vs_arm_timer_locked(void) {
    if (!RDNA4VS.timer) return;
    if (RDNA4VS.c.dirty_count > 0 && !RDNA4VS.timerArmed) {
        RDNA4VS.timerArmed = YES;
        dispatch_source_set_timer(RDNA4VS.timer, dispatch_time(DISPATCH_TIME_NOW, (int64_t)GARNET_VS_IDLE_FLUSH_NS),
                                  25 * NSEC_PER_MSEC, 5 * NSEC_PER_MSEC);
    } else if (RDNA4VS.c.dirty_count == 0 && RDNA4VS.timerArmed) {
        RDNA4VS.timerArmed = NO;
        dispatch_source_set_timer(RDNA4VS.timer, DISPATCH_TIME_FOREVER, DISPATCH_TIME_FOREVER, 0);
    }
}

static void rdna4_vs_drain(void) {
    if (!RDNA4VS.on) return;
    uint64_t st, comp; rdna4_imp_fence(&st, &comp);
    for (;;) {
        garnet_vs_rel_t rel;
        pthread_mutex_lock(&RDNA4VS.mu);
        int got = garnet_vs_pop_release(&RDNA4VS.c, comp, &rel);
        pthread_mutex_unlock(&RDNA4VS.mu);
        if (!got) break;
        if (rel.opt_img) vkDestroyImage(RDNA4R.dev, (VkImage)rel.opt_img, NULL);
        if (rel.opt_mem) {
            vkFreeMemory(RDNA4R.dev, (VkDeviceMemory)rel.opt_mem, NULL);
            atomic_fetch_sub(&rdna4_alloc_total, rel.vram_bytes);
        }
        if (rel.ibuf) vkDestroyBuffer(RDNA4R.dev, (VkBuffer)rel.ibuf, NULL);
        if (rel.imem) rdna4_imp_put((VkDeviceMemory)rel.imem);
        if (rel.owner) CFRelease((IOSurfaceRef)rel.owner);
    }
}

static BOOL rdna4_vs_run_flush(uint64_t vsId, uint64_t snapGen, VkImage optImg, VkBuffer ibuf,
                               VkImageLayout savedLayout, uint32_t w, uint32_t h, uint32_t bpp,
                               uint64_t poff, uint64_t bpr) {
    NSError *err = nil;
    BOOL unresolved = NO;
    BOOL ok = NO;
    if (optImg && ibuf && w && h && bpp && savedLayout != VK_IMAGE_LAYOUT_UNDEFINED) {
        ok = rdna4_oneshot(&err, ^(VkCommandBuffer cmd) {
            VkImageMemoryBarrier toSrc = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                .oldLayout = savedLayout, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = optImg, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, NULL, 0, NULL, 1, &toSrc);
            VkBufferImageCopy bic = { .bufferOffset = poff, .bufferRowLength = (uint32_t)(bpr / bpp), .bufferImageHeight = 0,
                .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { w, h, 1 } };
            vkCmdCopyImageToBuffer(cmd, optImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ibuf, 1, &bic);
            VkMemoryBarrier host = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT };
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                                 0, 1, &host, 0, NULL, 0, NULL);
            VkImageMemoryBarrier back = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, .newLayout = savedLayout,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = optImg, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                 0, 0, NULL, 0, NULL, 1, &back);
        }, &unresolved);
    }
    uint64_t st, comp; rdna4_imp_fence(&st, &comp);
    pthread_mutex_lock(&RDNA4VS.mu);
    if (unresolved) {
        // Quarantine entry permanently if GPU timed out so handles are never freed while in flight.
        garnet_vs_ent_t *e = garnet_vs_find(&RDNA4VS.c, vsId);
        if (e) { e->stale = 1; e->refs++; }
    }
    garnet_vs_finish_flush(&RDNA4VS.c, vsId, snapGen, ok ? 1 : 0);
    garnet_vs_retire_stale_ready(&RDNA4VS.c, st);
    rdna4_vs_arm_timer_locked();
    pthread_cond_broadcast(&RDNA4VS.flushCond);
    pthread_mutex_unlock(&RDNA4VS.mu);
    rdna4_vs_drain();
    return ok;
}

static void rdna4_vs_wait_flush_done(uint64_t vsId) {
    if (!RDNA4VS.on || !vsId) return;
    const uint64_t deadline = rdna4_now() + 2000000000ULL;
    pthread_mutex_lock(&RDNA4VS.mu);
    while (garnet_vs_is_flushing(&RDNA4VS.c, vsId) &&
           atomic_load(&rdna4_submit_fault) == VK_SUCCESS &&
           rdna4_now() < deadline) {
        struct timespec ts = { 0, 50000000 };
        (void)pthread_cond_timedwait_relative_np(&RDNA4VS.flushCond, &RDNA4VS.mu, &ts);
    }
    pthread_mutex_unlock(&RDNA4VS.mu);
}

GARNET_INTERNAL BOOL rdna4_vs_flush_entry_sync(uint64_t vsId) {
    if (!RDNA4VS.on || !vsId) return YES;
    const uint64_t deadline = rdna4_now() + 2000000000ULL;
    uint64_t snapGen = 0;
    VkImage optImg = VK_NULL_HANDLE;
    VkBuffer ibuf = VK_NULL_HANDLE;
    VkImageLayout savedLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t w = 0, h = 0, bpp = 4;
    uint64_t poff = 0, bpr = 0;
    BOOL doFlush = NO, clean = NO;
    pthread_mutex_lock(&RDNA4VS.mu);
    for (;;) {
        garnet_vs_ent_t *e = garnet_vs_find(&RDNA4VS.c, vsId);
        if (!e) { clean = YES; break; }
        if (e->flushing || e->active_cbs > 0) {
            if (atomic_load(&rdna4_submit_fault) != VK_SUCCESS || rdna4_now() >= deadline) {
                clean = NO;
                break;
            }
            struct timespec ts = { 0, 50000000 }; // 50 ms slice inside 2 s total deadline
            (void)pthread_cond_timedwait_relative_np(&RDNA4VS.flushCond, &RDNA4VS.mu, &ts);
            continue;
        }
        if (garnet_vs_begin_sync_flush(&RDNA4VS.c, vsId, &snapGen)) {
            optImg = (VkImage)e->opt_img;
            ibuf = (VkBuffer)e->ibuf;
            savedLayout = (VkImageLayout)e->layout;
            w = e->range.w; h = e->range.h; bpp = e->bpp ? e->bpp : 4u;
            poff = e->range.poff; bpr = e->range.bpr;
            doFlush = YES;
        } else {
            if (e->layout == 0) garnet_vs_clear_dirty(&RDNA4VS.c, e);
            clean = !e->host_dirty;
        }
        break;
    }
    pthread_mutex_unlock(&RDNA4VS.mu);
    if (doFlush) return rdna4_vs_run_flush(vsId, snapGen, optImg, ibuf, savedLayout, w, h, bpp, poff, bpr);
    uint64_t st, comp; rdna4_imp_fence(&st, &comp);
    pthread_mutex_lock(&RDNA4VS.mu);
    garnet_vs_retire_stale_ready(&RDNA4VS.c, st);
    rdna4_vs_arm_timer_locked();
    pthread_mutex_unlock(&RDNA4VS.mu);
    rdna4_vs_drain();
    return clean;
}

static void rdna4_vs_idle_timer_fire(void) {
    if (!RDNA4VS.on || !RDNA4R.ok) return;
    if (atomic_load(&rdna4_submit_fault) != VK_SUCCESS) {
        pthread_mutex_lock(&RDNA4VS.mu);
        if (RDNA4VS.timer && RDNA4VS.timerArmed) {
            RDNA4VS.timerArmed = NO;
            dispatch_source_set_timer(RDNA4VS.timer, DISPATCH_TIME_FOREVER, DISPATCH_TIME_FOREVER, 0);
        }
        pthread_mutex_unlock(&RDNA4VS.mu);
        return;
    }
    pthread_mutex_lock(&RDNA4VS.mu);
    const size_t maxIter = RDNA4VS.c.n < 256 ? RDNA4VS.c.n + 1 : 256;
    pthread_mutex_unlock(&RDNA4VS.mu);
    for (size_t iter = 0; iter < maxIter; iter++) {
        if (atomic_load(&rdna4_submit_fault) != VK_SUCCESS) break;
        uint64_t fid = 0, fgen = 0;
        VkImage optImg = VK_NULL_HANDLE;
        VkBuffer ibuf = VK_NULL_HANDLE;
        VkImageLayout savedLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        uint32_t w = 0, h = 0, bpp = 4;
        uint64_t poff = 0, bpr = 0;
        pthread_mutex_lock(&RDNA4VS.mu);
        int picked = garnet_vs_pick_idle_flush(&RDNA4VS.c, rdna4_now(), &fid, &fgen);
        if (picked) {
            garnet_vs_ent_t *e = garnet_vs_find(&RDNA4VS.c, fid);
            if (e) {
                optImg = (VkImage)e->opt_img;
                ibuf = (VkBuffer)e->ibuf;
                savedLayout = (VkImageLayout)e->layout;
                w = e->range.w; h = e->range.h; bpp = e->bpp ? e->bpp : 4u;
                poff = e->range.poff; bpr = e->range.bpr;
            }
        } else {
            rdna4_vs_arm_timer_locked();
        }
        pthread_mutex_unlock(&RDNA4VS.mu);
        if (!picked) break;
        if (!rdna4_vs_run_flush(fid, fgen, optImg, ibuf, savedLayout, w, h, bpp, poff, bpr)) break;
    }
}

static void rdna4_vs_evict_if_needed(void) {
    if (!RDNA4VS.on) return;
    for (;;) {
        uint64_t st, comp; rdna4_imp_fence(&st, &comp);
        uint64_t evId = 0; int needsFlush = 0;
        pthread_mutex_lock(&RDNA4VS.mu);
        int got = garnet_vs_pick_evict_candidate(&RDNA4VS.c, &evId, &needsFlush);
        if (got && !needsFlush) (void)garnet_vs_retire(&RDNA4VS.c, evId, st);
        pthread_mutex_unlock(&RDNA4VS.mu);
        if (!got) break;
        if (needsFlush) {
            if (!rdna4_vs_flush_entry_sync(evId)) break;
            rdna4_imp_fence(&st, &comp);
            pthread_mutex_lock(&RDNA4VS.mu);
            int retired = garnet_vs_retire(&RDNA4VS.c, evId, st);
            pthread_mutex_unlock(&RDNA4VS.mu);
            if (!retired) break;
        }
    }
    rdna4_vs_drain();
}

static BOOL rdna4_vs_bt_is_capture(void) {
    NSArray<NSNumber *> *frames = [NSThread callStackReturnAddresses];
    for (NSUInteger i = 0; i < frames.count; i++) {
        Dl_info di;
        void *p = (void *)(uintptr_t)frames[i].unsignedLongLongValue;
        if (!dladdr(p, &di)) continue;
        if (garnet_vs_symbol_is_capture(di.dli_sname)) return YES;
    }
    return NO;
}

GARNET_INTERNAL void rdna4_vs_observe_ws_write(RDNA4Texture *t) {
    if (!RDNA4VS.on || !t) return;
    t = [t rootResource];
    if ([t rdna4IsDisp] || [t rdna4VramSurfCached] || [t rdna4HostReadOnly]) return;
    IOSurfaceRef s = [t rdna4IOSRef];
    if (!s || IOSurfaceGetPlaneCount(s) > 1) return;
    const uint32_t sid = IOSurfaceGetID(s);
    const uint32_t w = (uint32_t)[t width], h = (uint32_t)[t height];
    if (!sid || w < GARNET_VS_CLASS2_MIN_DIM || h < GARNET_VS_CLASS2_MIN_DIM) return;
    pthread_mutex_lock(&RDNA4VS.mu);
    const int needBt = garnet_vs_obs_needs_bt(&RDNA4VS.c, sid);
    pthread_mutex_unlock(&RDNA4VS.mu);
    const int isCap = needBt ? (rdna4_vs_bt_is_capture() ? 1 : 0) : 0;
    pthread_mutex_lock(&RDNA4VS.mu);
    (void)garnet_vs_note_ws_write(&RDNA4VS.c, sid, w, h, 0, isCap, rdna4_now());
    pthread_mutex_unlock(&RDNA4VS.mu);
}

GARNET_INTERNAL VkImageLayout rdna4_vs_get_layout(uint64_t vsId) {
    if (!RDNA4VS.on || !vsId) return VK_IMAGE_LAYOUT_UNDEFINED;
    pthread_mutex_lock(&RDNA4VS.mu);
    VkImageLayout l = (VkImageLayout)garnet_vs_get_layout(&RDNA4VS.c, vsId);
    pthread_mutex_unlock(&RDNA4VS.mu);
    return l;
}

GARNET_INTERNAL void rdna4_vs_set_layout(uint64_t vsId, VkImageLayout l) {
    if (!RDNA4VS.on || !vsId) return;
    pthread_mutex_lock(&RDNA4VS.mu);
    garnet_vs_set_layout(&RDNA4VS.c, vsId, (uint32_t)l);
    pthread_mutex_unlock(&RDNA4VS.mu);
}

GARNET_INTERNAL void rdna4_vs_cb_begin(uint64_t vsId) {
    if (!RDNA4VS.on || !vsId) return;
    pthread_mutex_lock(&RDNA4VS.mu);
    while (garnet_vs_is_flushing(&RDNA4VS.c, vsId)) {
        struct timespec ts = { 0, 50000000 };
        (void)pthread_cond_timedwait_relative_np(&RDNA4VS.flushCond, &RDNA4VS.mu, &ts);
    }
    garnet_vs_cb_begin(&RDNA4VS.c, vsId);
    pthread_mutex_unlock(&RDNA4VS.mu);
}

GARNET_INTERNAL void rdna4_vs_cb_end(uint64_t vsId) {
    if (!RDNA4VS.on || !vsId) return;
    uint64_t st, comp; rdna4_imp_fence(&st, &comp);
    pthread_mutex_lock(&RDNA4VS.mu);
    garnet_vs_cb_end(&RDNA4VS.c, vsId);
    garnet_vs_retire_stale_ready(&RDNA4VS.c, st);
    pthread_cond_broadcast(&RDNA4VS.flushCond);
    pthread_mutex_unlock(&RDNA4VS.mu);
}

GARNET_INTERNAL BOOL rdna4_vs_peek_needs_upload(uint64_t vsId, uint32_t seed) {
    if (!RDNA4VS.on || !vsId) return YES;
    pthread_mutex_lock(&RDNA4VS.mu);
    const int up = garnet_vs_peek_needs_upload(&RDNA4VS.c, vsId, seed);
    pthread_mutex_unlock(&RDNA4VS.mu);
    return up != 0;
}

GARNET_INTERNAL void rdna4_vs_note_upload_skip(void) {
    if (!RDNA4VS.on) return;
    pthread_mutex_lock(&RDNA4VS.mu);
    RDNA4VS.c.uploads_skipped++;
    pthread_mutex_unlock(&RDNA4VS.mu);
}

GARNET_INTERNAL BOOL rdna4_vs_needs_upload(uint64_t vsId, uint32_t seed, BOOL needContents, BOOL willWrite, BOOL fullOverwrite) {
    if (!RDNA4VS.on || !vsId) return needContents;
    pthread_mutex_lock(&RDNA4VS.mu);
    const int up = garnet_vs_begin_access(&RDNA4VS.c, vsId, seed, needContents ? 1 : 0, willWrite ? 1 : 0, fullOverwrite ? 1 : 0);
    pthread_mutex_unlock(&RDNA4VS.mu);
    return up != 0;
}

GARNET_INTERNAL void rdna4_vs_note_full_clear(uint64_t vsId, uint32_t seed) {
    if (!RDNA4VS.on || !vsId) return;
    pthread_mutex_lock(&RDNA4VS.mu);
    (void)garnet_vs_begin_access(&RDNA4VS.c, vsId, seed, 0, 1, 1);
    pthread_mutex_unlock(&RDNA4VS.mu);
}

GARNET_INTERNAL void rdna4_vs_note_write(uint64_t vsId, uint32_t seed, BOOL syncWriteback) {
    if (!RDNA4VS.on || !vsId) return;
    pthread_mutex_lock(&RDNA4VS.mu);
    garnet_vs_note_gpu_write(&RDNA4VS.c, vsId, seed, rdna4_now(), syncWriteback ? 1 : 0);
    rdna4_vs_arm_timer_locked();
    pthread_mutex_unlock(&RDNA4VS.mu);
}

GARNET_INTERNAL void rdna4_vs_invalidate_cpu(uint64_t vsId) {
    if (!RDNA4VS.on || !vsId) return;
    pthread_mutex_lock(&RDNA4VS.mu);
    garnet_vs_invalidate_cpu_write(&RDNA4VS.c, vsId);
    pthread_mutex_unlock(&RDNA4VS.mu);
}

static void rdna4_vs_put(uint64_t vsId) {
    if (!RDNA4VS.on || !vsId) return;
    uint64_t st, comp; rdna4_imp_fence(&st, &comp);
    pthread_mutex_lock(&RDNA4VS.mu);
    (void)garnet_vs_unref(&RDNA4VS.c, vsId, rdna4_now(), st);
    pthread_mutex_unlock(&RDNA4VS.mu);
    rdna4_vs_evict_if_needed();
}

static BOOL rdna4_vs_try_bind(IOSurfaceRef s, void *base, size_t ialloc, size_t alloc, NSUInteger poff,
                              NSUInteger w, NSUInteger h, size_t bpr, const RDNA4Fmt *fmt,
                              NSUInteger mtlUsage, VkImageUsageFlags reqUse, VkFormatFeatureFlags offs,
                              BOOL attOK, BOOL isDisp, BOOL hostReadOnly, const VkMemoryAllocateInfo *ma,
                              VkImage *outImg, VkBuffer *outIbuf, uint64_t *outId) {
    if (!RDNA4VS.on || !RDNA4IC.on || !s || !fmt) return NO;
    uint64_t st, comp; rdna4_imp_fence(&st, &comp);
    const garnet_vs_range_t vr = { .sid = IOSurfaceGetID(s), .base = (uint64_t)(uintptr_t)base,
        .ialloc = ialloc, .poff = poff, .w = (uint32_t)w, .h = (uint32_t)h, .bpr = bpr };
    const uint64_t now = rdna4_now();
    uint64_t hitId = 0;
    VkImage hitImg = VK_NULL_HANDLE;
    VkBuffer hitBuf = VK_NULL_HANDLE;
    int probe = GARNET_VS_PROBE_MISS;
    int promote = 0;
    pthread_mutex_lock(&RDNA4VS.mu);
    probe = garnet_vs_probe(&RDNA4VS.c, &vr, (uint32_t)fmt->vk, (uint32_t)reqUse, now, st, &hitId);
    if (probe == GARNET_VS_PROBE_HIT) {
        garnet_vs_ent_t *e = garnet_vs_find(&RDNA4VS.c, hitId);
        if (e) {
            if (isDisp) e->is_disp = 1;
            hitImg = (VkImage)e->opt_img; hitBuf = (VkBuffer)e->ibuf;
        }
    } else if (probe == GARNET_VS_PROBE_MISS) {
        const int wantWrite = (mtlUsage & (MTLTextureUsageRenderTarget | MTLTextureUsageShaderWrite)) ? 1 : 0;
        promote = garnet_vs_should_promote(&RDNA4VS.c, vr.sid, vr.w, vr.h, isDisp ? 1 : 0, hostReadOnly ? 1 : 0, 1, wantWrite);
    }
    pthread_mutex_unlock(&RDNA4VS.mu);

    if (probe == GARNET_VS_PROBE_HIT && hitImg && hitBuf) {
        *outImg = hitImg; *outIbuf = hitBuf; *outId = hitId;
        return YES;
    }
    if (probe == GARNET_VS_PROBE_INCOMPAT_FLUSH) {
        if (hitId) rdna4_vs_flush_entry_sync(hitId);
        rdna4_vs_drain();
        return NO;
    }
    if (!promote) return NO;

    // Also inspect creation backtrace before Class 2 promotion so CreateCompositeDestination / capture
    // surfaces allocated on newTexture(iosurface:) are permanently excluded from deferred writeback.
    if (!isDisp && rdna4_vs_bt_is_capture()) {
        pthread_mutex_lock(&RDNA4VS.mu);
        garnet_vs_mark_capture_excluded(&RDNA4VS.c, vr.sid);
        pthread_mutex_unlock(&RDNA4VS.mu);
        return NO;
    }

    const VkFormatFeatureFlags needOpt = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                                         VK_FORMAT_FEATURE_TRANSFER_DST_BIT | (attOK ? VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT : 0);
    if ((offs & needOpt) != needOpt) return NO;

    uint32_t baseFmt = (uint32_t)fmt->vk, fmtListU32[2] = { 0, 0 }, fmtListCnt = 0;
    (void)garnet_vs_format_family((uint32_t)fmt->vk, &baseFmt, fmtListU32, &fmtListCnt);
    // Avoid VUID/DCC hazards: never combine STORAGE_BIT with sRGB-mutable format list, and never promote
    // PixelFormatView surfaces whose format has no mutable view list (flags=0).
    const BOOL wantStorage = (reqUse & VK_IMAGE_USAGE_STORAGE_BIT) != 0;
    if (wantStorage && fmtListCnt == 2) return NO;
    if ((mtlUsage & MTLTextureUsagePixelFormatView) && fmtListCnt == 0) return NO;

    const VkFormat vkFmtList[2] = { (VkFormat)fmtListU32[0], (VkFormat)fmtListU32[1] };
    VkImageFormatListCreateInfo flci = { .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
        .viewFormatCount = fmtListCnt, .pViewFormats = vkFmtList };
    const VkImageUsageFlags superUse = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT |
        (attOK ? (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT) : 0) |
        (wantStorage ? VK_IMAGE_USAGE_STORAGE_BIT : 0);
    VkImageCreateInfo ic = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = fmtListCnt == 2 ? &flci : NULL,
        .flags = fmtListCnt == 2 ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0,
        .imageType = VK_IMAGE_TYPE_2D, .format = (VkFormat)baseFmt,
        .extent = { (uint32_t)w, (uint32_t)h, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = superUse, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    if (garnet_async_compute_use_concurrent_image(RDNA4R.asyncComputeOn ? 1 : 0, RDNA4R.qfi, RDNA4R.computeQfi, 0)) {
        ic.sharingMode = VK_SHARING_MODE_CONCURRENT;
        ic.queueFamilyIndexCount = 2;
        ic.pQueueFamilyIndices = RDNA4R.sharedQfis;
    }
    VkImage optImg = VK_NULL_HANDLE;
    if (vkCreateImage(RDNA4R.dev, &ic, NULL, &optImg) != VK_SUCCESS) return NO;
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(RDNA4R.dev, optImg, &mr);
    const int mt = rdna4_find_vram_hipool_mem(mr.memoryTypeBits);
    if (mt < 0) { vkDestroyImage(RDNA4R.dev, optImg, NULL); return NO; }
    VkMemoryAllocateInfo vma = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)mt };
    VkDeviceMemory optMem = VK_NULL_HANDLE;
    if (vkAllocateMemory(RDNA4R.dev, &vma, NULL, &optMem) != VK_SUCCESS) {
        vkDestroyImage(RDNA4R.dev, optImg, NULL); return NO;
    }
    if (vkBindImageMemory(RDNA4R.dev, optImg, optMem, 0) != VK_SUCCESS) {
        vkDestroyImage(RDNA4R.dev, optImg, NULL); vkFreeMemory(RDNA4R.dev, optMem, NULL); return NO;
    }
    VkDeviceMemory entryImem = VK_NULL_HANDLE; BOOL entryImpHit = NO;
    if (rdna4_imp_get(s, base, ialloc, ma, &entryImem, &entryImpHit) != VK_SUCCESS || !entryImem) {
        vkDestroyImage(RDNA4R.dev, optImg, NULL); vkFreeMemory(RDNA4R.dev, optMem, NULL); return NO;
    }
    if (!entryImpHit) @synchronized ([GarnetDevice class]) { RDNA4R.impTotal += ialloc; }
    VkExternalMemoryBufferCreateInfo emb = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT };
    VkBufferCreateInfo bc = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &emb,
        .size = alloc, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    if (garnet_async_compute_use_concurrent_buffer(RDNA4R.asyncComputeOn ? 1 : 0, RDNA4R.qfi, RDNA4R.computeQfi)) {
        bc.sharingMode = VK_SHARING_MODE_CONCURRENT;
        bc.queueFamilyIndexCount = 2;
        bc.pQueueFamilyIndices = RDNA4R.sharedQfis;
    }
    VkBuffer entryIbuf = VK_NULL_HANDLE;
    if (vkCreateBuffer(RDNA4R.dev, &bc, NULL, &entryIbuf) != VK_SUCCESS ||
        vkBindBufferMemory(RDNA4R.dev, entryIbuf, entryImem, 0) != VK_SUCCESS) {
        if (entryIbuf) vkDestroyBuffer(RDNA4R.dev, entryIbuf, NULL);
        rdna4_imp_put(entryImem);
        vkDestroyImage(RDNA4R.dev, optImg, NULL);
        vkFreeMemory(RDNA4R.dev, optMem, NULL);
        return NO;
    }

    IOSurfaceRef owner = (IOSurfaceRef)CFRetain(s);
    uint64_t finalId = 0;
    BOOL raced = NO, abortInsert = NO;
    uint64_t incompatId = 0;
    rdna4_imp_fence(&st, &comp);
    pthread_mutex_lock(&RDNA4VS.mu);
    int reProbe = garnet_vs_probe(&RDNA4VS.c, &vr, (uint32_t)fmt->vk, (uint32_t)reqUse, rdna4_now(), st, &hitId);
    if (reProbe == GARNET_VS_PROBE_HIT) {
        garnet_vs_ent_t *e = garnet_vs_find(&RDNA4VS.c, hitId);
        if (e) { if (isDisp) e->is_disp = 1; hitImg = (VkImage)e->opt_img; hitBuf = (VkBuffer)e->ibuf; finalId = hitId; raced = YES; }
    } else if (reProbe == GARNET_VS_PROBE_INCOMPAT_FLUSH) {
        abortInsert = YES;
        incompatId = hitId;
    }
    if (!raced && !abortInsert) {
        finalId = garnet_vs_add(&RDNA4VS.c, &vr, (uint32_t)fmt->vk, (uint32_t)superUse, mr.size,
                                (void *)optImg, (void *)optMem, (void *)entryIbuf, (void *)entryImem, (void *)owner, rdna4_now());
        if (finalId) {
            garnet_vs_set_bpp(&RDNA4VS.c, finalId, fmt->bpp);
            garnet_vs_set_disp(&RDNA4VS.c, finalId, isDisp ? 1 : 0);
            atomic_fetch_add(&rdna4_alloc_total, mr.size);
        } else {
            abortInsert = YES;
        }
    }
    pthread_mutex_unlock(&RDNA4VS.mu);

    if (raced || abortInsert) {
        vkDestroyImage(RDNA4R.dev, optImg, NULL);
        vkFreeMemory(RDNA4R.dev, optMem, NULL);
        vkDestroyBuffer(RDNA4R.dev, entryIbuf, NULL);
        rdna4_imp_put(entryImem);
        CFRelease(owner);
        if (abortInsert) {
            if (incompatId) rdna4_vs_flush_entry_sync(incompatId);
            rdna4_vs_drain();
            return NO;
        }
        *outImg = hitImg; *outIbuf = hitBuf; *outId = finalId;
        return YES;
    }
    rdna4_vs_evict_if_needed();
    *outImg = optImg; *outIbuf = entryIbuf; *outId = finalId;
    return YES;
}

GARNET_INTERNAL void rdna4_vs_tick(void) {
    if (!RDNA4VS.on) return;
    if (RDNA4VS.canStatSyncWb)
        atomic_store_explicit(&RDNA4VS.syncWb, access(GARNET_VS_SYNC_WB_FILE, F_OK) == 0 ? 1 : 0, memory_order_relaxed);
    rdna4_vs_evict_if_needed();
}

GARNET_INTERNAL void rdna4_vs_report(unsigned tick) {
    if (!RDNA4VS.on) return;
    const uint64_t pbUp = atomic_load_explicit(&RDNA4VS.pathbUploads, memory_order_relaxed);
    const uint64_t pbSk = atomic_load_explicit(&RDNA4VS.pathbSkipped, memory_order_relaxed);
    pthread_mutex_lock(&RDNA4VS.mu);
    const garnet_vs_cache_t *c = &RDNA4VS.c;
    const uint64_t sig = c->created + c->hits + c->uploads + c->uploads_skipped + c->lazy_writes + c->idle_flushes + c->evictions + pbUp + pbSk;
    if (sig != RDNA4VS.lastSig || tick % 6 == 5) {
        RDNA4VS.lastSig = sig;
        RDNA4LOG("T1 vram_surf ON: live=%llu MiB (%zu entries) unused=%llu MiB dirty=%zu created=%llu hits=%llu misses=%llu uploads=%llu skipped=%llu pathb_up=%llu pathb_skip=%llu lazy_wb=%llu sync_wb=%llu idle_flush=%llu incompat=%llu evict=%llu cap_excl=%llu [%s pid %d]",
                 (unsigned long long)(c->live_bytes >> 20), c->n, (unsigned long long)(c->unused_bytes >> 20), c->dirty_count,
                 (unsigned long long)c->created, (unsigned long long)c->hits, (unsigned long long)c->misses,
                 (unsigned long long)c->uploads, (unsigned long long)c->uploads_skipped,
                 (unsigned long long)pbUp, (unsigned long long)pbSk,
                 (unsigned long long)c->lazy_writes,
                 (unsigned long long)c->sync_writes, (unsigned long long)c->idle_flushes, (unsigned long long)c->incompat_flushes,
                 (unsigned long long)c->evictions, (unsigned long long)c->capture_excluded_sids, getprogname(), (int)getpid());
    }
    pthread_mutex_unlock(&RDNA4VS.mu);
}

// ---------------------------------------------------------------------------------------------------------------
// Metal API gap census fixes (driver design). Shared pieces:
//   RDNA4_ONCE          one log line per call site per process (no-op stubs are called thousands of times a second by the compositor)
//   RDNA4_RES_IVARS /   state + the public/SPI MTLResource surface that QuartzCore / SkyLight send to every texture and buffer
//   RDNA4_RESOURCE_SPI  (census: -setResponsibleProcess: 7450, -protectionOptions 6741, -heap 1397, -virtualAddress 1092 in 45 s)
// ---------------------------------------------------------------------------------------------------------------
static _Atomic uint64_t rdna4_uid_ctr;
GARNET_INTERNAL uint64_t rdna4_resource_uid(_Atomic uint64_t *uid) {
    uint64_t current = atomic_load_explicit(uid, memory_order_relaxed);
    if (current) return current;
    uint64_t candidate = atomic_fetch_add_explicit(&rdna4_uid_ctr, 1, memory_order_relaxed) + 1;
    uint64_t expected = 0;
    return atomic_compare_exchange_strong_explicit(uid, &expected, candidate,
        memory_order_relaxed, memory_order_relaxed) ? candidate : expected;
}
GARNET_INTERNAL _Atomic uint64_t rdna4_alloc_total;   // bytes of VkDeviceMemory held by live buffers and textures (-[MTLDevice currentAllocatedSize])
GARNET_INTERNAL _Atomic uint64_t rdna4_heap_reserved;   // heap capacity promises; not device memory and not deducted from rdna4_alloc_total
// Heap membership (m11 H1): a resource made by -[RDNA4Heap newBuffer.../newTexture...] is a normal VkBuffer/VkImage that carries its heap, the offset the heap
// assigned, the bytes it was charged and the block id; the budget goes back to the heap at -makeAliasable or at dealloc, whichever is first.
// Detach decides whether Vulkan handles can die now (shared block) or must stay
// with the dedicated lease's retained owner until its retirement completes.

// ---------------------------------------------------------------------------------------------------------------
// RDNA4Buffer: minimal MTLBuffer (10a: Shared only) = VkBuffer + host-visible coherent VkDeviceMemory, mapped once.
// ---------------------------------------------------------------------------------------------------------------

GARNET_INTERNAL BOOL rdna4nc_vk_offset_ok(VkDeviceSize off, VkDeviceSize align, unsigned use) {
    if (!align || off % align == 0) return YES;
    static _Atomic unsigned warned;
    static const char *const names[] = { "storage descriptor", "uniform descriptor", "index buffer", "indirect command", "buffer/image copy" };
    if (!(atomic_fetch_or(&warned, 1u << use) & (1u << use)))
        RDNA4LOG_WARN("NoCopy Vulkan alignment limit: %s offset %llu must be a multiple of %llu; operation refused before encoding", names[use], (unsigned long long)off, (unsigned long long)align);
    return NO;
}


// The heap requires requirements2 so dedicated requirements are never guessed.
// Non-heap callers retain the Vulkan 1.0 fallback for an old loader, where they
// cannot choose a dedicated allocation anyway.
GARNET_INTERNAL BOOL rdna4_requirements2(VkBuffer buffer, VkImage image, VkMemoryRequirements *mr,
                                BOOL *prefersDedicated, BOOL *requiresDedicated,
                                BOOL requireDedicatedInfo) {
    if (requireDedicatedInfo &&
        ((buffer && !RDNA4P.bmr2) || (image && !RDNA4P.imr2))) return NO;
    VkMemoryDedicatedRequirements dedicated = { .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS };
    VkMemoryRequirements2 req = { .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = &dedicated };
    if (buffer && RDNA4P.bmr2) {
        VkBufferMemoryRequirementsInfo2 info = { .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2, .buffer = buffer };
        RDNA4P.bmr2(RDNA4R.dev, &info, &req);
    } else if (image && RDNA4P.imr2) {
        VkImageMemoryRequirementsInfo2 info = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, .image = image };
        RDNA4P.imr2(RDNA4R.dev, &info, &req);
    } else {
        if (buffer) vkGetBufferMemoryRequirements(RDNA4R.dev, buffer, &req.memoryRequirements);
        else if (image) vkGetImageMemoryRequirements(RDNA4R.dev, image, &req.memoryRequirements);
        memset(&dedicated, 0, sizeof dedicated);
    }
    if (mr) *mr = req.memoryRequirements;
    if (prefersDedicated) *prefersDedicated = dedicated.prefersDedicatedAllocation != VK_FALSE;
    if (requiresDedicated) *requiresDedicated = dedicated.requiresDedicatedAllocation != VK_FALSE;
    return YES;
}
// One source of truth for the heap buffer's create info. `external` is non-NULL
// only for the >64 MiB Shared/Managed heap import path.
GARNET_INTERNAL VkBufferCreateInfo rdna4_heap_buffer_create_info(NSUInteger len, VkExternalMemoryBufferCreateInfo *external) {
    VkBufferCreateInfo info = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = len,
        .pNext = external,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT |
                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                 (RDNA4R.bufferAddress ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0) |
                 (RDNA4R.accelStruct ? VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR : 0) };
    if (garnet_async_compute_use_concurrent_buffer(RDNA4R.asyncComputeOn ? 1 : 0, RDNA4R.qfi, RDNA4R.computeQfi)) {
        info.sharingMode = VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = 2;
        info.pQueueFamilyIndices = RDNA4R.sharedQfis;
    }
    return info;
}
static void *rdna4nc_owned_allocate(uint64_t len, uint64_t hostAlign, uint64_t *span) {
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0 || !hostAlign || (hostAlign & (hostAlign - 1))) return NULL;
    uint64_t align = MAX(hostAlign, (uint64_t)page), base, delta, size;
    if (rdna4nc_import_range(0, len, align, &base, &size, &delta) || size > SIZE_MAX - align) return NULL;
    size_t reserved = (size_t)(size + align);
    void *mapping = mmap(NULL, reserved, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mapping == MAP_FAILED) return NULL;
    uintptr_t start = ((uintptr_t)mapping + align - 1) & ~(uintptr_t)(align - 1);
    size_t prefix = start - (uintptr_t)mapping, suffix = reserved - prefix - (size_t)size;
    if (prefix) (void)munmap(mapping, prefix);
    if (suffix) (void)munmap((void *)(start + size), suffix);
    *span = size;
    return (void *)start;
}

// The heap owns physical memory, not the Metal resource object. A dedicated block must
// outlive the resource's Vulkan handles until the completion point, so retain this small
// handle owner in the lease instead of a raw RDNA4Buffer/RDNA4Texture pointer.
@interface RDNA4HeapBacking (RDNA4HeapHandleState)
- (void)rdna4MarkHandleOpen;
- (BOOL)rdna4ConsumeHandleOpen;
@end

@implementation RDNA4HeapBacking {
    VkBuffer _buffer;
    VkImage _image;
    NSMutableArray<NSNumber *> *_views;
    BOOL _destroyed;
    BOOL _handleOpen;
}
- (instancetype)initWithBuffer:(VkBuffer)buffer image:(VkImage)image view:(VkImageView)view {
    self = [super init];
    if (!self) return nil;
    _buffer = buffer;
    _image = image;
    _views = [NSMutableArray array];
    if (view) [_views addObject:@((uintptr_t)view)];
    return self;
}
- (void)rdna4MarkHandleOpen {
    @synchronized (self) { _handleOpen = YES; }
}
- (BOOL)rdna4ConsumeHandleOpen {
    @synchronized (self) {
        BOOL was = _handleOpen;
        _handleOpen = NO;
        return was;
    }
}
- (void)addView:(VkImageView)view {
    if (!view) return;
    @synchronized (self) {
        if (_destroyed) {
            if (RDNA4R.ok) vkDestroyImageView(RDNA4R.dev, view, NULL);
            return;
        }
        [_views addObject:@((uintptr_t)view)];
    }
}
- (void)destroyHandles {
    @synchronized (self) {
        if (_destroyed) return;
        _destroyed = YES;
        if (RDNA4R.ok) {
            for (NSNumber *v in _views) vkDestroyImageView(RDNA4R.dev, (VkImageView)(uintptr_t)v.unsignedLongLongValue, NULL);
            if (_image) vkDestroyImage(RDNA4R.dev, _image, NULL);
            if (_buffer) vkDestroyBuffer(RDNA4R.dev, _buffer, NULL);
        }
        _views = nil;
        _buffer = VK_NULL_HANDLE;
        _image = VK_NULL_HANDLE;
    }
}
- (void)dealloc {
    [self destroyHandles];
}
@end

@implementation RDNA4Buffer
- (instancetype)initWithDevice:(id)dev length:(NSUInteger)len options:(NSUInteger)opts error:(NSError **)err {
    return [self initWithDevice:dev length:len options:opts heap:nil error:err];
}
- (instancetype)initWithDevice:(id)dev length:(NSUInteger)len options:(NSUInteger)opts heap:(RDNA4Heap *)heap error:(NSError **)err {
    return [self initWithDevice:dev length:len options:opts heap:heap offset:0 hasOffset:NO error:err];
}
- (instancetype)initWithDevice:(id)dev length:(NSUInteger)len options:(NSUInteger)opts heap:(RDNA4Heap *)heap offset:(NSUInteger)offset error:(NSError **)err {
    return [self initWithDevice:dev length:len options:opts heap:heap offset:offset hasOffset:YES error:err];
}
- (instancetype)initWithDevice:(id)dev length:(NSUInteger)len options:(NSUInteger)opts heap:(RDNA4Heap *)heap offset:(NSUInteger)offset hasOffset:(BOOL)hasOffset error:(NSError **)err {
    self = [super init];
    if (!self) return nil;
    _dev = dev; _len = len; BOOL nozero = (opts & RDNA4_OPT_NOZERO) != 0; _opts = opts & ~RDNA4_OPT_NOZERO;   // P1: the marker never leaves this init
    opts = _opts;
    _hpHeap = heap;
    if (!rdna4_radv_open(err)) return nil;
    rdna4dc_caps caps = rdna4_device_caps();
    if (!rdna4dc_buffer_ok(&caps, len)) { if (err) *err = rdna4_err(25, @"buffer length exceeds the implemented logical allocation limit"); return nil; }
    unsigned sm = (unsigned)((opts >> 4) & 0xF);
    if (sm > 2) { if (err) *err = rdna4_err(25, @"buffer storage mode Memoryless is not supported"); return nil; }
    if (!heap && sm < 2 && len > (64ull << 20)) {
        if (!RDNA4R.hostExt || !rdna4_vkGetMHPP) { if (err) *err = rdna4_err(112, @"large Shared/Managed buffers require coherent host import"); return nil; }
        _ptr = rdna4nc_owned_allocate(len, RDNA4R.hostAlign, &_ownedHostSpan);
        if (!_ptr) { if (err) *err = rdna4_err(22, @"cannot reserve aligned CPU memory for the shared buffer"); return nil; }
        return [self rdna4ImportHostPointer:_ptr length:len ownedSpan:_ownedHostSpan deallocator:nil error:err];
    }
    BOOL heapHost = heap && sm < 2;
    BOOL heapImport = heapHost && (hasOffset ? ([heap size] > (64ull << 20)) : (len > (64ull << 20)));
    if (heapImport && (!RDNA4R.hostExt || !rdna4_vkGetMHPP)) { if (err) *err = rdna4_err(112, @"large Shared/Managed heap buffers require coherent host import"); return nil; }
    VkExternalMemoryBufferCreateInfo external = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT };
    VkBufferCreateInfo bc = rdna4_heap_buffer_create_info(len, heapImport ? &external : NULL);
    VkResult r = vkCreateBuffer(RDNA4R.dev, &bc, NULL, &_vkbuf);
    if (r != VK_SUCCESS) { if (err) *err = rdna4_err(20, [NSString stringWithFormat:@"vkCreateBuffer = %d", r]); return nil; }
    VkMemoryRequirements mr; BOOL prefersDedicated = NO, requiresDedicated = NO;
    if (!rdna4_requirements2(_vkbuf, VK_NULL_HANDLE, &mr, &prefersDedicated, &requiresDedicated, heap != nil)) {
        if (err) *err = rdna4_err(22, @"heap buffers require vkGetBufferMemoryRequirements2 and dedicated requirements");
        [self rdna4AbortHeapConstruction];
        return nil;
    }
    BOOL dedicated = prefersDedicated || requiresDedicated;
    _asz = (NSUInteger)mr.size;
    if (heap) {
        VkDeviceMemory hmem = VK_NULL_HANDLE; uint64_t bindOff = 0, rangeSize = 0; uint8_t *map = NULL;
        if (hasOffset) {
            _hpOff = offset;
            _hpSize = (NSUInteger)mr.size;
            if (![heap rdna4AcquirePlacementBackingForBuffer:_vkbuf image:VK_NULL_HANDLE requirements:mr offset:offset
                                                  hostImport:heapImport requiresDedicated:requiresDedicated
                                                         mem:&hmem offset:&bindOff map:&map block:&_hpBlk
                                                       lease:&_hpLease rangeSize:&rangeSize error:err]) {
                [self rdna4AbortHeapConstruction];
                return nil;
            }
            dedicated = NO;
        } else {
            if (![heap rdna4ReserveRequirements:mr offset:&_hpOff id:&_hpId]) {
                if (err) *err = rdna4_err(22, [NSString stringWithFormat:@"heap has no logical range for a %llu-byte buffer requirement", (unsigned long long)mr.size]);
                [self rdna4AbortHeapConstruction];
                return nil;
            }
            _hpSize = (NSUInteger)mr.size;
            BOOL acquired = [heap rdna4AcquireBackingForBuffer:_vkbuf image:VK_NULL_HANDLE requirements:mr dedicated:dedicated hostImport:heapImport
                                                           mem:&hmem offset:&bindOff map:&map block:&_hpBlk lease:&_hpLease rangeSize:&rangeSize error:err];
            if (!acquired && dedicated && !requiresDedicated) {
                if (err) *err = nil;
                dedicated = NO;
                acquired = [heap rdna4AcquireBackingForBuffer:_vkbuf image:VK_NULL_HANDLE requirements:mr dedicated:NO hostImport:heapImport
                                                          mem:&hmem offset:&bindOff map:&map block:&_hpBlk lease:&_hpLease rangeSize:&rangeSize error:err];
            }
            if (!acquired) {
                [self rdna4AbortHeapConstruction];
                return nil;
            }
        }
        _hpBindOff = bindOff; _hpPhysSize = rangeSize; _hpDedicated = dedicated; _noacct = YES;
        r = vkBindBufferMemory(RDNA4R.dev, _vkbuf, hmem, bindOff);
        if (r != VK_SUCCESS) {
            if (err) *err = rdna4_err(23, [NSString stringWithFormat:@"vkBindBufferMemory = %d (heap block, offset %llu)", r, (unsigned long long)bindOff]);
            [self rdna4AbortHeapConstruction];
            return nil;
        }
        if (map) _ptr = map + bindOff;
        if (RDNA4R.bufferAddress) {
            VkBufferDeviceAddressInfo address = { .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = _vkbuf };
            _gpuAddress = rdna4_vkGetBufferAddress(RDNA4R.dev, &address);
            if (!_gpuAddress) {
                if (err) *err = rdna4_err(26, @"Vulkan returned a zero GPU address for the heap buffer");
                [self rdna4AbortHeapConstruction];
                return nil;
            }
        }
        _hpBacking = [[RDNA4HeapBacking alloc] initWithBuffer:_vkbuf image:VK_NULL_HANDLE view:VK_NULL_HANDLE];
        if (!_hpBacking || ![heap rdna4BindSucceeded:_hpBlk lease:_hpLease owner:_hpBacking]) {
            if (err) *err = rdna4_err(22, @"cannot retain the heap buffer's lifecycle backing");
            [self rdna4AbortHeapConstruction];
            return nil;
        }
        if (!hasOffset && sm == 2 && len > RDNA4_BUFFER_LEGACY_MAX && !nozero) {
            BOOL unresolved = NO;
            if (![self rdna4ZeroLargePrivate:len unresolved:&unresolved error:err]) {
                if (!unresolved) [self rdna4AbortHeapConstruction];
                return nil;
            }
        }
        RDNA4LOGR("RDNA4Buffer %p: heap %lu bytes (req %llu, block %llu, bind %llu) mapped at %p", (__bridge void *)self, (unsigned long)len,
               (unsigned long long)mr.size, (unsigned long long)_hpBlk, (unsigned long long)_hpBindOff, _ptr);
        return self;
    }
    // 11e: Shared (0) and Managed (1) are host-visible coherent memory (the CPU and GPU copies are one, so didModifyRange: is a no-op);
    // Private (2) is DEVICE_LOCAL with no CPU mapping (contents = NULL). Memoryless (3) is refused.
    int mt = sm == 2 ? rdna4_find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
                     : rdna4_find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt < 0) { if (err) *err = rdna4_err(21, sm == 2 ? @"no device-local memory type" : @"no host-visible coherent memory type"); return nil; }
    VkMemoryAllocateFlagsInfo flags = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT };
    VkMemoryAllocateInfo ma = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = RDNA4R.bufferAddress ? &flags : NULL,
        .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)mt };
    if (RDNA4P.on) {   // P1: slab range / recycled allocation; the host-visible memory is mapped once by the pool (zeroed here unless the caller said it will overwrite it)
        r = rdna4_mem_alloc((uint32_t)mt, mr, sm != 2, sm != 2 && !nozero, _vkbuf, VK_NULL_HANDLE, &_pm);
        if (r == VK_SUCCESS) {
            _pooled = YES; atomic_fetch_add(&rdna4_alloc_total, (uint64_t)_asz);
            r = vkBindBufferMemory(RDNA4R.dev, _vkbuf, _pm.mem, _pm.off);
            if (r != VK_SUCCESS) { if (err) *err = rdna4_err(23, [NSString stringWithFormat:@"vkBindBufferMemory = %d (pool, offset %llu)", r, (unsigned long long)_pm.off]); return nil; }
            if (sm != 2) _ptr = _pm.map;
        }
    } else {
        r = vkAllocateMemory(RDNA4R.dev, &ma, NULL, &_vkmem);
        if (r == VK_SUCCESS) {
            atomic_fetch_add(&rdna4_alloc_total, (uint64_t)_asz);
            r = vkBindBufferMemory(RDNA4R.dev, _vkbuf, _vkmem, 0);
            if (r != VK_SUCCESS) { if (err) *err = rdna4_err(23, [NSString stringWithFormat:@"vkBindBufferMemory = %d", r]); return nil; }
            if (sm != 2) {
                r = vkMapMemory(RDNA4R.dev, _vkmem, 0, VK_WHOLE_SIZE, 0, &_ptr);
                if (r != VK_SUCCESS) { if (err) *err = rdna4_err(24, [NSString stringWithFormat:@"vkMapMemory = %d", r]); return nil; }
            }
        }
    }
    if (r != VK_SUCCESS) {
        if (r == VK_ERROR_OUT_OF_DEVICE_MEMORY && !heap && sm < 2 && RDNA4R.hostExt && rdna4_vkGetMHPP) {
            void *owned = rdna4nc_owned_allocate(len, RDNA4R.hostAlign, &_ownedHostSpan);
            if (owned) {
                vkDestroyBuffer(RDNA4R.dev, _vkbuf, NULL);
                _vkbuf = VK_NULL_HANDLE;
                _ptr = owned;
                return [self rdna4ImportHostPointer:owned length:len ownedSpan:_ownedHostSpan deallocator:nil error:err];
            }
        }
        if (err) *err = rdna4_err(22, [NSString stringWithFormat:RDNA4P.on ? @"vkAllocateMemory = %d (pool)" : @"vkAllocateMemory = %d", r]);
        return nil;
    }
    if (RDNA4R.bufferAddress) {
        VkBufferDeviceAddressInfo address = { .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = _vkbuf };
        _gpuAddress = rdna4_vkGetBufferAddress(RDNA4R.dev, &address);
        if (!_gpuAddress) { if (err) *err = rdna4_err(26, @"Vulkan returned a zero GPU address for the bound buffer"); return nil; }
    }
    if (sm == 2 && len > RDNA4_BUFFER_LEGACY_MAX && !nozero &&
        ![self rdna4ZeroLargePrivate:len unresolved:NULL error:err]) return nil;
    RDNA4LOGR("RDNA4Buffer %p: %lu bytes (alloc %llu, memtype %d) mapped at %p", (__bridge void *)self, (unsigned long)len,
           (unsigned long long)mr.size, mt, _ptr);
    return self;
}
- (BOOL)rdna4ZeroLargePrivate:(NSUInteger)len unresolved:(BOOL *)unresolved error:(NSError **)err {
    if (unresolved) *unresolved = NO;
    RDNA4Buffer *edge = nil;
    NSUInteger whole = len & ~(NSUInteger)3, tail = len - whole;
    if (tail) {
        edge = [[RDNA4Buffer alloc] initWithDevice:_dev length:4 options:0 error:err];
        if (!edge || !edge.contents) return NO;
        memset(edge.contents, 0, 4);
    }
    // High-pool children are not CPU mapped or cleared by the kernel. The
    // recording block retains self and the edge so an unresolved submit cannot
    // tear down either resource while the GPU may still use it.
    return rdna4_oneshot(err, ^(VkCommandBuffer cmd) {
        (void)self;
        if (whole) vkCmdFillBuffer(cmd, self->_vkbuf, 0, whole, 0);
        if (tail) {
            VkBufferCopy copy = { [edge rdna4VkOffset], whole, tail };
            vkCmdCopyBuffer(cmd, [edge vkBuffer], self->_vkbuf, 1, &copy);
        }
        rdna4_full_barrier(cmd);
    }, unresolved);
}
// 12 (no-copy): the enclosing aligned range of client memory is imported with VK_EXT_external_memory_host (the IOSurface texture path, 11h.6). The CPU pointer
// IS the client's (contents == p); GPU writes are visible to the CPU after the writing command buffer completed, CPU writes before commit are seen by the GPU
// (host-visible import: the memory type is host coherent). Private storage and import spans > 64 MiB return nil.
#include <mach/vm_region.h>
static uint32_t rdna4nc_mach_prot(vm_prot_t p) {
    uint32_t flags = 0;
    if (p & VM_PROT_READ) flags |= RDNA4NC_PROT_READ;
    if (p & VM_PROT_WRITE) flags |= RDNA4NC_PROT_WRITE;
    if (p & VM_PROT_EXECUTE) flags |= RDNA4NC_PROT_EXEC;
    return flags;
}
static int rdna4nc_host_readonly(const void *p, NSUInteger len) {
    mach_vm_address_t cursor = (mach_vm_address_t)(uintptr_t)p;
    if ((uint64_t)len > UINT64_MAX - cursor) return -1;
    const mach_vm_address_t end = cursor + (uint64_t)len;
    int readonly = 0;
    while (cursor < end) {
        mach_vm_address_t address = cursor; mach_vm_size_t size = 0;
        vm_region_basic_info_data_64_t info; mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        kern_return_t kr = mach_vm_region(mach_task_self(), &address, &size, VM_REGION_BASIC_INFO_64,
                                          (vm_region_info_t)&info, &count, &object);
        if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
        if (kr != KERN_SUCCESS || !size || address > cursor || cursor - address >= size) return -1;
        uint64_t part = MIN((uint64_t)size - (cursor - address), end - cursor);
        struct rdna4nc_region region = { address, size, rdna4nc_mach_prot(info.protection) };
        int verdict = rdna4nc_range_readonly(&region, 1, cursor, part);
        if (verdict < 0) return -1;
        readonly |= verdict;
        cursor += part;
    }
    return readonly;
}
static void rdna4nc_region_summary(void *p, NSUInteger len, char *out, size_t cap) {
    if (!cap) return;
    out[0] = '\0';
    mach_vm_address_t cursor = (mach_vm_address_t)(uintptr_t)p;
    if ((mach_vm_size_t)len > UINT64_MAX - cursor) { snprintf(out, cap, "range-overflow; any_page_not_writable=unknown"); return; }
    const mach_vm_address_t end = cursor + (mach_vm_size_t)len;
    size_t used = 0; unsigned nregions = 0; int anyNotWritable = 0, unknown = 0;
    while (cursor < end) {
        mach_vm_address_t address = cursor; mach_vm_size_t size = 0;
        vm_region_basic_info_data_64_t info; mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        kern_return_t kr = mach_vm_region(mach_task_self(), &address, &size, VM_REGION_BASIC_INFO_64,
                                          (vm_region_info_t)&info, &count, &object);
        if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
        if (kr != KERN_SUCCESS || !size || address >= end) { unknown = 1; break; }
        if (address > cursor) { unknown = 1; cursor = address; }
        mach_vm_address_t regionEnd = size > UINT64_MAX - address ? end : address + size;
        if (!(info.protection & VM_PROT_WRITE)) anyNotWritable = 1;
        if (nregions < 4 && used < cap) {
            char prot[4], maxProt[4];
            rdna4nc_protection_text(rdna4nc_mach_prot(info.protection), prot);
            rdna4nc_protection_text(rdna4nc_mach_prot(info.max_protection), maxProt);
            int wrote = snprintf(out + used, cap - used, "%s%s/%s-%s", nregions ? "," : "",
                                 prot, maxProt, info.shared ? "shared" : "private");
            if (wrote > 0) used += (size_t)wrote < cap - used ? (size_t)wrote : cap - used - 1;
        }
        nregions++;
        if (regionEnd <= cursor) { unknown = 1; break; }
        cursor = regionEnd;
    }
    if (!nregions) snprintf(out, cap, "unavailable");
    else if (nregions > 4 && used < cap) (void)snprintf(out + used, cap - used, ",...");
    size_t usedNow = strnlen(out, cap);
    if (usedNow < cap) (void)snprintf(out + usedNow, cap - usedNow, "; any_page_not_writable=%s",
                                      anyNotWritable ? "yes" : unknown ? "unknown" : "no");
}
- (instancetype)initWithDevice:(id)dev bytesNoCopy:(void *)p length:(NSUInteger)len options:(NSUInteger)opts deallocator:(void (^)(void *, NSUInteger))d error:(NSError **)err {
    self = [super init]; if (!self) return nil;
    _dev = dev; _len = len; _opts = opts;
    return [self rdna4ImportHostPointer:p length:len ownedSpan:0 deallocator:d error:err];
}
- (instancetype)rdna4ImportHostPointer:(void *)p length:(NSUInteger)len ownedSpan:(uint64_t)ownedSpan deallocator:(void (^)(void *, NSUInteger))d error:(NSError **)err {
    #define NCFAIL(code, ...) do { NSString *m_ = [NSString stringWithFormat:__VA_ARGS__]; RDNA4LOG_WARN("newBufferWithBytesNoCopy: refused: %s", m_.UTF8String); if (err) *err = rdna4_err(code, m_); return nil; } while (0)
    if (!p || !len) NCFAIL(110, @"NULL pointer or zero length");
    if (((_opts >> 4) & 0xF) > 1) NCFAIL(111, @"storage mode %lu: only Shared and Managed client memory can be wrapped", (unsigned long)((_opts >> 4) & 0xF));
    if (!rdna4_radv_open(err)) return nil;
    rdna4dc_caps caps = rdna4_device_caps();
    if (!RDNA4R.hostExt || !rdna4_vkGetMHPP) NCFAIL(112, @"VK_EXT_external_memory_host is not available on this RADV");
    uint64_t base, span, delta;
    if (rdna4nc_import_range((uint64_t)(uintptr_t)p, len, RDNA4R.hostAlign, &base, &span, &delta)) NCFAIL(113, @"client range overflows or host import alignment is invalid");
    if (!rdna4dc_buffer_ok(&caps, span)) NCFAIL(114, @"import span exceeds the Vulkan storage/allocation limit");
    if (!ownedSpan && span > 64u << 20) NCFAIL(114, @"import span %llu > 64 MiB (the NoCopy single-import limit)", (unsigned long long)span);
    if (ownedSpan && (base != (uint64_t)(uintptr_t)p || span > ownedSpan)) NCFAIL(113, @"owned CPU range does not cover the aligned import");
    void *hostBase = (void *)(uintptr_t)base;
    VkExternalMemoryBufferCreateInfo external = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT };
    VkBufferCreateInfo bc = rdna4_heap_buffer_create_info(span, &external);
    VkResult r = vkCreateBuffer(RDNA4R.dev, &bc, NULL, &_vkbuf);
    if (r != VK_SUCCESS) { _vkbuf = VK_NULL_HANDLE; NCFAIL(115, @"vkCreateBuffer = %d", r); }
    VkMemoryHostPointerPropertiesEXT hp = { .sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT };
    uint64_t timp_ = rdna4_now();
    r = rdna4_vkGetMHPP(RDNA4R.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, hostBase, &hp);
    if (r != VK_SUCCESS || !hp.memoryTypeBits) NCFAIL(116, @"vkGetMemoryHostPointerPropertiesEXT = %d bits 0x%x", r, hp.memoryTypeBits);
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(RDNA4R.dev, _vkbuf, &mr);
    if (mr.size > span) NCFAIL(117, @"Vulkan buffer memory requirement exceeds the aligned host span");
    int mt = rdna4_find_mem(hp.memoryTypeBits & mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt < 0) NCFAIL(117, @"no memory type common to the import (0x%x) and the buffer (0x%x)", hp.memoryTypeBits, mr.memoryTypeBits);
    VkImportMemoryHostPointerInfoEXT imp = { .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, .pHostPointer = hostBase };
    VkMemoryAllocateFlagsInfo flags = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, .pNext = &imp };
    VkMemoryAllocateInfo ma = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = RDNA4R.bufferAddress ? (void *)&flags : (void *)&imp,
        .allocationSize = span, .memoryTypeIndex = (uint32_t)mt };
    r = vkAllocateMemory(RDNA4R.dev, &ma, NULL, &_vkmem); rdna4t_add(&T1.imp, rdna4_now() - timp_);
    if (r != VK_SUCCESS) {
        _vkmem = VK_NULL_HANDLE;
        char pages[256]; rdna4nc_region_summary(hostBase, (NSUInteger)span, pages, sizeof pages);
        NSString *message = [NSString stringWithFormat:@"vkAllocateMemory(import %lu B at %p) = %d; host_alignment %llu; vk_alignment %llu; regions %s",
                             (unsigned long)span, hostBase, r, (unsigned long long)RDNA4R.hostAlign, (unsigned long long)mr.alignment, pages];
        static _Atomic uint64_t refusedLogCount;
        uint64_t logIndex = atomic_fetch_add(&refusedLogCount, 1);
        if (logIndex < 8 || (logIndex & 1023u) == 0) RDNA4LOG_WARN("newBufferWithBytesNoCopy: refused: %s", message.UTF8String);
        if (err) *err = rdna4_err(118, message);
        return nil;
    }
    _imported = YES; _impSpan = span;
    @synchronized ([GarnetDevice class]) { RDNA4R.impTotal += span; }
    if (ownedSpan) { _asz = (NSUInteger)span; atomic_fetch_add(&rdna4_alloc_total, (uint64_t)_asz); }
    r = vkBindBufferMemory(RDNA4R.dev, _vkbuf, _vkmem, 0);
    if (r != VK_SUCCESS) NCFAIL(119, @"vkBindBufferMemory = %d", r);
    if (RDNA4R.bufferAddress) {
        VkBufferDeviceAddressInfo address = { .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = _vkbuf };
        _gpuAddress = rdna4_vkGetBufferAddress(RDNA4R.dev, &address);
        if (!_gpuAddress) NCFAIL(120, @"Vulkan returned a zero GPU address for the imported buffer");
        if (delta > UINT64_MAX - _gpuAddress) NCFAIL(120, @"imported GPU address overflows");
        _gpuAddress += delta;
    }
    _vkoff = delta;
    _ptr = p; _deallocator = [d copy];
    [self rdna4CheckHostReadOnly:imp.pHostPointer length:(NSUInteger)ma.allocationSize];
    RDNA4LOGR("RDNA4Buffer %p: NO-COPY %lu bytes at %p imported as [%p, +%llu) offset %llu", (__bridge void *)self, (unsigned long)len, p, hostBase, (unsigned long long)span, (unsigned long long)delta);
    #undef NCFAIL
    return self;
}
- (BOOL)rdna4Imported { return _imported; }
- (BOOL)rdna4HostReadOnly { return _hostReadOnly; }
- (void)rdna4CheckHostReadOnly:(const void *)p length:(NSUInteger)len {
    // A mapping that cannot be inspected is conservatively treated as non-writable.
    int protection = rdna4nc_host_readonly(p, len);
    _hostReadOnly = protection != 0;
    if (_hostReadOnly) rdna4nc_note_readonly_buffer();
    if (protection < 0) RDNA4LOGR("RDNA4Buffer %p: NO-COPY protection unknown, treating as read-only", (__bridge void *)self);
    if (_hostReadOnly) RDNA4LOGR("RDNA4Buffer %p: NO-COPY %lu bytes at %p imported READ-ONLY (GPU writes will be dropped)",
                                (__bridge void *)self, (unsigned long)len, p);
}
- (VkDeviceMemory)rdna4NativeMemory:(uint64_t *)offset {
    if (_hpHeap) { *offset = _hpBindOff; return [_hpHeap rdna4BackingMemory:_hpBlk]; }
    *offset = _pooled ? _pm.off : 0;
    return _imported || _asz > RDNA4_BUFFER_LEGACY_MAX ? VK_NULL_HANDLE : (_pooled ? _pm.mem : _vkmem);
}
- (void)rdna4AdoptHeap:(RDNA4Heap *)h offset:(NSUInteger)o size:(NSUInteger)sz bid:(uint64_t)bid {
    _hpHeap = h; _hpOff = o; _hpSize = sz; _hpId = bid;
    if (h) _hpPhysSize = sz;
    if (!_noacct && (!_imported || _ownedHostSpan)) { atomic_fetch_sub(&rdna4_alloc_total, (uint64_t)_asz); _noacct = YES; }
}
- (void)dealloc {
    const BOOL heapBacked = _hpHeap && _hpBlk;
    RDNA4_HEAP_DEALLOC;
    if (RDNA4R.ok && !heapBacked) {
        if (_vkbuf) vkDestroyBuffer(RDNA4R.dev, _vkbuf, NULL);
        if (_pooled) { rdna4_mem_free(&_pm); if (!_noacct) atomic_fetch_sub(&rdna4_alloc_total, (uint64_t)_asz); }   // P1
        else if (_vkmem) {
            vkFreeMemory(RDNA4R.dev, _vkmem, NULL);
            if (_imported) { @synchronized ([GarnetDevice class]) { RDNA4R.impTotal -= _impSpan; } }
            if (!_noacct && (!_imported || _ownedHostSpan)) atomic_fetch_sub(&rdna4_alloc_total, (uint64_t)_asz);
        }
    }
    // Free the CPU VA after Vulkan retires the imports; quarantined descriptors retain their pinned physical pages.
    if (_ownedHostSpan) (void)munmap(_ptr, (size_t)_ownedHostSpan);
    if (_deallocator) { void (^d)(void *, NSUInteger) = _deallocator; _deallocator = nil; d(_ptr, _len); }
}
- (void)doesNotRecognizeSelector:(SEL)sel {
    RDNA4LOG_ERR("UNRECOGNIZED selector %s on RDNA4Buffer", sel_getName(sel));
    [super doesNotRecognizeSelector:sel];
}
- (VkBuffer)vkBuffer { return _vkbuf; }
- (VkDeviceSize)rdna4VkOffset { return _vkoff; }
- (RDNA4Heap *)rdna4ResourceHeap { return _hpHeap; }
- (NSUInteger)rdna4ResourceHeapOffset { return (_hpHeap && [_hpHeap type] == MTLHeapTypePlacement) ? _hpOff : 0; }
- (BOOL)rdna4ResourceAliasable { return _hpAl; }
- (void)rdna4ResourceMakeAliasable { RDNA4_HEAP_RETIRE; }
- (BOOL)rdna4ResourceAliases:(id)other {
    return _hpHeap && _hpBlk && [_hpHeap rdna4Block:_hpBlk offset:_hpBindOff size:_hpPhysSize aliases:other];
}
- (uint64_t)rdna4BackingBlock { return _hpBlk; }
- (uint64_t)rdna4BackingOffset { return _hpBindOff; }
- (uint64_t)rdna4BackingSize { return _hpPhysSize; }
- (void)rdna4AbortHeapConstruction {
    RDNA4Heap *heap = _hpHeap;
    uint64_t bid = _hpId, block = _hpBlk, lease = _hpLease;
    const BOOL bound = _hpBacking != nil;
    [self rdna4DestroyRetiredBacking];
    _hpBacking = nil;
    if (heap && block && bound) [heap rdna4CloseBackingHandle:block];
    if (heap && lease) [heap rdna4AbandonBacking:lease];
    if (heap && bid) [heap rdna4Free:bid];
    _hpHeap = nil;
    _hpId = _hpBlk = _hpLease = _hpBindOff = _hpPhysSize = 0;
    _hpDedicated = _hpAl = NO; _hpOff = _hpSize = 0; _noacct = NO;
}
- (void)rdna4DestroyRetiredBacking {
    if (_hpBacking) [_hpBacking destroyHandles];
    else if (RDNA4R.ok && _vkbuf) vkDestroyBuffer(RDNA4R.dev, _vkbuf, NULL);
    _vkbuf = VK_NULL_HANDLE; _ptr = NULL; _gpuAddress = 0;
}
- (void *)contents { return _ptr; }
- (NSUInteger)length { return _len; }
- (uint64_t)gpuAddress { return _gpuAddress; }
- (id)device { return _dev; }
- (NSString *)label { return _lbl; }
- (void)setLabel:(NSString *)l { _lbl = [l copy]; }
- (NSUInteger)resourceOptions { return _opts; }
- (MTLStorageMode)storageMode { return (MTLStorageMode)((_opts >> 4) & 0xF); }
- (MTLCPUCacheMode)cpuCacheMode { return (MTLCPUCacheMode)(_opts & 0xF); }
- (MTLHazardTrackingMode)hazardTrackingMode { return (MTLHazardTrackingMode)((_opts >> 8) & 0x3); }
- (void)didModifyRange:(NSRange)r { rdna4_argument_note_dirty_range(self, r); }   // Shared memory is coherent; notify incremental argscan cache if present
// ---- gap census: MTLResource / MTLBuffer surface sent by QuartzCore, SkyLight and Metal ----
RDNA4_RESOURCE_SPI
- (NSUInteger)allocatedSize { return _hpHeap ? _hpSize : (_asz ? _asz : _len); }
- (void *)virtualAddress { return _ptr; }   // MTLIOAccelResource -virtualAddress is ^v (CPU pointer); QuartzCore sends it next to -contents and -didModifyRange: (SUSPECTED use)
- (void)addDebugMarker:(NSString *)m range:(NSRange)r { (void)m; (void)r; }
- (void)removeAllDebugMarkers {}
- (IOSurfaceRef)iosurface { return NULL; }
@end


// ---------------------------------------------------------------------------------------------------------------
// bundle 9: garnet_texdesc.h carries the numeric Metal / Vulkan values (it includes neither header); these asserts tie them to the real enums.
_Static_assert((int)MTLTextureType1D == RDNA4TD_MTL_1D && (int)MTLTextureType1DArray == RDNA4TD_MTL_1DARRAY && (int)MTLTextureType2D == RDNA4TD_MTL_2D && (int)MTLTextureType2DArray == RDNA4TD_MTL_2DARRAY &&
               (int)MTLTextureType2DMultisample == RDNA4TD_MTL_2DMS && (int)MTLTextureTypeCube == RDNA4TD_MTL_CUBE && (int)MTLTextureTypeCubeArray == RDNA4TD_MTL_CUBEARRAY && (int)MTLTextureType3D == RDNA4TD_MTL_3D, "MTLTextureType values");
_Static_assert((int)MTLTextureUsageShaderWrite == RDNA4TD_USE_SHADERWRITE && (int)MTLTextureUsageRenderTarget == RDNA4TD_USE_RENDERTARGET, "MTLTextureUsage values");
_Static_assert((int)VK_IMAGE_TYPE_1D == RDNA4TD_VK_IMAGE_1D && (int)VK_IMAGE_TYPE_2D == RDNA4TD_VK_IMAGE_2D && (int)VK_IMAGE_TYPE_3D == RDNA4TD_VK_IMAGE_3D, "VkImageType values");
_Static_assert((int)VK_IMAGE_VIEW_TYPE_1D == RDNA4TD_VK_VIEW_1D && (int)VK_IMAGE_VIEW_TYPE_2D == RDNA4TD_VK_VIEW_2D && (int)VK_IMAGE_VIEW_TYPE_3D == RDNA4TD_VK_VIEW_3D && (int)VK_IMAGE_VIEW_TYPE_CUBE == RDNA4TD_VK_VIEW_CUBE &&
               (int)VK_IMAGE_VIEW_TYPE_1D_ARRAY == RDNA4TD_VK_VIEW_1D_ARRAY && (int)VK_IMAGE_VIEW_TYPE_2D_ARRAY == RDNA4TD_VK_VIEW_2D_ARRAY &&
               (int)VK_IMAGE_VIEW_TYPE_CUBE_ARRAY == RDNA4TD_VK_VIEW_CUBE_ARRAY, "VkImageViewType values");
_Static_assert((unsigned)VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT == RDNA4TD_VK_CREATE_CUBE_COMPATIBLE, "VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT");
_Static_assert((int)MTLTextureTypeTextureBuffer == RDNA4TD_MTL_BUFFER, "MTLTextureTypeTextureBuffer value");
// RDNA4Texture: VkImage (optimal, DEVICE_LOCAL), always TRANSFER_SRC|DST (10c).
// ---------------------------------------------------------------------------------------------------------------

@implementation RDNA4Buffer (Textures)
// Linear textures over this buffer (public -newTextureWithDescriptor:offset:bytesPerRow:, SPI -newLinearTexture..., -newTiledTexture...): see RDNA4Texture initWithDevice:descriptor:buffer:
- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)off bytesPerRow:(NSUInteger)bpr {
    NSError *e = nil; id t = [[RDNA4Texture alloc] initWithDevice:_dev descriptor:d buffer:self offset:off bytesPerRow:bpr error:&e];
    if (!t) RDNA4LOG_WARN("buffer newTextureWithDescriptor:offset:bytesPerRow: NIL: %s; pf %lu type %lu usage 0x%lx storage %lu %lux%lu", e.localizedDescription.UTF8String,
                          (unsigned long)d.pixelFormat, (unsigned long)d.textureType, (unsigned long)d.usage, (unsigned long)d.storageMode, (unsigned long)d.width, (unsigned long)d.height);
    if (t) [[self rdna4ResourceHeap] rdna4TrackResource:t];
    return t;
}
- (id)newLinearTextureWithDescriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)off bytesPerRow:(NSUInteger)bpr bytesPerImage:(NSUInteger)bpi { (void)bpi; return [self newTextureWithDescriptor:d offset:off bytesPerRow:bpr]; }
- (id)newTiledTextureWithDescriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)off bytesPerRow:(NSUInteger)bpr { return [self newTextureWithDescriptor:d offset:off bytesPerRow:bpr]; }
@end

static void rdna4_layout_barrier_masks(VkImageLayout layout, BOOL isSrc,
                                       VkPipelineStageFlags *stage, VkAccessFlags *access) {
    switch (layout) {
    case VK_IMAGE_LAYOUT_UNDEFINED:
        *stage = isSrc ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        *access = isSrc ? 0 : (VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        return;
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        *stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        *access = isSrc ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                        : (VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        return;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        *stage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        *access = isSrc ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                        : (VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        return;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        *stage = VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        *access = isSrc ? 0 : VK_ACCESS_SHADER_READ_BIT;
        return;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        *stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        *access = isSrc ? 0 : VK_ACCESS_TRANSFER_READ_BIT;
        return;
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        *stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        *access = VK_ACCESS_TRANSFER_WRITE_BIT;
        return;
    default:
        *stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        *access = isSrc ? VK_ACCESS_MEMORY_WRITE_BIT : (VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        return;
    }
}

GARNET_INTERNAL void rdna4_image_transition(VkCommandBuffer cmd, RDNA4Texture *t, VkImageLayout oldLayout, VkImageLayout newLayout) {
    if (oldLayout == newLayout) return;
    VkPipelineStageFlags srcStage = 0, dstStage = 0;
    VkAccessFlags srcAccess = 0, dstAccess = 0;
    rdna4_layout_barrier_masks(oldLayout, YES, &srcStage, &srcAccess);
    rdna4_layout_barrier_masks(newLayout, NO, &dstStage, &dstAccess);
    VkImageMemoryBarrier ib = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = srcAccess, .dstAccessMask = dstAccess,
        .oldLayout = oldLayout, .newLayout = newLayout, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = [t vkImage], .subresourceRange = { rdna4dp_barrier_aspect([t rdna4Aspects]), 0, [t rdna4Levels], 0, [t rdna4ImageLayers] } };   // bundle 10: a depth/stencil image's barrier names its own aspects
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, NULL, 0, NULL, 1, &ib);
    [t setLayout:newLayout];
}
GARNET_INTERNAL VkImageLayout rdna4_use_layout(RDNA4Texture *texture, VkImageLayout preferred) {
    return [texture rdna4IOSLinear] ? VK_IMAGE_LAYOUT_GENERAL : preferred;
}
// Layout transition recorded into cmd; the texture object tracks the layout at RECORD time (command buffers execute in order).
GARNET_INTERNAL void rdna4_tex_to(VkCommandBuffer cmd, RDNA4Texture *t, VkImageLayout nl) {
    if ([t rdna4IsTexel]) {
        RDNA4Buffer *buffer = [t rdna4TexelBuffer];
        VkBufferMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = [buffer vkBuffer], .offset = [buffer rdna4VkOffset] + [t bufferOffset],
            .size = [t width] * [t bytesPerPixel]};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            0, 0, NULL, 1, &barrier, 0, NULL);
        [t setLayout:nl];
        return;
    }
    if (![t rdna4IOSLinear]) rdna4_image_transition(cmd, t, [t layout], nl);
}
GARNET_INTERNAL void rdna4_tex_to_compute(VkCommandBuffer cmd, RDNA4Texture *t, VkImageLayout nl) {
    if ([t rdna4IsTexel]) { rdna4_tex_to(cmd, t, nl); return; }
    if ([t rdna4IOSLinear]) return;
    const VkImageLayout oldLayout = [t layout];
    if (oldLayout == nl) return;
    const VkPipelineStageFlags srcStage = (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED)
        ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    const VkAccessFlags srcAccess = (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED)
        ? 0 : (VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    const VkPipelineStageFlags dstStage = (nl == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    const VkAccessFlags dstAccess = (nl == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        ? VK_ACCESS_SHADER_READ_BIT : (VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    VkImageMemoryBarrier ib = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = srcAccess, .dstAccessMask = dstAccess,
        .oldLayout = oldLayout, .newLayout = nl,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = [t vkImage], .subresourceRange = { rdna4dp_barrier_aspect([t rdna4Aspects]), 0, [t rdna4Levels], 0, [t rdna4ImageLayers] } };
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, NULL, 0, NULL, 1, &ib);
    [t setLayout:nl];
}
GARNET_INTERNAL BOOL rdna4_region_ok(RDNA4Texture *t, MTLRegion r, NSUInteger level, NSUInteger slice, const char *what) {
    if (level >= [t mipmapLevelCount] || slice >= [t rdna4Layers]) { RDNA4LOG_WARN("%s: mip level %lu / slice %lu: the texture has %lu level(s) and %u slice(s); refused", what, (unsigned long)level, (unsigned long)slice, (unsigned long)[t mipmapLevelCount], [t rdna4Layers]); return NO; }
    NSUInteger lw = MAX((NSUInteger)1, t.width >> level), lh = MAX((NSUInteger)1, t.height >> level), ld = MAX((NSUInteger)1, t.depth >> level);   // depth is 1 for everything but 3D
    if (!r.size.width || !r.size.height || !r.size.depth || r.origin.x > lw || r.size.width > lw - r.origin.x ||
        r.origin.y > lh || r.size.height > lh - r.origin.y || r.origin.z > ld || r.size.depth > ld - r.origin.z) {
        RDNA4LOG_WARN("%s: region origin %lu,%lu,%lu size %lux%lux%lu outside the %lux%lux%lu level %lu; refused", what, (unsigned long)r.origin.x, (unsigned long)r.origin.y,
                      (unsigned long)r.origin.z, (unsigned long)r.size.width, (unsigned long)r.size.height, (unsigned long)r.size.depth, (unsigned long)lw, (unsigned long)lh, (unsigned long)ld, (unsigned long)level); return NO; }
    if ([t isCompressed] &&
        ((r.origin.x & 3) || (r.origin.y & 3) ||
         ((r.size.width & 3) && r.origin.x + r.size.width != lw) ||
         ((r.size.height & 3) && r.origin.y + r.size.height != lh))) {
        RDNA4LOG_WARN("%s: block-compressed region origin/size (%lu,%lu %lux%lu) is not 4x4 block aligned on %lux%lu level; refused", what, (unsigned long)r.origin.x, (unsigned long)r.origin.y, (unsigned long)r.size.width, (unsigned long)r.size.height, (unsigned long)lw, (unsigned long)lh);
        return NO;
    }
    return YES;
}


// Constructors and SizeAndAlign queries share the same Vulkan image description.
GARNET_INTERNAL BOOL rdna4_heap_image_plan_make(MTLTextureDescriptor *d, BOOL aliasable, rdna4_heap_image_plan *p, NSError **err) {
    if (!d || !p) return NO;
    memset(p, 0, sizeof *p);
    if (!d.width || !d.height || d.width > UINT32_MAX || d.height > UINT32_MAX ||
        d.depth > UINT32_MAX || d.mipmapLevelCount > UINT32_MAX ||
        d.arrayLength > UINT32_MAX || d.sampleCount > UINT32_MAX) {
        if (err) *err = rdna4_err(31, @"texture geometry is zero or exceeds Vulkan's 32-bit fields");
        return NO;
    }
    p->fmt = rdna4_fmt(d.pixelFormat);
    if (!p->fmt && rdna4dp_is_dsformat((unsigned long)d.pixelFormat)) {
        rdna4dp_fmt_t df;
        if (!rdna4dp_fmt((unsigned long)d.pixelFormat, &df)) {
            if (err) *err = rdna4_err(30, [NSString stringWithFormat:@"pixel format %lu: %s", (unsigned long)d.pixelFormat, df.why]);
            return NO;
        }
        p->fmt = rdna4_fmt_ds(d.pixelFormat);
        p->isDS = p->fmt != NULL;
        p->aspects = df.aspects;
    }
    if (!p->fmt) {
        if (err) *err = rdna4_err(30, [NSString stringWithFormat:@"pixel format %lu not supported", (unsigned long)d.pixelFormat]);
        return NO;
    }
    if (!rdna4td_map((unsigned long)d.textureType, d.width, d.height, d.depth, d.arrayLength, d.sampleCount, d.mipmapLevelCount, (unsigned long)d.usage, &p->td)) {
        if (err) *err = rdna4_err(31, [NSString stringWithFormat:@"%s (type %lu %lux%lu depth %lu mips %lu array %lu samples %lu usage 0x%lx)", p->td.why,
            (unsigned long)d.textureType, (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.depth, (unsigned long)d.mipmapLevelCount,
            (unsigned long)d.arrayLength, (unsigned long)d.sampleCount, (unsigned long)d.usage]);
        return NO;
    }
    p->is1D = p->td.is1D;
    p->is3D = p->td.is3D;
    p->arr1D = d.textureType == MTLTextureType1DArray;
    p->levels = p->td.levels;
    p->depth = p->td.depth;
    p->samples = p->td.samples > 1 ? p->td.samples : 0;
    NSUInteger md = MAX(MAX(d.width, d.height), p->is3D ? p->depth : (NSUInteger)1), maxLevels = 1;
    while (md > 1) { md >>= 1; maxLevels++; }
    if (p->levels > maxLevels) {
        if (err) *err = rdna4_err(42, [NSString stringWithFormat:@"%lu mip levels for a %lux%lux%lu texture (at most %lu)",
            (unsigned long)p->levels, (unsigned long)d.width, (unsigned long)d.height, (unsigned long)p->depth, (unsigned long)maxLevels]);
        return NO;
    }
    if ((p->fmt->flags & RDNA4F_A8) && (d.usage & MTLTextureUsageRenderTarget)) {
        if (err) *err = rdna4_err(44, @"A8Unorm render targets are not supported (an R8 attachment cannot take the fragment's alpha)");
        return NO;
    }
    {
        const char *swzWhy = NULL;
        if (!rdna4swz_check(rdna4_swz_from_mtl(d.swizzle), (unsigned long)d.usage, 0, 0, &swzWhy)) {
            if (err) *err = rdna4_err(44, [NSString stringWithFormat:@"%s", swzWhy]);
            return NO;
        }
    }
    if (rdna4_fmt_is_bc(p->fmt)) {
        if (p->is1D || p->samples > 1 || (d.usage & (MTLTextureUsageRenderTarget | MTLTextureUsageShaderWrite))) {
            if (err) *err = rdna4_err(44, @"block-compressed textures do not support 1D, multisample, render-target, or ShaderWrite usage");
            return NO;
        }
    }
    if (p->is3D && (d.usage & MTLTextureUsageRenderTarget)) {
        if (err) *err = rdna4_err(45, @"3D render targets are not supported");
        return NO;
    }
    if (!rdna4_radv_open(err)) return NO;
    if (rdna4_fmt_is_bc(p->fmt) && !RDNA4R.bcCompression) {
        if (err) *err = rdna4_err(30, @"RADV does not support BC texture compression");
        return NO;
    }
    if (d.textureType == MTLTextureTypeCubeArray && !RDNA4R.cubeArray) {
        if (err) *err = rdna4_err(31, @"RADV does not support cube-array image views");
        return NO;
    }
    if (p->samples > 1 && d.storageMode != MTLStorageModePrivate) {
        if (err) *err = rdna4_err(31, [NSString stringWithFormat:@"multisample textures are Private storage only (storage mode %lu)", (unsigned long)d.storageMode]);
        return NO;
    }
    if (p->is1D && (d.height != 1 || d.width > RDNA4R.lim.maxImageDimension1D || p->td.layersIvar > RDNA4R.lim.maxImageArrayLayers)) {
        if (err) *err = rdna4_err(37, [NSString stringWithFormat:@"1D texture %lux%lu array %lu outside RADV limits (width <= %u, layers <= %u, height 1)",
            (unsigned long)d.width, (unsigned long)d.height, (unsigned long)p->td.layersIvar, RDNA4R.lim.maxImageDimension1D, RDNA4R.lim.maxImageArrayLayers]);
        return NO;
    }
    if (p->is3D && (d.width > RDNA4R.lim.maxImageDimension3D || d.height > RDNA4R.lim.maxImageDimension3D || p->depth > RDNA4R.lim.maxImageDimension3D)) {
        if (err) *err = rdna4_err(46, [NSString stringWithFormat:@"3D texture %lux%lux%lu outside the RADV limit %u",
            (unsigned long)d.width, (unsigned long)d.height, (unsigned long)p->depth, RDNA4R.lim.maxImageDimension3D]);
        return NO;
    }
    VkFormatFeatureFlags ff = rdna4_fmt_feats(p->fmt);
    const VkFormatFeatureFlags need = p->isDS ? 0 : (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
    if (p->isDS) {
        const char *why = NULL;
        if (!rdna4dp_check((unsigned long)d.textureType, (unsigned long)d.usage, (unsigned long)d.storageMode, (unsigned)ff, &why)) {
            if (err) *err = rdna4_err(48, [NSString stringWithFormat:@"%s (pixel format %lu vk %d features 0x%x type %lu storage %lu usage 0x%lx)",
                why, (unsigned long)d.pixelFormat, p->fmt->vk, ff, (unsigned long)d.textureType, (unsigned long)d.storageMode, (unsigned long)d.usage]);
            return NO;
        }
    }
    if ((ff & need) != need) {
        if (err) *err = rdna4_err(38, [NSString stringWithFormat:@"pixel format %lu (vk %d): RADV features 0x%x lack sampled/transfer",
            (unsigned long)d.pixelFormat, p->fmt->vk, ff]);
        return NO;
    }
    p->canRT = p->isDS ? ((ff & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0)
                       : (!p->is1D && !p->is3D && !rdna4_fmt_is_bc(p->fmt) && !(p->fmt->flags & RDNA4F_A8) && (ff & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT));
    if ((d.usage & MTLTextureUsageRenderTarget) && !p->canRT) {
        if (err) *err = rdna4_err(39, [NSString stringWithFormat:@"pixel format %lu (vk %d) type %lu cannot be a render target on RADV (features 0x%x)",
            (unsigned long)d.pixelFormat, p->fmt->vk, (unsigned long)d.textureType, ff]);
        return NO;
    }
    if (!rdna4swz_is_identity(rdna4_swz_from_mtl(d.swizzle))) p->canRT = NO;
    // Vulkan forbids VK_IMAGE_USAGE_STORAGE_BIT on multisample images without shaderStorageImageMultisample;
    // MSAA textures with RenderTarget/ShaderRead (e.g., ANGLE WebGL) omit STORAGE_BIT while staying renderable/sampled.
    p->wantSt = (d.usage & MTLTextureUsageShaderWrite) != 0 && p->td.samples <= 1;
    if (p->wantSt && !(ff & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) {
        if (err) *err = rdna4_err(40, [NSString stringWithFormat:@"pixel format %lu (vk %d) has no storage-image support on RADV (features 0x%x)",
            (unsigned long)d.pixelFormat, p->fmt->vk, ff]);
        return NO;
    }
    VkImageUsageFlags iu = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
        (p->canRT ? (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT) : 0) |
        (p->wantSt ? VK_IMAGE_USAGE_STORAGE_BIT : 0);
    if (p->isDS) iu = ((ff & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0) |
        ((ff & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : 0) |
        ((ff & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ? VK_IMAGE_USAGE_SAMPLED_BIT : 0) |
        (p->canRT ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : 0);
    VkImageCreateFlags icf = ((d.usage & MTLTextureUsagePixelFormatView) ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0) | p->td.flags |
        (aliasable ? VK_IMAGE_CREATE_ALIAS_BIT : 0) |
        ((RDNA4R.sampleLocationsExt && p->isDS && p->td.samples > 1) ? VK_IMAGE_CREATE_SAMPLE_LOCATIONS_COMPATIBLE_DEPTH_BIT_EXT : 0);
    VkImageType vit = (VkImageType)p->td.imageType;
    VkImageFormatProperties ifp = {0};
    VkResult qr = vkGetPhysicalDeviceImageFormatProperties ? vkGetPhysicalDeviceImageFormatProperties(RDNA4R.pd, p->fmt->vk, vit, VK_IMAGE_TILING_OPTIMAL, iu, icf, &ifp) : VK_SUCCESS;
    if (qr != VK_SUCCESS && p->canRT && !(d.usage & MTLTextureUsageRenderTarget)) {
        iu &= ~(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
        qr = vkGetPhysicalDeviceImageFormatProperties(RDNA4R.pd, p->fmt->vk, vit, VK_IMAGE_TILING_OPTIMAL, iu, icf, &ifp);
    }
    if (qr != VK_SUCCESS) {
        if (err) *err = rdna4_err(41, [NSString stringWithFormat:@"RADV refuses image type %s format %d usage 0x%x (vkGetPhysicalDeviceImageFormatProperties = %d)",
            p->is1D ? "1D" : p->is3D ? "3D" : "2D", p->fmt->vk, iu, qr]);
        return NO;
    }
    if (p->td.samples > 1) {
        unsigned fc = vkGetPhysicalDeviceImageFormatProperties ? (unsigned)ifp.sampleCounts : 0u;
        if (!rdna4ms_texture_ok(p->td.samples, fc, p->isDS ? rdna4_ms_mask() : (unsigned)(RDNA4R.lim.framebufferColorSampleCounts))) {
            if (err) *err = rdna4_err(49, [NSString stringWithFormat:@"%u samples are not supported for pixel format %lu (vk %d): the format offers 0x%x, the device's framebuffer limits 0x%x",
                p->td.samples, (unsigned long)d.pixelFormat, p->fmt->vk, fc, p->isDS ? rdna4_ms_mask() : (unsigned)RDNA4R.lim.framebufferColorSampleCounts]);
            return NO;
        }
    }
    if (vkGetPhysicalDeviceImageFormatProperties && p->td.layers > ifp.maxArrayLayers) {
        if (err) *err = rdna4_err(47, [NSString stringWithFormat:@"%u array layers (%s) > RADV's %u for this format",
            p->td.layers, p->td.kind == RDNA4TD_K_CUBE ? "cube: 6 faces" : "array", ifp.maxArrayLayers]);
        return NO;
    }
    if (vkGetPhysicalDeviceImageFormatProperties && p->levels > ifp.maxMipLevels) {
        if (err) *err = rdna4_err(42, [NSString stringWithFormat:@"%lu mip levels > RADV's %u for this format", (unsigned long)p->levels, ifp.maxMipLevels]);
        return NO;
    }
    if (vkGetPhysicalDeviceImageFormatProperties &&
        (d.width > ifp.maxExtent.width || d.height > ifp.maxExtent.height ||
         (p->is3D && p->depth > ifp.maxExtent.depth))) {
        if (err) *err = rdna4_err(37, @"texture geometry exceeds the format's Vulkan image extent");
        return NO;
    }
    p->create = (VkImageCreateInfo){ .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = vit, .format = p->fmt->vk,
        .extent = { (uint32_t)d.width, (uint32_t)d.height, p->is3D ? (uint32_t)p->depth : 1 },
        .mipLevels = (uint32_t)p->levels, .arrayLayers = p->td.layers, .samples = (VkSampleCountFlagBits)p->td.samples,
        .flags = icf, .tiling = VK_IMAGE_TILING_OPTIMAL, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .usage = iu };
    if (garnet_async_compute_use_concurrent_image(RDNA4R.asyncComputeOn ? 1 : 0, RDNA4R.qfi, RDNA4R.computeQfi, p->isDS ? 1 : 0)) {
        p->create.sharingMode = VK_SHARING_MODE_CONCURRENT;
        p->create.queueFamilyIndexCount = 2;
        p->create.pQueueFamilyIndices = RDNA4R.sharedQfis;
    }
    return YES;
}

@implementation RDNA4Texture
// Texture buffers share their VkBuffer storage directly; they never enter the image-copy path.
- (instancetype)initTextureBufferWithDevice:(id)dev descriptor:(MTLTextureDescriptor *)d
    buffer:(RDNA4Buffer *)buffer offset:(NSUInteger)off error:(NSError **)err {
    self = [super init]; if (!self) return nil;
    #define TBFAIL(...) do { if (err) *err = rdna4_err(131, [NSString stringWithFormat:__VA_ARGS__]); return nil; } while (0)
    const RDNA4Fmt *format = rdna4_fmt(d.pixelFormat);
    NSUInteger bytes;
    if (!d || d.textureType != MTLTextureTypeTextureBuffer || !d.width || d.height != 1 ||
        d.depth != 1 || d.arrayLength != 1 || d.sampleCount != 1 || d.mipmapLevelCount != 1 ||
        (d.usage & ~(MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite)) ||
        !format || (format->flags & (RDNA4F_A8 | RDNA4F_BC)) ||
        __builtin_mul_overflow(d.width, (NSUInteger)format->bpp, &bytes))
        TBFAIL(@"invalid texture-buffer geometry, format, or usage");
    {
        const char *swzWhy = NULL;
        if (!rdna4swz_check(rdna4_swz_from_mtl(d.swizzle), (unsigned long)d.usage, RDNA4SWZ_FMT_TEXEL, 0, &swzWhy))
            TBFAIL(@"%s", swzWhy);
    }
    if (buffer && (![buffer isKindOfClass:[RDNA4Buffer class]] || [buffer device] != dev ||
        [buffer isAliasable] || !rdna4_buffer_range(buffer.length, off, bytes)))
        TBFAIL(@"texture-buffer backing is foreign, aliasable, or too small");
    if (buffer && [buffer rdna4HostReadOnly] && (d.usage & MTLTextureUsageShaderWrite))
        TBFAIL(@"texture-buffer write usage requires writable backing");
    if (!rdna4_radv_open(err)) return nil;
    if (!RDNA4R.robustTexelBuffers) TBFAIL(@"texture buffers require robustBufferAccess2 for defined out-of-bounds access");
    VkFormatProperties features = {0};
    vkGetPhysicalDeviceFormatProperties(RDNA4R.pd, format->vk, &features);
    VkFormatFeatureFlags required = VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT |
        ((d.usage & MTLTextureUsageShaderWrite) ? VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT : 0);
    uint64_t absolute;
    if ((features.bufferFeatures & required) != required || !vkCreateBufferView || !vkDestroyBufferView ||
        !RDNA4R.lim.maxTexelBufferElements || d.width > RDNA4R.lim.maxTexelBufferElements ||
        __builtin_add_overflow((uint64_t)[buffer rdna4VkOffset], (uint64_t)off, &absolute) ||
        absolute > UINT64_MAX - bytes ||
        off % 256 || absolute % format->bpp ||
        (RDNA4R.lim.minTexelBufferOffsetAlignment && absolute % RDNA4R.lim.minTexelBufferOffsetAlignment))
        TBFAIL(@"texture-buffer format, size, or offset exceeds Vulkan requirements");
    if (!buffer) {
        NSUInteger options = ((NSUInteger)d.storageMode << 4) | d.cpuCacheMode |
            ((NSUInteger)d.hazardTrackingMode << 8);
        buffer = [[RDNA4Buffer alloc] initWithDevice:dev length:bytes options:options error:err];
        if (!buffer) return nil;
        absolute = [buffer rdna4VkOffset]; _texelOwnedBuffer = YES;
        if (absolute > UINT64_MAX - bytes || absolute % format->bpp ||
            (RDNA4R.lim.minTexelBufferOffsetAlignment && absolute % RDNA4R.lim.minTexelBufferOffsetAlignment))
            TBFAIL(@"allocated texture-buffer backing has an invalid Vulkan offset");
    }
    VkBufferViewCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
        .buffer = [buffer vkBuffer], .format = format->vk, .offset = absolute, .range = bytes};
    VkResult result = vkCreateBufferView(RDNA4R.dev, &info, NULL, &_texelView);
    if (result != VK_SUCCESS) { _texelView = VK_NULL_HANDLE; TBFAIL(@"vkCreateBufferView = %d", result); }
    _texel = YES; _dev = dev; _hostBuf = buffer; _hoff = off;
    _texelStorage = (features.bufferFeatures & VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT) != 0 &&
        (!d.usage || (d.usage & MTLTextureUsageShaderWrite));
    _w = d.width; _h = _depth = _levels = 1; _pf = d.pixelFormat; _fmt = format;
    _usage = d.usage; _opts = [buffer resourceOptions]; _size = [buffer allocatedSize];
    _swz = (MTLTextureSwizzleChannels){MTLTextureSwizzleRed, MTLTextureSwizzleGreen,
        MTLTextureSwizzleBlue, MTLTextureSwizzleAlpha};
    #undef TBFAIL
    return self;
}
- (BOOL)rdna4IsTexel { return _texel; }
- (VkBufferView)rdna4TexelView { return _texelView; }
- (RDNA4Buffer *)rdna4TexelBuffer { return _texel ? _hostBuf : nil; }
- (BOOL)rdna4TexelWritable { return _texelStorage && ![self rdna4HostReadOnly]; }

- (instancetype)initWithDevice:(id)dev descriptor:(MTLTextureDescriptor *)d error:(NSError **)err {
    if (d.textureType == MTLTextureTypeTextureBuffer)
        return [self initTextureBufferWithDevice:dev descriptor:d buffer:nil offset:0 error:err];
    return [self initWithDevice:dev descriptor:d heap:nil error:err];
}
- (instancetype)initWithDevice:(id)dev descriptor:(MTLTextureDescriptor *)d heap:(RDNA4Heap *)heap error:(NSError **)err {
    return [self initWithDevice:dev descriptor:d heap:heap offset:0 hasOffset:NO error:err];
}
- (instancetype)initWithDevice:(id)dev descriptor:(MTLTextureDescriptor *)d heap:(RDNA4Heap *)heap offset:(NSUInteger)offset error:(NSError **)err {
    return [self initWithDevice:dev descriptor:d heap:heap offset:offset hasOffset:YES error:err];
}
- (instancetype)initWithDevice:(id)dev descriptor:(MTLTextureDescriptor *)d heap:(RDNA4Heap *)heap offset:(NSUInteger)offset hasOffset:(BOOL)hasOffset error:(NSError **)err {
    self = [super init];
    if (!self) return nil;
    _dev = dev; _w = d.width; _h = d.height; _pf = d.pixelFormat; _usage = d.usage; _layout = VK_IMAGE_LAYOUT_UNDEFINED;
    _hpHeap = heap;
    _swz = d.swizzle;
    _opts = ((NSUInteger)d.storageMode << 4) | ((NSUInteger)d.cpuCacheMode) | ((NSUInteger)d.hazardTrackingMode << 8);
    rdna4_heap_image_plan plan;
    if (!rdna4_heap_image_plan_make(d, heap != nil, &plan, err)) return nil;
    _fmt = plan.fmt;
    BOOL isDS = plan.isDS, is1D = plan.is1D, is3D = plan.is3D;
    rdna4td_t ti = plan.td;
    NSUInteger levels = plan.levels, depth = plan.depth;
    _is1D = is1D; _arr1D = plan.arr1D; _layers = ti.layersIvar; _tdk = ti.kind; _is3D = is3D; _levels = levels; _depth = depth; _samples = plan.samples; _aspects = plan.aspects;
    VkImageCreateInfo ic = plan.create;
    _imageFlags = ic.flags;
    VkResult r = vkCreateImage(RDNA4R.dev, &ic, NULL, &_img);
    if (r != VK_SUCCESS) { if (err) *err = rdna4_err(32, [NSString stringWithFormat:@"vkCreateImage = %d", r]); return nil; }
    VkMemoryRequirements mr; BOOL prefersDedicated = NO, requiresDedicated = NO;
    if (!rdna4_requirements2(VK_NULL_HANDLE, _img, &mr, &prefersDedicated, &requiresDedicated, heap != nil)) {
        if (err) *err = rdna4_err(22, @"heap textures require vkGetImageMemoryRequirements2 and dedicated requirements");
        [self rdna4AbortHeapConstruction];
        return nil;
    }
    BOOL dedicated = prefersDedicated || requiresDedicated;
    int mt = -1;
    if (heap) {
        VkDeviceMemory hmem = VK_NULL_HANDLE; uint64_t bindOff = 0, rangeSize = 0; uint8_t *map = NULL;
        if (hasOffset) {
            _hpOff = offset;
            _hpSize = (NSUInteger)mr.size;
            if (![heap rdna4AcquirePlacementBackingForBuffer:VK_NULL_HANDLE image:_img requirements:mr offset:offset
                                                  hostImport:NO requiresDedicated:requiresDedicated
                                                         mem:&hmem offset:&bindOff map:&map block:&_hpBlk
                                                       lease:&_hpLease rangeSize:&rangeSize error:err]) {
                [self rdna4AbortHeapConstruction];
                return nil;
            }
            dedicated = NO;
        } else {
            if (![heap rdna4ReserveRequirements:mr offset:&_hpOff id:&_hpId]) {
                if (err) *err = rdna4_err(22, [NSString stringWithFormat:@"heap has no logical range for a %llu-byte image requirement", (unsigned long long)mr.size]);
                [self rdna4AbortHeapConstruction];
                return nil;
            }
            _hpSize = (NSUInteger)mr.size;
            BOOL acquired = [heap rdna4AcquireBackingForBuffer:VK_NULL_HANDLE image:_img requirements:mr dedicated:dedicated hostImport:NO
                                                           mem:&hmem offset:&bindOff map:&map block:&_hpBlk lease:&_hpLease rangeSize:&rangeSize error:err];
            if (!acquired && dedicated && !requiresDedicated) {
                if (err) *err = nil;
                dedicated = NO;
                acquired = [heap rdna4AcquireBackingForBuffer:VK_NULL_HANDLE image:_img requirements:mr dedicated:NO hostImport:NO
                                                          mem:&hmem offset:&bindOff map:&map block:&_hpBlk lease:&_hpLease rangeSize:&rangeSize error:err];
            }
            if (!acquired) {
                [self rdna4AbortHeapConstruction];
                return nil;
            }
        }
        (void)map;
        _hpBindOff = bindOff; _hpPhysSize = rangeSize; _hpDedicated = dedicated;
        _size = (NSUInteger)mr.size; _acct = 0;
        r = vkBindImageMemory(RDNA4R.dev, _img, hmem, bindOff);
        if (r != VK_SUCCESS) {
            if (err) *err = rdna4_err(35, [NSString stringWithFormat:@"vkBindImageMemory = %d (heap block, offset %llu)", r, (unsigned long long)bindOff]);
            [self rdna4AbortHeapConstruction];
            return nil;
        }
    } else {
        mt = rdna4_find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mt < 0) { if (err) *err = rdna4_err(33, @"no device-local memory type"); return nil; }
        VkMemoryAllocateInfo ma = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)mt };
        if (RDNA4P.on) {   // P1
            r = rdna4_mem_alloc((uint32_t)mt, mr, NO, NO, VK_NULL_HANDLE, _img, &_pm);
            if (r != VK_SUCCESS) { if (err) *err = rdna4_err(34, [NSString stringWithFormat:@"vkAllocateMemory = %d (pool)", r]); return nil; }
            _pooled = YES; _size = (NSUInteger)mr.size; _acct = mr.size; atomic_fetch_add(&rdna4_alloc_total, _acct);
            r = vkBindImageMemory(RDNA4R.dev, _img, _pm.mem, _pm.off);
            if (r != VK_SUCCESS) { if (err) *err = rdna4_err(35, [NSString stringWithFormat:@"vkBindImageMemory = %d (pool, offset %llu)", r, (unsigned long long)_pm.off]); return nil; }
        } else {
        r = vkAllocateMemory(RDNA4R.dev, &ma, NULL, &_mem);
        if (r != VK_SUCCESS) { if (err) *err = rdna4_err(34, [NSString stringWithFormat:@"vkAllocateMemory = %d", r]); return nil; }
        _size = (NSUInteger)mr.size; _acct = mr.size; atomic_fetch_add(&rdna4_alloc_total, _acct);
        r = vkBindImageMemory(RDNA4R.dev, _img, _mem, 0);
        if (r != VK_SUCCESS) { if (err) *err = rdna4_err(35, [NSString stringWithFormat:@"vkBindImageMemory = %d", r]); return nil; }
        }
    }
    VkComponentMapping cm = rdna4_view_map(_fmt, _swz); _viewSwz = !rdna4_is_identity_map(&cm);
    VkImageViewCreateInfo vc = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = _img,
        .viewType = (VkImageViewType)ti.viewType,
        .format = _fmt->vk, .components = cm, .subresourceRange = { isDS ? rdna4dp_view_aspect(_aspects) : VK_IMAGE_ASPECT_COLOR_BIT, 0, (uint32_t)_levels, 0, ti.layers } };   // bundle 10: a depth/stencil sampling view has ONE aspect
    r = vkCreateImageView(RDNA4R.dev, &vc, NULL, &_view);
    if (r != VK_SUCCESS) {
        if (err) *err = rdna4_err(36, [NSString stringWithFormat:@"vkCreateImageView = %d", r]);
        if (heap) [self rdna4AbortHeapConstruction];
        return nil;
    }
    if (heap) {
        _hpBacking = [[RDNA4HeapBacking alloc] initWithBuffer:VK_NULL_HANDLE image:_img view:_view];
        if (!_hpBacking || ![heap rdna4BindSucceeded:_hpBlk lease:_hpLease owner:_hpBacking]) {
            if (err) *err = rdna4_err(22, @"cannot retain the heap texture's lifecycle backing");
            [self rdna4AbortHeapConstruction];
            return nil;
        }
    }
    RDNA4LOGR("RDNA4Texture %p: %lux%lu%s pf %lu vkformat %d levels %lu (alloc %llu, memtype %d)%s", (__bridge void *)self, (unsigned long)_w, (unsigned long)_h, is3D ? [NSString stringWithFormat:@"x%lu (3D)", (unsigned long)_depth].UTF8String : "",
           (unsigned long)_pf, _fmt->vk, (unsigned long)_levels, (unsigned long long)mr.size, mt, _viewSwz ? " [sampled through a swizzled view]" : "");
    return self;
}
// A single-level, identity-swizzle view for attachments, input attachments and storage images. Level 0 of a one-level, unswizzled texture is the sampling view itself.
// Bundle 9: a cube or 2D-array texture is attached one layer at a time (a plain 2D view of layer ly); every other texture has only layer 0.
- (VkImageView)rdna4AttViewLevel:(uint32_t)lv { return [self rdna4AttViewLevel:lv layer:0]; }
- (VkImageView)rdna4AttViewLevel:(uint32_t)lv layer:(uint32_t)ly {
    BOOL sameAsp = !_aspects || _aspects == rdna4dp_view_aspect(_aspects);   // bundle 10: the sampling view is the attachment view unless a combined depth/stencil format needs both aspects
    if (lv == 0 && ly == 0 && _levels <= 1 && !_viewSwz && !rdna4td_is_layered2d(_tdk) && sameAsp) return _view;
    RDNA4Texture *imageRoot = self; while (imageRoot->_root) imageRoot = imageRoot->_root;
    rdna4td_att_t av; if (!rdna4td_att_view(imageRoot->_tdk, (unsigned)imageRoot->_layers, [self rdna4Slice:ly], &av)) { RDNA4LOG("rdna4AttViewLevel %u layer %u: the texture has %u layer(s); falling back to the sampling view", lv, ly, [self rdna4Layers]); return _view; }
    @synchronized (self) {
        if (!_lvViews) _lvViews = [NSMutableDictionary dictionary];
        NSNumber *key = @(((unsigned long long)ly << 32) | lv);
        NSNumber *have = _lvViews[key]; if (have) return (VkImageView)(uintptr_t)have.unsignedLongLongValue;
        VkImageViewCreateInfo vc = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = [self vkImage],
            .viewType = (VkImageViewType)av.viewType, .format = _fmt->vk,
            .subresourceRange = { rdna4dp_barrier_aspect(_aspects), [self rdna4MipLevel:lv], 1, av.baseLayer, av.layerCount } };   // bundle 10: colour, or every depth/stencil aspect of the format
        VkImageView v = VK_NULL_HANDLE; VkResult r = vkCreateImageView(RDNA4R.dev, &vc, NULL, &v);
        if (r != VK_SUCCESS) { RDNA4LOG("rdna4AttViewLevel %u layer %u: vkCreateImageView = %d; falling back to the sampling view", lv, ly, r); return _view; }
        _lvViews[key] = @((unsigned long long)(uintptr_t)v);
        [_hpBacking addView:v];
        return v;
    }
}
- (VkImageView)rdna4AttViewLevel:(uint32_t)lv layer:(uint32_t)ly layerCount:(uint32_t)lc {
    if (lc <= 1) return [self rdna4AttViewLevel:lv layer:ly];
    @synchronized (self) {
        if (!_lvViews) _lvViews = [NSMutableDictionary dictionary];
        unsigned long long rawKey = (1ULL << 62) | ((unsigned long long)lc << 40) | ((unsigned long long)ly << 16) | (unsigned long long)lv;
        NSNumber *key = @(rawKey);
        NSNumber *have = _lvViews[key]; if (have) return (VkImageView)(uintptr_t)have.unsignedLongLongValue;
        VkImageViewCreateInfo vc = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = [self vkImage],
            .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY, .format = _fmt->vk,
            .subresourceRange = { rdna4dp_barrier_aspect(_aspects), [self rdna4MipLevel:lv], 1, [self rdna4Slice:ly], lc } };
        VkImageView v = VK_NULL_HANDLE; VkResult r = vkCreateImageView(RDNA4R.dev, &vc, NULL, &v);
        if (r != VK_SUCCESS) { RDNA4LOG("rdna4AttViewLevel %u layer %u+%u: vkCreateImageView = %d; falling back to single layer", lv, ly, lc, r); return [self rdna4AttViewLevel:lv layer:ly]; }
        _lvViews[key] = @((unsigned long long)(uintptr_t)v);
        [_hpBacking addView:v];
        return v;
    }
}
- (VkImageView)rdna4StorageView {
    if (!_viewSwz && _levels <= 1 && !_aspects) return _view;
    if (rdna4td_is_layered2d(_tdk)) return [self rdna4AttViewLevel:0 layer:0 layerCount:[self rdna4Layers]];
    return [self rdna4AttViewLevel:0];
}
- (void)rdna4AdoptHeap:(RDNA4Heap *)h offset:(NSUInteger)o size:(NSUInteger)sz bid:(uint64_t)bid {
    _hpHeap = h; _hpOff = o; _hpSize = sz; _hpId = bid;
    if (h) _hpPhysSize = sz;
    if (_acct) { atomic_fetch_sub(&rdna4_alloc_total, _acct); _acct = 0; }   // the heap's size is what counts
}
- (RDNA4Texture *)rdna4ImageRoot {
    RDNA4Texture *root = self;
    while (root->_root) root = root->_root;
    return root;
}
- (RDNA4Heap *)rdna4ResourceHeap { RDNA4Texture *root = [self rdna4ImageRoot]; return root->_hpHeap ?: [root->_hostBuf rdna4ResourceHeap]; }
- (NSUInteger)rdna4ResourceHeapOffset {
    if (_texel) return [_hostBuf rdna4ResourceHeapOffset];
    RDNA4Texture *root = [self rdna4ImageRoot];
    if (root->_hpHeap) return [root->_hpHeap type] == MTLHeapTypePlacement ? root->_hpOff : 0;
    return root->_hostBuf ? [root->_hostBuf rdna4ResourceHeapOffset] : 0;
}
- (BOOL)rdna4ResourceAliasable { return _texel ? [_hostBuf isAliasable] : _root ? [_root rdna4ResourceAliasable] : _hpAl; }
- (void)rdna4ResourceMakeAliasable {
    if (_root) {
        RDNA4_ONCE("makeAliasable on a texture view: prohibited by Metal; ignored without releasing the base resource");
        return;
    }
    RDNA4_HEAP_RETIRE;
}
- (BOOL)rdna4ResourceAliases:(id)other {
    if (_texel) return [_hostBuf rdna4ResourceAliases:other];
    RDNA4Texture *root = [self rdna4ImageRoot];
    return root->_hpHeap && root->_hpBlk &&
        [root->_hpHeap rdna4Block:root->_hpBlk offset:root->_hpBindOff size:root->_hpPhysSize aliases:other];
}
- (uint64_t)rdna4BackingBlock { return _texel ? [_hostBuf rdna4BackingBlock] : _root ? [_root rdna4BackingBlock] : _hpBlk; }
- (uint64_t)rdna4BackingOffset { return _texel ? [_hostBuf rdna4BackingOffset] : _root ? [_root rdna4BackingOffset] : _hpBindOff; }
- (uint64_t)rdna4BackingSize { return _texel ? [_hostBuf rdna4BackingSize] : _root ? [_root rdna4BackingSize] : _hpPhysSize; }
- (void)rdna4AbortHeapConstruction {
    RDNA4Heap *heap = _hpHeap;
    uint64_t bid = _hpId, block = _hpBlk, lease = _hpLease;
    const BOOL bound = _hpBacking != nil;
    [self rdna4DestroyRetiredBacking];
    _hpBacking = nil;
    if (heap && block && bound) [heap rdna4CloseBackingHandle:block];
    if (heap && lease) [heap rdna4AbandonBacking:lease];
    if (heap && bid) [heap rdna4Free:bid];
    _hpHeap = nil;
    _hpId = _hpBlk = _hpLease = _hpBindOff = _hpPhysSize = 0;
    _hpDedicated = _hpAl = NO; _hpOff = _hpSize = 0; _size = 0; _acct = 0;
}
- (void)rdna4DestroyFBCache {
    os_unfair_lock_lock(&_fbLock);
    const uint8_t n = _fbCacheN;
    _fbCacheN = 0;
    VkFramebuffer fbs[4];
    for (uint8_t i = 0; i < n; i++) { fbs[i] = _fbCache[i].fb; _fbCache[i].fb = VK_NULL_HANDLE; }
    os_unfair_lock_unlock(&_fbLock);
    if (RDNA4R.ok) for (uint8_t i = 0; i < n; i++) if (fbs[i]) vkDestroyFramebuffer(RDNA4R.dev, fbs[i], NULL);
}
- (VkFramebuffer)rdna4CachedFramebufferForRP:(VkRenderPass)rp view:(VkImageView)view width:(uint32_t)w height:(uint32_t)h {
    if (!RDNA4DO.on || RDNA4P.auditSubmit || !RDNA4R.ok || _hpHeap || (_root && [self rdna4ResourceHeap]) || !rp || !view ||
        RDNA4R.realCreateFB == NULL || vkCreateFramebuffer != RDNA4R.realCreateFB ||
        vkDestroyFramebuffer != RDNA4R.realDestroyFB)
        return VK_NULL_HANDLE;
    os_unfair_lock_lock(&_fbLock);
    for (uint8_t i = 0; i < _fbCacheN; i++) {
        if (_fbCache[i].rp == rp && _fbCache[i].view == view && _fbCache[i].w == w && _fbCache[i].h == h) {
            VkFramebuffer hit = _fbCache[i].fb;
            os_unfair_lock_unlock(&_fbLock);
            return hit;
        }
    }
    const BOOL full = _fbCacheN >= 4u;
    os_unfair_lock_unlock(&_fbLock);
    if (full) return VK_NULL_HANDLE;   // never evict a potentially in-flight cached FB; caller falls back to per-pass handles
    VkFramebufferCreateInfo fbc = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp,
        .attachmentCount = 1, .pAttachments = &view, .width = w, .height = h, .layers = 1 };
    VkFramebuffer fb = VK_NULL_HANDLE;
    if (vkCreateFramebuffer(RDNA4R.dev, &fbc, NULL, &fb) != VK_SUCCESS || !fb) return VK_NULL_HANDLE;
    os_unfair_lock_lock(&_fbLock);
    for (uint8_t i = 0; i < _fbCacheN; i++) {
        if (_fbCache[i].rp == rp && _fbCache[i].view == view && _fbCache[i].w == w && _fbCache[i].h == h) {
            VkFramebuffer existing = _fbCache[i].fb;
            os_unfair_lock_unlock(&_fbLock);
            vkDestroyFramebuffer(RDNA4R.dev, fb, NULL);
            return existing;
        }
    }
    if (_fbCacheN < 4u) {
        _fbCache[_fbCacheN++] = (RDNA4TexFBCacheEnt){ rp, view, w, h, fb };
        os_unfair_lock_unlock(&_fbLock);
        return fb;
    }
    os_unfair_lock_unlock(&_fbLock);
    vkDestroyFramebuffer(RDNA4R.dev, fb, NULL);
    return VK_NULL_HANDLE;
}
- (void)rdna4DestroyRetiredBacking {
    [self rdna4DestroyFBCache];
    if (_texelView && RDNA4R.ok) vkDestroyBufferView(RDNA4R.dev, _texelView, NULL);
    _texelView = VK_NULL_HANDLE;
    if (_hpBacking) [_hpBacking destroyHandles];
    else if (RDNA4R.ok) {
        for (NSNumber *lvv in _lvViews.allValues) vkDestroyImageView(RDNA4R.dev, (VkImageView)(uintptr_t)lvv.unsignedLongLongValue, NULL);
        if (_view) vkDestroyImageView(RDNA4R.dev, _view, NULL);
        if (_vramSurfCached) {
            uint64_t vsId = _vramSurfId;
            _vramSurfCached = NO; _vramSurfId = 0; _img = VK_NULL_HANDLE; _ibuf = VK_NULL_HANDLE;
            rdna4_vs_put(vsId);
        } else if (_img && !_root) {
            if (_iosLinearCached) rdna4_linimg_put(_img);
            else vkDestroyImage(RDNA4R.dev, _img, NULL);
        }
        _iosLinearCached = NO;
    }
    [_lvViews removeAllObjects];
    _view = VK_NULL_HANDLE; if (!_root) _img = VK_NULL_HANDLE;
}
- (void)dealloc {
    [self rdna4DestroyFBCache];
    const BOOL heapBacked = _hpHeap && _hpBlk;
    RDNA4_HEAP_DEALLOC;
    if (RDNA4R.ok && !heapBacked) {
        if (_texelView) vkDestroyBufferView(RDNA4R.dev, _texelView, NULL);
        for (NSNumber *lvv in _lvViews.allValues) vkDestroyImageView(RDNA4R.dev, (VkImageView)(uintptr_t)lvv.unsignedLongLongValue, NULL);
        if (_view) vkDestroyImageView(RDNA4R.dev, _view, NULL);
        if (_vramSurfCached) {
            uint64_t vsId = _vramSurfId;
            _vramSurfCached = NO; _vramSurfId = 0; _img = VK_NULL_HANDLE; _ibuf = VK_NULL_HANDLE;
            rdna4_vs_put(vsId);
        } else {
            if (_img && !_root) {
                if (_iosLinearCached) rdna4_linimg_put(_img);
                else vkDestroyImage(RDNA4R.dev, _img, NULL);
            }
            _iosLinearCached = NO;
            if (_pooled) { rdna4_mem_free(&_pm); atomic_fetch_sub(&rdna4_alloc_total, _acct); }   // P1
            else if (_mem) { vkFreeMemory(RDNA4R.dev, _mem, NULL); atomic_fetch_sub(&rdna4_alloc_total, _acct); }
            if (_ibuf) vkDestroyBuffer(RDNA4R.dev, _ibuf, NULL);
        }
        if (_imem && _impShared) rdna4_imp_put(_imem);   // P4: the shared import goes back to the cache (kept 2 s; freed later through the fence)
        else if (_imem) { vkFreeMemory(RDNA4R.dev, _imem, NULL); @synchronized ([GarnetDevice class]) { RDNA4R.impTotal -= _ialloc; } }
    }
    if (_led) rdna4_led_del(self);   // build 16 (P5 Step 0)
    if (_ios) CFRelease(_ios);   // build 16 (F3): no use count is held for the texture's life any more; a command buffer holds it while in flight
}
RDNA4_DNR(RDNA4Texture)
// ---- 11h.6: IOSurface-backed texture ----
// Accepts a 2D, page-aligned (P2: any plane of a 2/3-plane surface, via garnet_plane.h), page-multiple 32 bpp surface (BGRA8Unorm / RGBA8Unorm descriptors); anything else returns nil + log.
// The surface is retained for the texture's lifetime (build 16, F3: NOT use-counted for its life; a command buffer use-counts it while in flight, garnet_ledger.h); its pages are imported once (whole allocation).
- (instancetype)initWithDevice:(id)dev descriptor:(MTLTextureDescriptor *)d iosurface:(IOSurfaceRef)s plane:(NSUInteger)plane error:(NSError **)err {
    self = [super init];
    if (!self) return nil;
    #define IOSFAIL(code, ...) do { NSString *m_ = [NSString stringWithFormat:__VA_ARGS__]; RDNA4LOG_WARN("newTexture(iosurface): refused: %s", m_.UTF8String); if (err) *err = rdna4_err(code, m_); return nil; } while (0)
    if (!s) IOSFAIL(70, @"NULL IOSurface");
    _dev = dev; _w = d.width; _h = d.height; _pf = d.pixelFormat; _usage = d.usage; _layout = VK_IMAGE_LAYOUT_UNDEFINED; _iosPlane = plane;
    _swz = d.swizzle;
    _opts = ((NSUInteger)d.storageMode << 4) | ((NSUInteger)d.cpuCacheMode) | ((NSUInteger)d.hazardTrackingMode << 8);
    _fmt = rdna4_fmt(_pf);
    // m11h9: any pixel format of the table whose bytes per pixel equal the surface's bytes per element (BGR10A2Unorm 'l10r' and RGBA16Float 'RGhA' are what the compositor's
    // display surfaces use); an A8 / render-target mismatch is refused below by the same rules as an ordinary texture.
    if (!_fmt) IOSFAIL(71, @"pixel format %lu is not in the format table", (unsigned long)_pf);
    if (rdna4_fmt_is_bc(_fmt)) IOSFAIL(71, @"block-compressed IOSurface textures are not supported");
    if ((_fmt->flags & RDNA4F_A8) && (d.usage & MTLTextureUsageRenderTarget)) IOSFAIL(71, @"A8Unorm render targets are not supported");
    {
        const char *swzWhy = NULL;
        if (!rdna4swz_check(rdna4_swz_from_mtl(_swz), (unsigned long)d.usage, 0, 0, &swzWhy))
            IOSFAIL(71, @"%s", swzWhy);
    }
    if (d.textureType != MTLTextureType2D || d.depth != 1 || d.arrayLength != 1 || d.mipmapLevelCount != 1 || d.sampleCount != 1) IOSFAIL(72, @"only single 2D textures (no mips/array/MSAA)");
    size_t pc = IOSurfaceGetPlaneCount(s);
    // P2: a plane p < planeCount of a 2- or 3-plane surface is accepted (CoreAnimation makes one texture per plane of a bi-planar YUV surface). The kill file
    // /private/tmp/garnet-noplanes (latched once per process) restores the old refusal of every multi-plane surface.
    static int noplanes = -1; if (noplanes < 0) { noplanes = rdna4_sb_access_exists(RDNA4_NOPLANES_FILE) ? 1 : 0; if (noplanes) RDNA4LOG("P2 planes: multi-plane IOSurfaces REFUSED (kill file " RDNA4_NOPLANES_FILE " exists; latched for the life of this process)"); }
    if (pc > 1 && noplanes) IOSFAIL(73, @"IOSurface has %zu planes; single-plane surfaces only", pc);
    size_t pidx = pc > 1 ? plane : 0;   // pc <= 1: any plane != 0 is refused by rdna4pl_geom below, exactly as before
    size_t sw = pc ? IOSurfaceGetWidthOfPlane(s, pidx) : IOSurfaceGetWidth(s), sh = pc ? IOSurfaceGetHeightOfPlane(s, pidx) : IOSurfaceGetHeight(s);
    size_t bpe = pc ? IOSurfaceGetBytesPerElementOfPlane(s, pidx) : IOSurfaceGetBytesPerElement(s);
    size_t bpr = pc ? IOSurfaceGetBytesPerRowOfPlane(s, pidx) : IOSurfaceGetBytesPerRow(s);
    rdna4pl_in pin = { .pc = pc, .plane = plane, .pw = sw, .ph = sh, .pbpe = bpe, .pbpr = bpr, .poff = 0, .alloc = 0, .dw = _w, .dh = _h, .dbpp = _fmt->bpp };
    rdna4pl_out pout;
    if (!rdna4pl_geom(&pin, &pout)) IOSFAIL(pout.code, @"%s", pout.why);
    if (!rdna4_radv_open(err)) return nil;
    {   // the colour-attachment / sampled features of this format (RADV decides; before m11h9 every surface was 32 bpp BGRA8/RGBA8 and this was implied)
        VkFormatFeatureFlags ff = rdna4_fmt_feats(_fmt), nd = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
            ((d.usage & MTLTextureUsageRenderTarget) ? VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT : 0) | ((d.usage & MTLTextureUsageShaderWrite) ? VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT : 0);
        if ((ff & nd) != nd) IOSFAIL(92, @"pixel format %lu (vk %d): RADV features 0x%x lack 0x%x", (unsigned long)_pf, _fmt->vk, ff, nd & ~ff);
    }
    if (!RDNA4R.hostExt || !rdna4_vkGetMHPP) IOSFAIL(78, @"VK_EXT_external_memory_host is not available on this RADV");
    // Read-only lock to query base address without bumping IOSurfaceGetSeed (which would invalidate Phase B VRAM / Path (b) upload caches).
    IOSurfaceLock(s, kIOSurfaceLockReadOnly | kIOSurfaceLockAvoidSync, NULL);
    void *base = IOSurfaceGetBaseAddress(s);   // P2: the whole allocation is imported from the surface base; the plane is used at its offset
    void *pbase = pc ? IOSurfaceGetBaseAddressOfPlane(s, pidx) : base;
    IOSurfaceUnlock(s, kIOSurfaceLockReadOnly | kIOSurfaceLockAvoidSync, NULL);
    size_t alloc = IOSurfaceGetAllocSize(s);
    uint32_t spf = IOSurfaceGetPixelFormat(s);
    RDNA4LOGR("newTexture(iosurface): id %u %zux%zu bpr %zu allocSize %zu base %p pixelFormat 0x%08x planes %zu", IOSurfaceGetID(s), sw, sh, bpr, alloc, base, spf, pc);
    // F1 (build 16): when the import of the surface is refused (out of the kernel's import budget, an unimportable range) this init does NOT return nil (SkyLight aborts on a nil texture: AbortWithTextureInfo). It jumps to
    // the "baseless" branch below (same GPU-only image, same classification) with f1 set: the content captured through this wrapper is blank, the process stays up. The import cache is flushed and the import retried
    // first (rdna4_imp_get). The branch itself is not edited for this except that a size difference between descriptor and plane no longer refuses and no "protected" note is logged.
    BOOL f1 = NO;
    #define RDNA4F1_FALL(...) do { f1 = YES; atomic_fetch_add(&RDNA4LED.f1Fallbacks, 1); static _Atomic int f1log_; if (atomic_fetch_add(&f1log_, 1) < 16) { NSString *w_ = [NSString stringWithFormat:__VA_ARGS__]; RDNA4LOG_WARN("F1: the import of IOSurface %u was refused (%s): GPU-only texture instead of nil (the content captured through this wrapper is blank; the process stays up)", IOSurfaceGetID(s), w_.UTF8String); } goto nobase_path; } while (0)
    if (!pbase || !base) {   // P4: a protected IOSurface has no CPU mapping (CoreDisplay's DisplaySurface::GetMTLTexture cannot survive a nil texture). Not refused: a device-local optimal image of the descriptor's
        // size/format that is NOT backed by the surface's memory (the surface's contents are not shared). No import, no _ibuf, no _ibase: rdna4IsIOS is NO, so no upload/write-back copy and no CPU path touches a NULL base.
        // P6: classified like any other IOSurface texture (below, before return): with only the VkImage as D-copy source (vkCmdCopyImageToBuffer).
      nobase_path:
        if (!f1) rdna4_baseless_note(IOSurfaceGetID(s));
        if (!f1 && (_w != sw || _h != sh)) IOSFAIL(79, @"IOSurface has no CPU mapping and the descriptor %lux%lu differs from the plane %zux%zu", (unsigned long)_w, (unsigned long)_h, sw, sh);
        VkFormatFeatureFlags nbf = rdna4_fmt_feats(_fmt);
        BOOL nbAtt = !(_fmt->flags & RDNA4F_A8) && (nbf & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT);
        VkImageUsageFlags nbu = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | (nbAtt ? (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT) : 0) |
                                ((d.usage & MTLTextureUsageShaderWrite) ? VK_IMAGE_USAGE_STORAGE_BIT : 0);
        VkImageCreateInfo nic = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = (_usage & MTLTextureUsagePixelFormatView) ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0, .imageType = VK_IMAGE_TYPE_2D, .format = _fmt->vk, .extent = { (uint32_t)_w, (uint32_t)_h, 1 },
            .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = nbu, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
        if (garnet_async_compute_use_concurrent_image(RDNA4R.asyncComputeOn ? 1 : 0, RDNA4R.qfi, RDNA4R.computeQfi, 0)) {
            nic.sharingMode = VK_SHARING_MODE_CONCURRENT;
            nic.queueFamilyIndexCount = 2;
            nic.pQueueFamilyIndices = RDNA4R.sharedQfis;
        }
        VkResult nr = vkCreateImage(RDNA4R.dev, &nic, NULL, &_img);
        if (nr != VK_SUCCESS) { _img = VK_NULL_HANDLE; IOSFAIL(85, @"vkCreateImage(optimal, no CPU mapping) = %d", nr); }
        VkMemoryRequirements nmr; vkGetImageMemoryRequirements(RDNA4R.dev, _img, &nmr);
        int nmt = rdna4_find_mem(nmr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (nmt < 0) IOSFAIL(86, @"no device-local memory type");
        if (RDNA4P.on) {
            nr = rdna4_mem_alloc((uint32_t)nmt, nmr, NO, NO, VK_NULL_HANDLE, _img, &_pm);
            if (nr != VK_SUCCESS) IOSFAIL(87, @"vkAllocateMemory(image) = %d (pool)", nr);
            _pooled = YES; _acct = nmr.size; atomic_fetch_add(&rdna4_alloc_total, _acct);
            nr = vkBindImageMemory(RDNA4R.dev, _img, _pm.mem, _pm.off);
            if (nr != VK_SUCCESS) IOSFAIL(88, @"vkBindImageMemory = %d (pool, offset %llu)", nr, (unsigned long long)_pm.off);
        } else {
            VkMemoryAllocateInfo nma = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = nmr.size, .memoryTypeIndex = (uint32_t)nmt };
            nr = vkAllocateMemory(RDNA4R.dev, &nma, NULL, &_mem);
            if (nr != VK_SUCCESS) { _mem = VK_NULL_HANDLE; IOSFAIL(87, @"vkAllocateMemory(image) = %d", nr); }
            _acct = nmr.size; atomic_fetch_add(&rdna4_alloc_total, _acct);
            nr = vkBindImageMemory(RDNA4R.dev, _img, _mem, 0);
            if (nr != VK_SUCCESS) IOSFAIL(88, @"vkBindImageMemory = %d", nr);
        }
        _levels = 1; _depth = 1; _size = nmr.size;
        VkComponentMapping nbm = rdna4_view_map(_fmt, _swz); _viewSwz = !rdna4_is_identity_map(&nbm);
        VkImageViewCreateInfo nvc = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = _img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = _fmt->vk, .components = nbm, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        nr = vkCreateImageView(RDNA4R.dev, &nvc, NULL, &_view);
        if (nr != VK_SUCCESS) IOSFAIL(91, @"vkCreateImageView = %d", nr);
        _iosNoBase = YES; _ios = (IOSurfaceRef)CFRetain(s); rdna4_led_add(self, s, 0, spf, (uint32_t)pc, YES); _led = YES;   // retained (F3: not use-counted for its life) and entered in the ledger; set last so a failed init above never releases
        RDNA4LOGR("newTexture(iosurface) %p: plane %lu %zux%zu bpr %zu pf %lu -> GPU-ONLY (no CPU mapping): optimal VkImage, not backed by the surface's memory", (__bridge void *)self, (unsigned long)plane, sw, sh, bpr, (unsigned long)_pf);
        // P6: same rule as the mapped surfaces (rdna4s_classify: geometry/pitch vs the plane + the DisplaySurface::GetMTLTexture backtrace). Inputs: width/height/pixel format/bytesPerRow/allocSize from the IOSurface
        // object (none needs a CPU mapping); alloc 0 -> bpr*height (rdna4df_nobase_alloc); the cache key's base is 0, which no mapped surface has (a mapped one passed the !base test above).
        // The image is the copy source, so the format must be 4 bytes per texel (the slot's pitch is in 4-byte texels) and the image must be the plane's size.
        if (pc <= 1 && _fmt->bpp == 4 && rdna4s_classify(sw, sh, spf, d.usage, bpr, (size_t)rdna4df_nobase_alloc(alloc, bpr, sh), IOSurfaceGetID(s), NULL)) {
            _disp = YES;
            static _Atomic int nblog; if (atomic_fetch_add(&nblog, 1) < 8) RDNA4LOG("scanout: display surface %u has no CPU mapping (protected): its copies into the scanout slot come from the VkImage (vkCmdCopyImageToBuffer, TRANSFER_SRC)", IOSurfaceGetID(s));
        }
        return self;
    }
    if (((uintptr_t)base & (RDNA4R.hostAlign - 1)) || !alloc) RDNA4F1_FALL(@"base %p / allocSize %zu: base not aligned to %llu (page-aligned surfaces only)", base, alloc, (unsigned long long)RDNA4R.hostAlign);
    pin.alloc = alloc; pin.poff = ((uintptr_t)pbase >= (uintptr_t)base) ? (uint64_t)((uintptr_t)pbase - (uintptr_t)base) : UINT64_MAX;
    if (!rdna4pl_bound(&pin, &pout)) IOSFAIL(pout.code, @"%s", pout.why);
    const NSUInteger poff = (NSUInteger)pout.off;
    if (pc > 1) RDNA4LOGR("newTexture(iosurface): plane %lu of %zu: offset %lu size %zux%zu bpe %zu bpr %zu (bufferRowLength %zu texels)", (unsigned long)plane, pc, (unsigned long)poff, sw, sh, bpe, bpr, pout.rowlen);
    // m11h6: IOSurface allocations are page-granular in the VM map, but IOSurfaceGetAllocSize reports the exact byte size (e.g. 5760, 303360, 1840000).
    // Import the whole pages that hold the surface: round up to the host-pointer alignment. The extra tail bytes are the surface's own page, never another object's.
    const size_t ialloc = (alloc + (size_t)RDNA4R.hostAlign - 1) & ~((size_t)RDNA4R.hostAlign - 1);
    if (ialloc != alloc) RDNA4LOGR("newTexture(iosurface): allocSize %zu rounded up to %zu for the page-granular import", alloc, ialloc);
    if (ialloc > 64u << 20) RDNA4F1_FALL(@"allocSize %zu > 64 MiB (the RDNA4N per-BO import limit)", ialloc);
    // Import (whole allocation). The kernel refuses BAR pages, unaligned ranges and > 256 MiB per client; that arrives as a vkAllocateMemory error.
    VkMemoryHostPointerPropertiesEXT hp = { .sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT };
    uint64_t timp_ = rdna4_now();
    VkResult r = rdna4_vkGetMHPP(RDNA4R.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, base, &hp);
    if (r != VK_SUCCESS || !hp.memoryTypeBits) RDNA4F1_FALL(@"vkGetMemoryHostPointerPropertiesEXT = %d bits 0x%x", r, hp.memoryTypeBits);
    int hmt = rdna4_find_mem(hp.memoryTypeBits, 0);
    VkImportMemoryHostPointerInfoEXT imp = { .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, .pHostPointer = base };
    VkMemoryAllocateInfo ma = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp, .allocationSize = ialloc, .memoryTypeIndex = (uint32_t)hmt };
    BOOL impHit = NO;
    if (RDNA4IC.on) { r = rdna4_imp_get(s, base, ialloc, &ma, &_imem, &impHit); _impShared = YES; }   // P4: shared import (a reused one is a hit)
    else r = rdna4_import_alloc(&ma, &_imem);
    rdna4t_add(&T1.imp, rdna4_now() - timp_);
    if (rdna4f1_action((int)r) == RDNA4F1_FALLBACK) { _imem = VK_NULL_HANDLE; _impShared = NO; RDNA4F1_FALL(@"vkAllocateMemory(import %zu B at %p) = %d, running import total %llu B", ialloc, base, r, (unsigned long long)RDNA4R.impTotal); }
    int protection = rdna4nc_host_readonly(base, ialloc);
    _hostReadOnly = protection != 0;
    if (protection < 0) RDNA4LOGR("RDNA4Texture %p: NO-COPY protection unknown, treating as read-only", (__bridge void *)self);
    if (_hostReadOnly) RDNA4LOGR("RDNA4Texture %p: NO-COPY %zu bytes at %p imported READ-ONLY (GPU writes will be dropped)", (__bridge void *)self, ialloc, base);
    if (impHit) { static _Atomic int nlog; if (atomic_fetch_add(&nlog, 1) < 8) RDNA4LOGR("newTexture(iosurface): id %u import REUSED from the cache (%zu B)", IOSurfaceGetID(s), ialloc); }
    else @synchronized ([GarnetDevice class]) { RDNA4R.impTotal += ialloc; RDNA4LOGR("newTexture(iosurface): imported %zu B (host memory type %d, bits 0x%x); running import total %llu B", ialloc, hmt, hp.memoryTypeBits, (unsigned long long)RDNA4R.impTotal); }
    _ialloc = ialloc; _ibase = (uint8_t *)base + poff; _ioff = poff; _ibpr = bpr; _ios = (IOSurfaceRef)CFRetain(s); rdna4_led_add(self, s, ialloc, spf, (uint32_t)pc, NO); _led = YES; _size = ialloc;   // F3: retained, not use-counted for its life; entered in the ledger
    // S5.2a: classify display surfaces BEFORE Phase B VRAM cache lookup so Class 1 (_disp) promotes directly into hi-pool VRAM.
    if (pc <= 1 && _w == sw && _h == sh && rdna4s_classify(sw, sh, spf, d.usage, bpr, alloc, IOSurfaceGetID(s), base)) {   // build 7: only an exact-size texture can be a display surface
        _disp = YES;
    }
    VkFormatFeatureFlags offs = rdna4_fmt_feats(_fmt);
    BOOL attOK = !(_fmt->flags & RDNA4F_A8) && (offs & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT);
    VkImageUsageFlags use = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | (attOK ? (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT) : 0) |
                            ((d.usage & MTLTextureUsageShaderWrite) ? VK_IMAGE_USAGE_STORAGE_BIT : 0);
    const char *force = garnet_getenv("GARNET_IOS_PATH", "RDNA4M_IOS_PATH");   // test hook: "b" forces the copy path
    if (RDNA4VS.on && RDNA4IC.on && _impShared && pc <= 1 && !(force && force[0] == 'a') &&
        rdna4_vs_try_bind(s, base, ialloc, alloc, poff, _w, _h, bpr, _fmt, d.usage, use, offs, attOK, _disp, _hostReadOnly, &ma, &_img, &_ibuf, &_vramSurfId)) {
        _vramSurfCached = YES;
        _iosLinear = NO;
    }
    // The first submitted segment restores the first-use snapshot with vkCmdCopyBufferToImage into the
    // LINEAR image bound to these host pages, even for a sampled-only use. RO imports need path (b).
    BOOL tryA = !_vramSurfCached && !_hostReadOnly && !(force && force[0] == 'b');
    if (tryA) {   // path (a) needs the LINEAR-tiling features for every usage asked for (RADV does not check at vkCreateImage); otherwise the copy path (b)
        VkFormatProperties lfp = {0}; vkGetPhysicalDeviceFormatProperties(RDNA4R.pd, _fmt->vk, &lfp);
        VkFormatFeatureFlags lneed = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
            (attOK ? VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT : 0) | ((d.usage & MTLTextureUsageShaderWrite) ? VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT : 0);
        if ((lfp.linearTilingFeatures & lneed) != lneed) { RDNA4LOGR("newTexture(iosurface): path (a) unavailable: LINEAR features 0x%x lack 0x%x for vk format %d", lfp.linearTilingFeatures, lneed & ~lfp.linearTilingFeatures, _fmt->vk); tryA = NO; }
        else RDNA4LOGR("newTexture(iosurface): LINEAR features 0x%x cover 0x%x for vk format %d", lfp.linearTilingFeatures, lneed, _fmt->vk);
    }
    if (tryA) {
        const VkImageCreateFlags liFlags = (_usage & MTLTextureUsagePixelFormatView) ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0;
        const rdna4li_key lk = { .id = IOSurfaceGetID(s), .base = (uint64_t)(uintptr_t)base, .size = ialloc, .poff = poff,
                                 .w = (uint32_t)_w, .h = (uint32_t)_h, .bpr = bpr,
                                 .vk_fmt = (uint32_t)_fmt->vk, .usage = (uint32_t)use, .flags = (uint32_t)liFlags,
                                 .mem = (void *)_imem };
        VkImage cachedImg = VK_NULL_HANDLE; int cachedInited = 0;
        if (RDNA4IC.on && _impShared && rdna4_linimg_get(&lk, &cachedImg, &cachedInited)) {
            _img = cachedImg;
            _iosLinear = YES;
            _iosLinearCached = YES;
            if (cachedInited) { _iosLinearSubmitted = YES; _layout = VK_IMAGE_LAYOUT_GENERAL; }
        } else {
        VkExternalMemoryImageCreateInfo emi = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT };
        VkImageCreateInfo ic = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &emi, .flags = liFlags, .imageType = VK_IMAGE_TYPE_2D, .format = _fmt->vk, .extent = { (uint32_t)_w, (uint32_t)_h, 1 },
            .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_LINEAR, .usage = use, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
        if (garnet_async_compute_use_concurrent_image(RDNA4R.asyncComputeOn ? 1 : 0, RDNA4R.qfi, RDNA4R.computeQfi, 0)) {
            ic.sharingMode = VK_SHARING_MODE_CONCURRENT;
            ic.queueFamilyIndexCount = 2;
            ic.pQueueFamilyIndices = RDNA4R.sharedQfis;
        }
        VkImage li = VK_NULL_HANDLE; r = vkCreateImage(RDNA4R.dev, &ic, NULL, &li);
        if (r != VK_SUCCESS) RDNA4LOGR("newTexture(iosurface): path (a) unavailable: linear vkCreateImage = %d", r);
        else {
            VkImageSubresource sr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 }; VkSubresourceLayout sl; vkGetImageSubresourceLayout(RDNA4R.dev, li, &sr, &sl);
            VkMemoryRequirements mr; vkGetImageMemoryRequirements(RDNA4R.dev, li, &mr);
            const VkDeviceSize naturalRowPitch = sl.rowPitch;
            const BOOL naturalPitchOK = sl.rowPitch == bpr && sl.offset == 0;
            BOOL pitchOK = naturalPitchOK;
            BOOL explicitOK = NO, alreadyBound = NO;
            int explicitReason = rdna4p_explicit_pitch_reason(bpr, _w, _fmt->bpp, sl.rowPitch, ic.flags, ic.mipLevels, ic.arrayLayers, ic.samples, RDNA4R.drmModExt);
            if (sl.rowPitch == bpr && sl.offset != 0) explicitReason = RDNA4P_EXPLICIT_OFFSET;
            if (!pitchOK && explicitReason == RDNA4P_EXPLICIT_TRY) {
                VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifierQuery = {
                    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
                    .drmFormatModifier = 0,
                    .sharingMode = ic.sharingMode,
                    .queueFamilyIndexCount = ic.queueFamilyIndexCount,
                    .pQueueFamilyIndices = ic.pQueueFamilyIndices };
                VkPhysicalDeviceImageFormatInfo2 formatQuery = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
                    .pNext = &modifierQuery, .format = ic.format, .type = ic.imageType,
                    .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT, .usage = ic.usage, .flags = ic.flags };
                VkImageFormatProperties2 formatProps = { .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2 };
                VkResult qr = vkGetPhysicalDeviceImageFormatProperties2(RDNA4R.pd, &formatQuery, &formatProps);
                vkDestroyImage(RDNA4R.dev, li, NULL); li = VK_NULL_HANDLE;
                if (qr == VK_SUCCESS) {
                    VkSubresourceLayout planeLayout = { .offset = 0, .size = 0, .rowPitch = bpr };
                    VkImageDrmFormatModifierExplicitCreateInfoEXT modifierInfo = {
                        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
                        .pNext = &emi, .drmFormatModifier = 0,
                        .drmFormatModifierPlaneCount = 1, .pPlaneLayouts = &planeLayout };
                    VkImageCreateInfo explicitIc = ic;
                    explicitIc.pNext = &modifierInfo;
                    explicitIc.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
                    r = vkCreateImage(RDNA4R.dev, &explicitIc, NULL, &li);
                    if (r == VK_SUCCESS) {
                        VkImageSubresource explicitSr = { VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT, 0, 0 };
                        vkGetImageSubresourceLayout(RDNA4R.dev, li, &explicitSr, &sl);
                        vkGetImageMemoryRequirements(RDNA4R.dev, li, &mr);
                        pitchOK = sl.rowPitch == bpr && sl.offset == 0;
                        BOOL explicitFits = pitchOK && rdna4pl_bind_ok(poff, mr.size, mr.alignment, ialloc) &&
                            (mr.alignment == 0 || ((uintptr_t)base % mr.alignment) == 0) && (mr.memoryTypeBits & (1u << hmt));
                        if (explicitFits) {
                            r = vkBindImageMemory(RDNA4R.dev, li, _imem, poff);
                            explicitFits = r == VK_SUCCESS;
                            alreadyBound = explicitFits;
                        }
                        explicitOK = explicitFits;
                        if (!explicitFits) {
                            explicitReason = pitchOK ? RDNA4P_EXPLICIT_BIND : RDNA4P_EXPLICIT_LAYOUT;
                            pitchOK = NO;
                            vkDestroyImage(RDNA4R.dev, li, NULL); li = VK_NULL_HANDLE;
                        }
                    } else explicitReason = RDNA4P_EXPLICIT_CREATE_FAILED;
                } else explicitReason = RDNA4P_EXPLICIT_UNSUPPORTED;
            }
            static _Atomic int explicitLogCount;
            if (!naturalPitchOK && atomic_fetch_add(&explicitLogCount, 1) < 8)
                RDNA4LOG("newTexture(iosurface): natural pitch %llu vs IOSurface bpr %zu -> explicit pitch %s%s%s%s",
                    (unsigned long long)naturalRowPitch, bpr, explicitOK ? "OK" : "refused",
                    explicitOK ? "" : " (", explicitOK ? "" : rdna4p_explicit_pitch_reason_text(explicitReason), explicitOK ? "" : ")");
            NSString *pitchResult = !naturalPitchOK ? [NSString stringWithFormat:@" -> explicit pitch %@%@",
                explicitOK ? @"OK" : @"refused", explicitOK ? @"" : [NSString stringWithFormat:@" (%s)", rdna4p_explicit_pitch_reason_text(explicitReason)]] : @"";
            RDNA4LOGR("newTexture(iosurface): linear layout for %lux%lu (descriptor; surface %zux%zu): rowPitch %llu offset %llu size %llu vs IOSurface bytesPerRow %zu -> %s%s; image req size %llu align %llu typeBits 0x%x (host bits 0x%x)",
                   (unsigned long)_w, (unsigned long)_h, sw, sh, (unsigned long long)sl.rowPitch, (unsigned long long)sl.offset, (unsigned long long)sl.size, bpr, naturalPitchOK ? "MATCH" : "MISMATCH",
                   pitchResult.UTF8String,
                   (unsigned long long)mr.size, (unsigned long long)mr.alignment, mr.memoryTypeBits, hp.memoryTypeBits);
            BOOL fits = pitchOK && rdna4pl_bind_ok(poff, mr.size, mr.alignment, ialloc) && (mr.alignment == 0 || ((uintptr_t)base % mr.alignment) == 0) && (mr.memoryTypeBits & (1u << hmt));
            if (pitchOK && !fits && poff) RDNA4LOGR("newTexture(iosurface): plane offset %lu: LINEAR bind at that offset not possible (image align %llu size %llu, import %zu) -> copy path (b)", (unsigned long)poff, (unsigned long long)mr.alignment, (unsigned long long)mr.size, ialloc);
            if (pitchOK && !fits) RDNA4LOGR("newTexture(iosurface): path (a) unavailable: image requirements do not fit the import (size/alignment/memory type)");
            if (fits && !alreadyBound) { r = vkBindImageMemory(RDNA4R.dev, li, _imem, poff); if (r != VK_SUCCESS) { RDNA4LOGR("newTexture(iosurface): path (a) unavailable: vkBindImageMemory = %d", r); fits = NO; } }
            if (fits && li) {
                _iosLinear = YES;
                if (RDNA4IC.on && _impShared) {
                    int racedInited = 0;
                    _img = rdna4_linimg_add(&lk, li, &racedInited);
                    _iosLinearCached = YES;
                    if (racedInited) { _iosLinearSubmitted = YES; _layout = VK_IMAGE_LAYOUT_GENERAL; }
                } else {
                    _img = li;
                }
            } else if (li) vkDestroyImage(RDNA4R.dev, li, NULL);
        }
        }
    }
    if (!_vramSurfCached && !_iosLinear) {   // path (b): optimal device-local image + a VkBuffer over the import
        VkImageCreateInfo ic = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = (_usage & MTLTextureUsagePixelFormatView) ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0, .imageType = VK_IMAGE_TYPE_2D, .format = _fmt->vk, .extent = { (uint32_t)_w, (uint32_t)_h, 1 },
            .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = use, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
        if (garnet_async_compute_use_concurrent_image(RDNA4R.asyncComputeOn ? 1 : 0, RDNA4R.qfi, RDNA4R.computeQfi, 0)) {
            ic.sharingMode = VK_SHARING_MODE_CONCURRENT;
            ic.queueFamilyIndexCount = 2;
            ic.pQueueFamilyIndices = RDNA4R.sharedQfis;
        }
        r = vkCreateImage(RDNA4R.dev, &ic, NULL, &_img);
        if (r != VK_SUCCESS) { _img = VK_NULL_HANDLE; IOSFAIL(85, @"vkCreateImage(optimal) = %d", r); }
        VkMemoryRequirements mr; vkGetImageMemoryRequirements(RDNA4R.dev, _img, &mr);
        int mt = rdna4_find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mt < 0) IOSFAIL(86, @"no device-local memory type");
        VkMemoryAllocateInfo m2 = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)mt };
        if (RDNA4P.on) {   // P1 (the optimal device-local image of path (b); the import stays as it was)
            r = rdna4_mem_alloc((uint32_t)mt, mr, NO, NO, VK_NULL_HANDLE, _img, &_pm);
            if (r != VK_SUCCESS) IOSFAIL(87, @"vkAllocateMemory(image) = %d (pool)", r);
            _pooled = YES; _acct = mr.size; atomic_fetch_add(&rdna4_alloc_total, _acct);
            r = vkBindImageMemory(RDNA4R.dev, _img, _pm.mem, _pm.off);
            if (r != VK_SUCCESS) IOSFAIL(88, @"vkBindImageMemory = %d (pool, offset %llu)", r, (unsigned long long)_pm.off);
        } else {
        r = vkAllocateMemory(RDNA4R.dev, &m2, NULL, &_mem);
        if (r != VK_SUCCESS) { _mem = VK_NULL_HANDLE; IOSFAIL(87, @"vkAllocateMemory(image) = %d", r); }
        _acct = mr.size; atomic_fetch_add(&rdna4_alloc_total, _acct);
        r = vkBindImageMemory(RDNA4R.dev, _img, _mem, 0);
        if (r != VK_SUCCESS) IOSFAIL(88, @"vkBindImageMemory = %d", r);
        }
    }
    // Both paths (a) and (b) need a buffer alias; Phase B VRAM-cached entries already borrow entry->ibuf.
    if (!_vramSurfCached) {
        VkExternalMemoryBufferCreateInfo emb = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT };
        VkBufferCreateInfo bc = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &emb,
            .size = alloc, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT };
        if (garnet_async_compute_use_concurrent_buffer(RDNA4R.asyncComputeOn ? 1 : 0, RDNA4R.qfi, RDNA4R.computeQfi)) {
            bc.sharingMode = VK_SHARING_MODE_CONCURRENT;
            bc.queueFamilyIndexCount = 2;
            bc.pQueueFamilyIndices = RDNA4R.sharedQfis;
        }
        r = vkCreateBuffer(RDNA4R.dev, &bc, NULL, &_ibuf);
        if (r != VK_SUCCESS) { _ibuf = VK_NULL_HANDLE; IOSFAIL(89, @"vkCreateBuffer(import) = %d", r); }
        r = vkBindBufferMemory(RDNA4R.dev, _ibuf, _imem, 0);
        if (r != VK_SUCCESS) IOSFAIL(90, @"vkBindBufferMemory(import) = %d", r);
    }
    _levels = 1; _depth = 1;
    VkComponentMapping icm = rdna4_view_map(_fmt, _swz); _viewSwz = !rdna4_is_identity_map(&icm);
    VkImageViewCreateInfo vc = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = _img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = _fmt->vk, .components = icm, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    r = vkCreateImageView(RDNA4R.dev, &vc, NULL, &_view);
    if (r != VK_SUCCESS) IOSFAIL(91, @"vkCreateImageView = %d", r);
    RDNA4LOGR("newTexture(iosurface) %p: plane %lu offset %lu %zux%zu bpr %zu pf %lu -> PATH (%s): %s", (__bridge void *)self, (unsigned long)plane, (unsigned long)poff, sw, sh, bpr, (unsigned long)_pf,
           _vramSurfCached ? "vram" : (_iosLinear ? "a" : "b"),
           _vramSurfCached ? "OPTIMAL hi-pool VRAM cached surface (shared optImg, lazy writeback)" :
           (_iosLinear ? "LINEAR VkImage bound directly to the imported IOSurface pages" : "optimal VkImage + copies to/from a VkBuffer over the imported pages (bufferRowLength = bytesPerRow/4)"));
    #undef IOSFAIL
    return self;
}
// A texture over a buffer (Metal's linear texture; QuartzCore's client-memory backing stores). Single-level 2D, formats of the table, Shared/Managed buffer.
// Path (b) of the IOSurface design: an optimal device-local image plus copies between it and the buffer around each command buffer (upload at the first use,
// write-back at the end), so the buffer memory (the client's memory for a no-copy buffer) is always the CPU-visible truth.
- (instancetype)initWithDevice:(id)dev descriptor:(MTLTextureDescriptor *)d buffer:(RDNA4Buffer *)b offset:(NSUInteger)off bytesPerRow:(NSUInteger)bpr error:(NSError **)err {
    if (d.textureType == MTLTextureTypeTextureBuffer) {
        self = [self initTextureBufferWithDevice:dev descriptor:d buffer:b offset:off error:err];
        if (self) _ibpr = bpr ?: _w * _fmt->bpp;
        return self;
    }
    self = [super init]; if (!self) return nil;
    #define BFAIL(code, ...) do { if (err) *err = rdna4_err(code, [NSString stringWithFormat:__VA_ARGS__]); return nil; } while (0)
    _dev = dev; _w = d.width; _h = d.height; _pf = d.pixelFormat; _usage = d.usage; _layout = VK_IMAGE_LAYOUT_UNDEFINED;
    _swz = d.swizzle;
    _opts = ((NSUInteger)d.storageMode << 4) | ((NSUInteger)d.cpuCacheMode) | ((NSUInteger)d.hazardTrackingMode << 8);
    _fmt = rdna4_fmt(_pf);
    if (!_fmt) BFAIL(120, @"pixel format %lu not supported", (unsigned long)_pf);
    if (rdna4_fmt_is_bc(_fmt)) BFAIL(120, @"block-compressed buffer-backed textures are not supported");
    if (d.textureType != MTLTextureType2D || d.depth != 1 || d.arrayLength != 1 || d.mipmapLevelCount != 1 || d.sampleCount != 1) BFAIL(121, @"only single 2D textures (no mips/array/MSAA)");
    if ((_fmt->flags & RDNA4F_A8) && (d.usage & MTLTextureUsageRenderTarget)) BFAIL(121, @"A8Unorm render targets are not supported");
    if (![b isKindOfClass:[RDNA4Buffer class]] || ![b contents]) BFAIL(122, @"the buffer has no CPU mapping (Private storage)");
    uint32_t bpp = _fmt->bpp;
    rdna4_texture_transfer transfer;
    if (!rdna4_texture_transfer_layout(_w, _h, 1, bpp, bpr, 0, &transfer) || bpr % bpp)
        BFAIL(123, @"bytesPerRow %lu or geometry invalid for a %lu-wide %u-byte format", (unsigned long)bpr, (unsigned long)_w, bpp);
    if (off % 4 || off % bpp) BFAIL(124, @"offset %lu must be a multiple of 4 and of the pixel size", (unsigned long)off);
    uint64_t vkoff;
    if (__builtin_add_overflow([b rdna4VkOffset], (uint64_t)off, &vkoff) ||
        !rdna4nc_vk_offset_ok(vkoff, bpp, RDNA4NC_ALIGN_IMAGE)) BFAIL(124, @"buffer-backed texture Vulkan offset overflows or violates pixel alignment");
    if (!rdna4_buffer_range([b length], off, transfer.span)) BFAIL(125, @"offset %lu + %lu rows of %lu bytes exceed the %lu-byte buffer", (unsigned long)off, (unsigned long)_h, (unsigned long)bpr, (unsigned long)[b length]);
    rdna4_heap_image_plan plan;
    if (!rdna4_heap_image_plan_make(d, NO, &plan, err)) return nil;
    VkImageCreateInfo ic = plan.create;
    VkResult r = vkCreateImage(RDNA4R.dev, &ic, NULL, &_img);
    if (r != VK_SUCCESS) { _img = VK_NULL_HANDLE; BFAIL(126, @"vkCreateImage = %d", r); }
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(RDNA4R.dev, _img, &mr);
    int mt = rdna4_find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt < 0) BFAIL(127, @"no device-local memory type");
    VkMemoryAllocateInfo ma = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)mt };
    if (RDNA4P.on) {   // P1
        r = rdna4_mem_alloc((uint32_t)mt, mr, NO, NO, VK_NULL_HANDLE, _img, &_pm);
        if (r != VK_SUCCESS) BFAIL(128, @"vkAllocateMemory = %d (pool)", r);
        _pooled = YES; _size = (NSUInteger)mr.size; _acct = mr.size; atomic_fetch_add(&rdna4_alloc_total, _acct);
        r = vkBindImageMemory(RDNA4R.dev, _img, _pm.mem, _pm.off);
        if (r != VK_SUCCESS) BFAIL(129, @"vkBindImageMemory = %d (pool, offset %llu)", r, (unsigned long long)_pm.off);
    } else {
    r = vkAllocateMemory(RDNA4R.dev, &ma, NULL, &_mem);
    if (r != VK_SUCCESS) { _mem = VK_NULL_HANDLE; BFAIL(128, @"vkAllocateMemory = %d", r); }
    _size = (NSUInteger)mr.size; _acct = mr.size; atomic_fetch_add(&rdna4_alloc_total, _acct);
    r = vkBindImageMemory(RDNA4R.dev, _img, _mem, 0);
    if (r != VK_SUCCESS) BFAIL(129, @"vkBindImageMemory = %d", r);
    }
    _levels = 1; _depth = 1;
    VkComponentMapping bcm = rdna4_view_map(_fmt, _swz); _viewSwz = !rdna4_is_identity_map(&bcm);
    VkImageViewCreateInfo vc = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = _img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = _fmt->vk, .components = bcm, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    r = vkCreateImageView(RDNA4R.dev, &vc, NULL, &_view);
    if (r != VK_SUCCESS) BFAIL(130, @"vkCreateImageView = %d", r);
    _hostBuf = b; _hoff = off; _ibase = (uint8_t *)[b contents] + off; _ibpr = bpr;
    RDNA4LOG("RDNA4Texture %p: %lux%lu pf %lu over buffer %p offset %lu bytesPerRow %lu (%s memory; copy path b)", (__bridge void *)self, (unsigned long)_w, (unsigned long)_h, (unsigned long)_pf,
           (__bridge void *)b, (unsigned long)off, (unsigned long)bpr, [b rdna4Imported] ? "client" : "bundle");
    #undef BFAIL
    return self;
}
- (BOOL)rdna4IsDisp { return _root ? [_root rdna4IsDisp] : _disp; }
- (const uint8_t *)rdna4IBase { return _root ? [_root rdna4IBase] : _ibase; }
- (IOSurfaceRef)rdna4IOSRef { return _root ? [_root rdna4IOSRef] : (_iosNoBase ? NULL : _ios); }   // P4: a base-less texture is not an IOSurface-coherent texture (sid tracking, CRC)
- (VkBuffer)rdna4DispBuf { return _root ? [_root rdna4DispBuf] : _ibuf; }
- (BOOL)rdna4DispImgOnly { return _root ? [_root rdna4DispImgOnly] : (_disp && _iosNoBase && !_ibuf); }
- (unsigned)rdna4DispSid { return _root ? [_root rdna4DispSid] : ((_disp && _ios) ? (unsigned)IOSurfaceGetID(_ios) : 0); }
- (BOOL)rdna4IsIOS { return _root ? [_root rdna4IsIOS] : ((_ios != NULL && !_iosNoBase) || (_hostBuf != nil && !_texel)); }
- (NSUInteger)rdna4IOSOffset { return _root ? [_root rdna4IOSOffset] : (_hostBuf ? [_hostBuf rdna4VkOffset] + _hoff : _ioff); }
- (IOSurfaceRef)rdna4UCSurface { return _root ? [_root rdna4UCSurface] : _ios; }
- (void)rdna4AliasKey:(uint32_t *)sid off:(uint64_t *)off {
    if (_root) { [_root rdna4AliasKey:sid off:off]; return; }
    if (_hostBuf) { *sid = 0; *off = (uint64_t)(uintptr_t)[_hostBuf contents] + (uint64_t)_hoff; }
    else { *sid = _ios ? (uint32_t)IOSurfaceGetID(_ios) : 0; *off = (uint64_t)_ioff; }
}
- (BOOL)rdna4HostReadOnly { return _root ? [_root rdna4HostReadOnly] : (_hostReadOnly || [_hostBuf rdna4HostReadOnly]); }
- (BOOL)rdna4IOSLinear { return _root ? [_root rdna4IOSLinear] : _iosLinear; }
- (BOOL)rdna4IOSLinearNeedsInit {
    if (!_iosLinear || _iosLinearSubmitted) return NO;
    if (_iosLinearCached && _img && rdna4_linimg_is_inited(_img)) {
        _iosLinearSubmitted = YES;
        _layout = VK_IMAGE_LAYOUT_GENERAL;
        return NO;
    }
    return YES;
}
- (void)rdna4IOSLinearDidSubmit { _iosLinearSubmitted = YES; if (_iosLinearCached && _img) rdna4_linimg_mark_inited(_img); }
- (BOOL)rdna4VramSurfCached { return _root ? [_root rdna4VramSurfCached] : _vramSurfCached; }
- (uint64_t)rdna4VramSurfId { return _root ? [_root rdna4VramSurfId] : _vramSurfId; }
- (uint64_t)rdna4PathBSeedState { return _root ? [_root rdna4PathBSeedState] : atomic_load_explicit(&_pathBSeedState, memory_order_acquire); }
- (void)rdna4PathBSetSeed:(uint32_t)seed {
    if (_root) { [_root rdna4PathBSetSeed:seed]; return; }
    atomic_store_explicit(&_pathBSeedState, garnet_vs_pathb_pack(seed), memory_order_release);
}
- (void)rdna4PathBInvalidate {
    if (_root) { [_root rdna4PathBInvalidate]; return; }
    atomic_store_explicit(&_pathBSeedState, 0ULL, memory_order_release);
}
- (VkBuffer)rdna4IOSBuffer { return _root ? [_root rdna4IOSBuffer] : (_hostBuf ? [_hostBuf vkBuffer] : _ibuf); }
- (size_t)rdna4IOSBytesPerRow { return _root ? [_root rdna4IOSBytesPerRow] : _ibpr; }
// 11e CPU access. Shared and Managed textures accept replaceRegion:/getBytes: (Managed has no separate CPU copy here: both are the one
// DEVICE_LOCAL image reached through a host-visible staging buffer + a one-shot copy that is WAITED, so the data is in place when the call
// returns). Private textures are REFUSED (Metal does not allow CPU access to them): there is no NSError channel in these selectors, so the
// refusal is an RDNA4LOG line and a no-op. Rows are re-packed tightly into the staging buffer, so any bytesPerRow is accepted.
- (void)replaceRegion:(MTLRegion)r mipmapLevel:(NSUInteger)lvl slice:(NSUInteger)slice withBytes:(const void *)bytes bytesPerRow:(NSUInteger)bpr bytesPerImage:(NSUInteger)bpi {
    if (!bytes) { RDNA4LOG_WARN("replaceRegion: NULL bytes; refused"); return; }
    if ([self rdna4HostReadOnly]) { RDNA4LOG_WARN("replaceRegion: read-only backing; CPU write refused"); return; }
    if (!rdna4_region_ok(self, r, lvl, slice, "replaceRegion")) return;
    BOOL bc = rdna4_fmt_is_bc(_fmt);
    NSUInteger bw = bc ? (r.size.width + 3) / 4 : r.size.width;
    NSUInteger bh = bc ? (r.size.height + 3) / 4 : r.size.height;
    NSUInteger rowBytes = 0;
    if (__builtin_mul_overflow(bw, (NSUInteger)_fmt->bpp, &rowBytes)) return;
    if ((_is1D || _texel) && bpr < rowBytes) bpr = rowBytes;
    rdna4_texture_transfer transfer;
    if (!rdna4_texture_transfer_layout(bw, bh, r.size.depth,
            _fmt->bpp, bpr, bpi, &transfer) ||
        !rdna4_texture_transfer_pointer(bytes, transfer.span)) {
        RDNA4LOG_WARN("replaceRegion: CPU row/image layout is too small or overflows; refused"); return;
    }
    if (_texel) {
        if (![_hostBuf contents]) { RDNA4LOG_WARN("replaceRegion: Private texture buffer; refused"); return; }
        memcpy((uint8_t *)[_hostBuf contents] + _hoff + r.origin.x * _fmt->bpp, bytes, rowBytes);
        return;
    }
    if ([self rdna4IsIOS]) {   // 11h.6: the IOSurface (or no-copy buffer) memory IS the CPU copy; the GPU copy (path b) is re-uploaded at the first use in each command buffer
        if ([self rdna4VramSurfCached]) {
            const uint64_t vsId = [self rdna4VramSurfId];
            const BOOL fullRegion = (r.origin.x == 0 && r.origin.y == 0 && r.size.width == _w && r.size.height == _h);
            if (!fullRegion) (void)rdna4_vs_flush_entry_sync(vsId);
            else rdna4_vs_wait_flush_done(vsId);
        }
        uint8_t *base = (uint8_t *)[self rdna4IBase]; size_t stride = [self rdna4IOSBytesPerRow];
        for (NSUInteger y = 0; y < r.size.height; y++) memcpy(base + (r.origin.y + y) * stride + r.origin.x * _fmt->bpp, (const uint8_t *)bytes + y * bpr, rowBytes);
        if ([self rdna4VramSurfCached]) rdna4_vs_invalidate_cpu([self rdna4VramSurfId]);
        [self rdna4PathBInvalidate];
        RDNA4LOGR("replaceRegion: %lux%lu into the IOSurface memory", (unsigned long)r.size.width, (unsigned long)r.size.height); return;
    }
    if ([self storageMode] == MTLStorageModePrivate) { RDNA4LOG_WARN("replaceRegion: texture %p is Private: CPU access refused (no NSError channel; no-op)", (__bridge void *)self); return; }
    NSUInteger nz = r.size.depth;
    NSError *err = nil;
    NSUInteger opts = MTLResourceStorageModeShared | RDNA4_OPT_NOZERO;
    RDNA4Buffer *stg = [[RDNA4Buffer alloc] initWithDevice:_dev length:transfer.packed_bytes options:opts error:&err];
    if (!stg) { RDNA4LOG_ERR("replaceRegion: staging buffer failed: %s", err.localizedDescription.UTF8String); return; }
    for (NSUInteger z = 0; z < nz; z++) for (NSUInteger y = 0; y < bh; y++)
        memcpy((uint8_t *)[stg contents] + (z * bh + y) * rowBytes, (const uint8_t *)bytes + z * bpi + y * bpr, rowBytes);
    BOOL ok = rdna4_oneshot(&err, ^(VkCommandBuffer cmd) {
        rdna4_tex_to(cmd, self, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy bic = { .bufferOffset = [stg rdna4VkOffset], .bufferRowLength = 0, .bufferImageHeight = 0,
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, [self rdna4MipLevel:lvl], [self rdna4Slice:slice], 1 }, .imageOffset = { (int32_t)r.origin.x, (int32_t)r.origin.y, (int32_t)r.origin.z },
            .imageExtent = { (uint32_t)r.size.width, (uint32_t)r.size.height, (uint32_t)nz } };
        vkCmdCopyBufferToImage(cmd, [stg vkBuffer], [self vkImage], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
        rdna4_tex_to(cmd, self, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }, NULL);
    RDNA4LOGR("replaceRegion: %lux%lux%lu at %lu,%lu,%lu level %lu (%s, storageMode %lu): %s", (unsigned long)r.size.width, (unsigned long)r.size.height, (unsigned long)nz, (unsigned long)r.origin.x,
           (unsigned long)r.origin.y, (unsigned long)r.origin.z, (unsigned long)lvl, ok ? "uploaded" : "FAILED", (unsigned long)[self storageMode], ok ? "ok" : err.localizedDescription.UTF8String);
}
- (void)replaceRegion:(MTLRegion)r mipmapLevel:(NSUInteger)lvl withBytes:(const void *)bytes bytesPerRow:(NSUInteger)bpr {
    NSUInteger bpi = 0;
    NSUInteger bh = rdna4_fmt_is_bc(_fmt) ? (r.size.height + 3) / 4 : r.size.height;
    if (r.size.depth > 1 && __builtin_mul_overflow(bpr, bh, &bpi)) {
        RDNA4LOG_WARN("replaceRegion: image stride overflows; refused"); return;
    }
    [self replaceRegion:r mipmapLevel:lvl slice:0 withBytes:bytes bytesPerRow:bpr bytesPerImage:bpi];
}
- (void)getBytes:(void *)dst bytesPerRow:(NSUInteger)bpr bytesPerImage:(NSUInteger)bpi fromRegion:(MTLRegion)r mipmapLevel:(NSUInteger)lvl slice:(NSUInteger)slice {
    if (!dst) { RDNA4LOG_WARN("getBytes: NULL destination; refused"); return; }
    if (!rdna4_region_ok(self, r, lvl, slice, "getBytes")) return;
    BOOL bc = rdna4_fmt_is_bc(_fmt);
    NSUInteger bw = bc ? (r.size.width + 3) / 4 : r.size.width;
    NSUInteger bh = bc ? (r.size.height + 3) / 4 : r.size.height;
    NSUInteger rowBytes = 0;
    if (__builtin_mul_overflow(bw, (NSUInteger)_fmt->bpp, &rowBytes)) return;
    if ((_is1D || _texel) && bpr < rowBytes) bpr = rowBytes;
    rdna4_texture_transfer transfer;
    if (!rdna4_texture_transfer_layout(bw, bh, r.size.depth,
            _fmt->bpp, bpr, bpi, &transfer) ||
        !rdna4_texture_transfer_pointer(dst, transfer.span)) {
        RDNA4LOG_WARN("getBytes: CPU row/image layout is too small or overflows; refused"); return;
    }
    if (_texel) {
        if (![_hostBuf contents]) { RDNA4LOG_WARN("getBytes: Private texture buffer; refused"); return; }
        memcpy(dst, (const uint8_t *)[_hostBuf contents] + _hoff + r.origin.x * _fmt->bpp, rowBytes);
        return;
    }
    if ([self rdna4IsIOS]) {   // 11h.6: contents are current in the IOSurface / buffer memory once the writing command buffer has completed
        if ([self rdna4VramSurfCached]) rdna4_vs_flush_entry_sync([self rdna4VramSurfId]);
        const uint8_t *base = [self rdna4IBase]; size_t stride = [self rdna4IOSBytesPerRow];
        for (NSUInteger y = 0; y < r.size.height; y++) memcpy((uint8_t *)dst + y * bpr, base + (r.origin.y + y) * stride + r.origin.x * _fmt->bpp, rowBytes);
        return;
    }
    if ([self storageMode] == MTLStorageModePrivate) { RDNA4LOG_WARN("getBytes: texture %p is Private: CPU access refused (no NSError channel; no-op)", (__bridge void *)self); return; }
    NSUInteger nz = r.size.depth;
    NSError *err = nil;
    RDNA4Buffer *stg = [[RDNA4Buffer alloc] initWithDevice:_dev length:transfer.packed_bytes options:MTLResourceStorageModeShared error:&err];
    if (!stg) { RDNA4LOG_ERR("getBytes: staging buffer failed: %s", err.localizedDescription.UTF8String); return; }
    VkImageLayout was = [self layout];
    BOOL ok = rdna4_oneshot(&err, ^(VkCommandBuffer cmd) {
        rdna4_tex_to(cmd, self, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy bic = { .bufferOffset = [stg rdna4VkOffset], .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, [self rdna4MipLevel:lvl], [self rdna4Slice:slice], 1 }, .imageOffset = { (int32_t)r.origin.x, (int32_t)r.origin.y, (int32_t)r.origin.z },
            .imageExtent = { (uint32_t)r.size.width, (uint32_t)r.size.height, (uint32_t)nz } };
        vkCmdCopyImageToBuffer(cmd, [self vkImage], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, [stg vkBuffer], 1, &bic);
        VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        if (was != VK_IMAGE_LAYOUT_UNDEFINED) rdna4_tex_to(cmd, self, was);
    }, NULL);
    if (ok) for (NSUInteger z = 0; z < nz; z++) for (NSUInteger y = 0; y < bh; y++)
        memcpy((uint8_t *)dst + z * bpi + y * bpr, (const uint8_t *)[stg contents] + (z * bh + y) * rowBytes, rowBytes);
    RDNA4LOGR("getBytes: %lux%lux%lu level %lu: %s", (unsigned long)r.size.width, (unsigned long)r.size.height, (unsigned long)nz, (unsigned long)lvl, ok ? "ok" : err.localizedDescription.UTF8String);
}
- (void)getBytes:(void *)dst bytesPerRow:(NSUInteger)bpr fromRegion:(MTLRegion)r mipmapLevel:(NSUInteger)lvl {
    NSUInteger bpi = 0;
    NSUInteger bh = rdna4_fmt_is_bc(_fmt) ? (r.size.height + 3) / 4 : r.size.height;
    if (r.size.depth > 1 && __builtin_mul_overflow(bpr, bh, &bpi)) {
        RDNA4LOG_WARN("getBytes: image stride overflows; refused"); return;
    }
    [self getBytes:dst bytesPerRow:bpr bytesPerImage:bpi fromRegion:r mipmapLevel:lvl slice:0];
}
- (VkImage)vkImage { return _root ? [_root vkImage] : _img; }
- (VkImageView)vkView { return _view; }
- (uint32_t)rdna4Layers { return rdna4td_nlayers((unsigned)_layers); }
- (uint32_t)rdna4ImageLayers { return _root ? [_root rdna4ImageLayers] : rdna4td_nlayers((unsigned)_layers); }
- (uint32_t)rdna4Levels { return _root ? [_root rdna4Levels] : (uint32_t)(_levels ? _levels : 1); }
- (uint32_t)rdna4MipLevel:(NSUInteger)level { return _baseLevel + (uint32_t)level; }
- (uint32_t)rdna4Slice:(NSUInteger)slice { return rdna4td_base_layer(_is3D, (NSUInteger)_baseSlice + slice); }
- (unsigned)rdna4Aspects { return rdna4dp_barrier_aspect(_root ? [_root rdna4Aspects] : _aspects); }
- (BOOL)rdna4IsDS { return (_root ? [_root rdna4Aspects] : _aspects) != 0 && (_root ? [_root rdna4Aspects] : _aspects) != RDNA4DP_ASP_COLOR; }
- (BOOL)rdna4IsUInt { return _fmt != NULL && (_fmt->flags & RDNA4F_UINT) != 0; }
- (BOOL)rdna4IsSInt { return _fmt != NULL && (_fmt->flags & RDNA4F_SINT) != 0; }
- (unsigned)rdna4UIntBits { unsigned rgb = 16, a = 16; rdna4_int_bits(_pf, &rgb, &a); return rgb; }
- (void)rdna4IntBitsRGB:(unsigned *)rgb alpha:(unsigned *)alpha { unsigned r = 16, a = 16; rdna4_int_bits(_pf, &r, &a); if (rgb) *rgb = r; if (alpha) *alpha = a; }
- (BOOL)rdna4IsView { return _root != nil; }
- (NSUInteger)rdna4Samples { return _root ? 1 : (_samples ? _samples : 1); }
- (unsigned)rdna4CopyAspect { return [self rdna4IsDS] ? rdna4dp_copy_aspect([self rdna4Aspects]) : RDNA4DP_ASP_COLOR; }
- (VkFormat)vkFormat { return _fmt->vk; }
- (uint32_t)bytesPerPixel { return _fmt->bpp; }
- (VkImageLayout)layout { return _root ? [_root layout] : _iosLinear ? VK_IMAGE_LAYOUT_GENERAL : _vramSurfCached ? rdna4_vs_get_layout(_vramSurfId) : _layout; }
- (void)setLayout:(VkImageLayout)l { if (_root) [_root setLayout:l]; else if (_vramSurfCached) rdna4_vs_set_layout(_vramSurfId, l); else if (!_iosLinear) _layout = l; }
- (id)device { return _dev; }
- (NSString *)label { return _lbl; }
- (void)setLabel:(NSString *)l { _lbl = [l copy]; }
- (MTLTextureType)textureType { return _texel ? MTLTextureTypeTextureBuffer : (MTLTextureType)rdna4td_mtl_type(_tdk); }
- (MTLPixelFormat)pixelFormat { return _pf; }
- (NSUInteger)width { return _w; }
- (NSUInteger)height { return _h; }
- (NSUInteger)depth { return _is3D ? _depth : 1; }
- (NSUInteger)mipmapLevelCount { return _levels ? _levels : 1; }
- (NSUInteger)sampleCount { return [self rdna4Samples]; }
- (NSUInteger)arrayLength { return rdna4td_array_length(_tdk, (unsigned)_layers); }
- (NSUInteger)usage { return _usage; }
- (BOOL)isFramebufferOnly { return NO; }
- (NSUInteger)firstMipmapInTail {
    if (!_root) return 0;
    RDNA4Texture *root = [self rdna4ImageRoot];
    if (![root isSparse]) return 0;
    NSUInteger rootFirst = [root firstMipmapInTail];
    if (_baseLevel >= rootFirst) return 0;
    NSUInteger rel = rootFirst - _baseLevel;
    NSUInteger lv = [self mipmapLevelCount];
    return rel < lv ? rel : lv;
}
- (NSUInteger)tailSizeInBytes {
    if (!_root) return 0;
    RDNA4Texture *root = [self rdna4ImageRoot];
    if (![root isSparse]) return 0;
    NSUInteger rootFirst = [root firstMipmapInTail];
    return (_baseLevel + [self mipmapLevelCount] > rootFirst) ? [root tailSizeInBytes] : 0;
}
- (BOOL)isSparse { return _root ? [[self rdna4ImageRoot] isSparse] : NO; }
- (BOOL)allowGPUOptimizedContents { return YES; }
- (id)parentTexture { return _root; }
- (NSUInteger)parentRelativeLevel { return _root ? _baseLevel - _root->_baseLevel : 0; }
- (NSUInteger)parentRelativeSlice { return _parentSlice; }
- (id)buffer { return _root ? [_root buffer] : (_texelOwnedBuffer ? nil : _hostBuf); }
- (NSUInteger)bufferOffset { return _root ? [_root bufferOffset] : _hoff; }
- (NSUInteger)bufferBytesPerRow { return _root ? [_root bufferBytesPerRow] : (_hostBuf ? _ibpr : 0); }
- (IOSurfaceRef)iosurface { return _root ? [_root iosurface] : _ios; }
- (NSUInteger)iosurfacePlane { return _root ? [_root iosurfacePlane] : _iosPlane; }
- (NSUInteger)allocatedSize { return _hpHeap ? _hpSize : (_root ? [_root allocatedSize] : _size); }
- (NSUInteger)resourceOptions { return _opts; }
- (MTLStorageMode)storageMode { return (MTLStorageMode)((_opts >> 4) & 0xF); }
- (MTLCPUCacheMode)cpuCacheMode { return (MTLCPUCacheMode)(_opts & 0xF); }
- (MTLHazardTrackingMode)hazardTrackingMode { return (MTLHazardTrackingMode)((_opts >> 8) & 0x3); }
// ---- gap census: MTLResource / MTLTexture surface sent by QuartzCore, SkyLight and Metal ----
// Bridge 1 (Metal 3 Bindless): unify texture uniqueIdentifier & gpuResourceID to the ABI-10 0x5244... token when device-bound.
RDNA4_RESOURCE_SPI_CUSTOM_UID(_dev ? (rdna4_argument_resource_token_bindless(self, _dev, YES, YES) ?: rdna4_resource_uid(&_uid)) : rdna4_resource_uid(&_uid))
- (id)rootResource { return _root ? [_root rootResource] : self; }
- (BOOL)isShareable { return NO; }
- (id)newSharedTextureHandle { RDNA4_ONCE("newSharedTextureHandle: shared texture handles are not implemented; nil"); return nil; }
- (id)remoteStorageTexture { return nil; }
- (id)newRemoteTextureViewForDevice:(id)d { (void)d; return nil; }
- (MTLTextureSwizzleChannels)swizzle { return _swz; }
- (BOOL)isCompressed { return rdna4_fmt_is_bc(_fmt); }
- (BOOL)isDrawable { return NO; }
- (void)didModifyData {}
- (BOOL)canGenerateMipmapLevels { return _levels > 1; }
- (void)generateMipmapLevel:(NSUInteger)l slice:(NSUInteger)s { (void)l; (void)s; }
- (uint32_t)swizzleKey { return rdna4swz_key(rdna4_swz_from_mtl(_swz)); }
- (NSUInteger)numFaces { return 1; }
- (NSUInteger)resourceIndex { return 0; }
- (void *)virtualAddress { return [self rdna4IsIOS] ? (void *)[self rdna4IBase] : NULL; }
// Views expose mip and slice ranges of 2D, array and cube textures, with optional format reinterpretation and swizzle.
// A format change requires MTLTextureUsagePixelFormatView; host coherency tracks the shared root, not each view.
- (id)rdna4View:(MTLPixelFormat)pf type:(MTLTextureType)tt levels:(NSRange)lv slices:(NSRange)sl swizzle:(MTLTextureSwizzleChannels)sw {
    NSUInteger count = [self mipmapLevelCount];
    if (!lv.length || lv.location >= count || lv.length > count - lv.location) {
        RDNA4LOG_WARN("newTextureView: type %lu levels %lu+%lu slices %lu+%lu: mip range outside the parent; nil", (unsigned long)tt, (unsigned long)lv.location,
                      (unsigned long)lv.length, (unsigned long)sl.location, (unsigned long)sl.length); return nil; }
    NSError *e = nil; RDNA4Texture *v = [[RDNA4Texture alloc] initViewOf:self pixelFormat:pf type:tt levels:lv slices:sl swizzle:sw error:&e];
    if (!v) RDNA4LOG_WARN("newTextureView: refused: %s", e.localizedDescription.UTF8String);
    return v;
}
- (id)newTextureViewWithPixelFormat:(MTLPixelFormat)pf {
    return [self rdna4View:pf type:[self textureType] levels:NSMakeRange(0, [self mipmapLevelCount]) slices:NSMakeRange(0, [self rdna4Layers]) swizzle:(MTLTextureSwizzleChannels){ MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleAlpha }];
}
- (id)newTextureViewWithPixelFormat:(MTLPixelFormat)pf textureType:(MTLTextureType)tt levels:(NSRange)lv slices:(NSRange)sl {
    return [self rdna4View:pf type:tt levels:lv slices:sl swizzle:(MTLTextureSwizzleChannels){ MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleAlpha }];
}
- (id)newTextureViewWithPixelFormat:(MTLPixelFormat)pf textureType:(MTLTextureType)tt levels:(NSRange)lv slices:(NSRange)sl swizzle:(MTLTextureSwizzleChannels)sw {
    return [self rdna4View:pf type:tt levels:lv slices:sl swizzle:sw];
}
- (instancetype)initViewOf:(RDNA4Texture *)root pixelFormat:(MTLPixelFormat)pf type:(MTLTextureType)type
                     levels:(NSRange)levels slices:(NSRange)slices swizzle:(MTLTextureSwizzleChannels)swz error:(NSError **)err {
    self = [super init]; if (!self) return nil;
    #define VFAIL(code, ...) do { if (err) *err = rdna4_err(code, [NSString stringWithFormat:__VA_ARGS__]); return nil; } while (0)
    if (!root) VFAIL(92, @"a view needs a parent texture");
    if ([root rdna4IsTexel]) VFAIL(98, @"texture buffers do not support image views");
    if (root->_samples > 1) VFAIL(100, @"views of multisample textures are not implemented");
    rdna4td_view_t view;
    if (!rdna4td_view_map(root->_tdk, [root rdna4Layers], (unsigned long)type, (unsigned long)slices.location, (unsigned long)slices.length, &view))
        VFAIL(98, @"texture type %lu with slice range %lu+%lu is not a compatible view of type %lu", (unsigned long)root.textureType,
              (unsigned long)slices.location, (unsigned long)slices.length, (unsigned long)type);
    RDNA4Texture *imageRoot = root; while (imageRoot->_root) imageRoot = imageRoot->_root;
    if ((view.kind == RDNA4TD_K_CUBE || view.kind == RDNA4TD_K_CUBEARRAY) && !(imageRoot->_imageFlags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT))
        VFAIL(98, @"the parent image was not created as cube-compatible");
    if (view.kind == RDNA4TD_K_CUBEARRAY && !RDNA4R.cubeArray) VFAIL(98, @"RADV does not support cube-array image views");
    const RDNA4Fmt *f = rdna4_fmt(pf);
    unsigned viewAspects = root->_aspects;
    if (root->_aspects) {   // bundle 10 + Metal 2 F29: Depth32Float_Stencil8 allows X32_Stencil8 sampling views; other DS reinterpretation stays refused
        if (root->_pf == MTLPixelFormatDepth32Float_Stencil8 && pf == MTLPixelFormatX32_Stencil8) {
            f = root->_fmt;
            viewAspects = RDNA4DP_ASP_STENCIL;
        } else if (pf != root->_pf) {
            VFAIL(101, @"pixel-format views of a depth/stencil texture (%lu -> %lu) are not supported", (unsigned long)root->_pf, (unsigned long)pf);
        } else {
            f = root->_fmt;
        }
    }
    if (!f) VFAIL(94, @"view pixel format %lu not supported", (unsigned long)pf);
    if (!root->_aspects && pf != root->_pf) {
        if (rdna4_fmt_is_bc(f) != rdna4_fmt_is_bc(root->_fmt)) VFAIL(95, @"cannot reinterpret between block-compressed and uncompressed formats (%lu -> %lu)", (unsigned long)root->_pf, (unsigned long)pf);
        if (f->bpp != root->_fmt->bpp) VFAIL(95, @"view pixel format %lu has %u bytes per pixel, the texture has %u", (unsigned long)pf, f->bpp, root->_fmt->bpp);
        if (!(root->_usage & MTLTextureUsagePixelFormatView)) VFAIL(96, @"the texture was not created with MTLTextureUsagePixelFormatView");
        if (imageRoot->_vramSurfCached) {
            uint32_t rootBase = (uint32_t)imageRoot->_fmt->vk;
            const uint32_t rootFam = garnet_vs_format_family((uint32_t)imageRoot->_fmt->vk, &rootBase, NULL, NULL);
            if (!garnet_vs_format_compatible(rootFam, rootBase, (uint32_t)f->vk))
                VFAIL(96, @"view pixel format %lu is not in the cached VRAM image's mutable format list", (unsigned long)pf);
        }
    }
    _root = root; _baseLevel = [root rdna4MipLevel:levels.location]; _baseSlice = [root rdna4Slice:slices.location]; _parentSlice = (uint32_t)slices.location; _dev = root->_dev;
    _imageFlags = imageRoot->_imageFlags; _tdk = view.kind; _layers = view.layersIvar;
    _w = MAX((NSUInteger)1, root->_w >> levels.location); _h = MAX((NSUInteger)1, root->_h >> levels.location);
    _pf = pf; _fmt = f; _usage = root->_usage; _opts = root->_opts;
    rdna4swz_t composed;
    if (!rdna4swz_compose(rdna4_swz_from_mtl(root->_swz), rdna4_swz_from_mtl(swz), &composed))
        VFAIL(97, @"bad swizzle");
    const char *swzWhy = NULL;
    if (!rdna4swz_check(composed, (unsigned long)root->_usage, 0, 1, &swzWhy))
        VFAIL(97, @"%s", swzWhy);
    _swz = rdna4_swz_to_mtl(composed);
    if (root->_is3D) VFAIL(99, @"views of 3D textures are not implemented");
    VkComponentMapping vcm = rdna4_view_map(f, _swz);   // composes the client swizzle with the format's own (A8: (0,0,0,R))
    // Attachments and storage images require identity mapping, including views.
    _levels = levels.length; _depth = 1; _aspects = viewAspects; _viewSwz = !rdna4_is_identity_map(&vcm);
    VkImageViewCreateInfo vc = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = [root vkImage], .viewType = (VkImageViewType)view.viewType, .format = f->vk,
        .components = vcm, .subresourceRange = { viewAspects ? rdna4dp_view_aspect(viewAspects) : VK_IMAGE_ASPECT_COLOR_BIT,
            _baseLevel, (uint32_t)_levels, _baseSlice, rdna4td_nlayers(view.layersIvar) } };
    VkResult r = vkCreateImageView(RDNA4R.dev, &vc, NULL, &_view);
    if (r != VK_SUCCESS) VFAIL(98, @"vkCreateImageView = %d", r);
    [[self rdna4ResourceHeap] rdna4TrackResource:self];
    RDNA4LOGR("RDNA4Texture view %p of %p: pf %lu -> %lu (vk format %d -> %d), swizzle %u%u%u%u", (__bridge void *)self, (__bridge void *)root, (unsigned long)root->_pf, (unsigned long)pf, root->_fmt->vk, f->vk,
           swz.red, swz.green, swz.blue, swz.alpha);
    #undef VFAIL
    return self;
}
@end

// ---------------------------------------------------------------------------------------------------------------
// m11 H1: MTLHeap (automatic heaps). SkyLight's MetalCompositor sends -[MTLDevice newHeapWithDescriptor:] through MPSSupportsMTLDevice -> MPSDevice::MPSDevice
// (MPSCore 0x7ff810821b40: setSize: / setStorageMode: / setCpuCacheMode: / setResourceOptions: / setProtectionOptions: / newHeapWithDescriptor:, then
// -setPurgeableState:3 and -size on the heap; CONFIRMED against the carved MPSCore and the WindowServer crash report of an earlier run).
// Design: automatic heaps use a first-fit logical allocator and on-demand
// physical backing blocks; placement heaps (MTLHeapTypePlacement) bind
// resources directly at caller-specified offsets inside a contiguous
// heap-sized physical block, allowing explicit-offset aliasing. Every
// buffer/texture is still an ordinary RDNA4Buffer / RDNA4Texture, validated
// against its real VkMemoryRequirements and bufferImageGranularity. The heap's
// size is added to currentAllocatedSize at creation and its resources are not
// counted again.
// Values below were measured on an Apple-silicon Mac first (mtlprobe heap): size rounds up to 16 KiB; usedSize = sum of the requested sizes (not rounded);
// maxAvailableSizeWithAlignment = largest free gap after aligning its start; automatic heaps return nil for the offset variants; hazardTrackingMode
// Default becomes Untracked; resourceOptions = storage << 4 | cpuCache | hazard << 8; setPurgeableState: returns the previous state (initially NonVolatile).
// Bundle-specific refusals (an Apple-silicon Mac aborts in its validation layer on these, so they are PC-only tests): resource storage/cpu-cache mode different from the
// heap's, a Tracked/Untracked request that differs from the heap's, Memoryless/placement/sparse heaps.
// ---------------------------------------------------------------------------------------------------------------
GARNET_INTERNAL NSUInteger rdna4_alup(NSUInteger v, NSUInteger a) { return (NSUInteger)rdna4ha_alup(v, a); }
GARNET_INTERNAL BOOL rdna4_heap_reserve_capacity(uint64_t size, uint64_t limit) {
    uint64_t old = atomic_load_explicit(&rdna4_heap_reserved, memory_order_relaxed);
    for (;;) {
        if (old > limit || size > limit - old) return NO;
        if (atomic_compare_exchange_weak_explicit(&rdna4_heap_reserved, &old, old + size,
                                                  memory_order_acq_rel, memory_order_relaxed)) return YES;
    }
}
static uint64_t rdna4_heap_release_serial(void) {
    pthread_mutex_lock(&RDNA4P.mu);
    uint64_t serial = RDNA4HT.ready ? RDNA4HT.p.submitted : 0;
    pthread_mutex_unlock(&RDNA4P.mu);
    return serial;
}
static uint64_t rdna4_heap_completed_serial(void) {
    pthread_mutex_lock(&RDNA4P.mu);
    uint64_t serial = RDNA4HT.ready ? RDNA4HT.p.completed : 0;
    pthread_mutex_unlock(&RDNA4P.mu);
    return serial;
}
@implementation RDNA4Heap {
    id _dev; NSString *_lbl; NSUInteger _size, _purge; MTLHeapType _type; unsigned _st, _cc, _hz; RDNA4HeapAlloc _ha; rdna4hb_heap _hb;
    NSHashTable *_members;
}
- (instancetype)initWithDevice:(id)dev descriptor:(MTLHeapDescriptor *)d {
    self = [super init]; if (!self) return nil;
    NSUInteger sz = d.size; unsigned st = (unsigned)d.storageMode, cc = (unsigned)d.cpuCacheMode, hz = (unsigned)d.hazardTrackingMode;
    #define HFAIL(...) do { RDNA4LOG_WARN("newHeapWithDescriptor: NIL: " __VA_ARGS__); return nil; } while (0)
    if (d.type != MTLHeapTypeAutomatic && d.type != MTLHeapTypePlacement) HFAIL("heap type %lu: only automatic and placement heaps are supported", (unsigned long)d.type);
    if (!sz) HFAIL("size 0");
    if (st > 2) HFAIL("storage mode %u (Memoryless) not supported", st);
    if (hz > 2) HFAIL("hazard tracking mode %u", hz);
    rdna4dc_caps caps = [(GarnetDevice *)dev rdna4MemoryCaps];
    sz = (NSUInteger)rdna4dc_heap_size(sz, caps.allocation_budget);
    if (!sz) HFAIL("size %lu exceeds the rounded Vulkan heap budget %llu", (unsigned long)d.size, (unsigned long long)caps.allocation_budget);
    if (!rdna4_heap_reserve_capacity(sz, caps.allocation_budget)) HFAIL("size %lu exceeds the capacity not already promised to live heaps (budget %llu)", (unsigned long)sz, (unsigned long long)caps.allocation_budget);
    #undef HFAIL
    _dev = dev; _size = sz; _type = d.type; _st = st; _cc = cc; _hz = hz ? hz : 1; _purge = 2; rdna4ha_init(&_ha, sz); rdna4hb_init(&_hb, sz, RDNA4HB_MAX_NORMAL);
    _members = [NSHashTable hashTableWithOptions:NSPointerFunctionsWeakMemory | NSPointerFunctionsObjectPointerPersonality];
    atomic_fetch_add(&rdna4_alloc_total, (uint64_t)_size);
    RDNA4LOG("RDNA4Heap %p: %lu bytes type %lu storage %u cache %u hazard %u (reserved capacity; VkDeviceMemory created on demand)", (__bridge void *)self, (unsigned long)_size, (unsigned long)_type, _st, _cc, _hz);
    return self;
}
- (void)dealloc {
    for (size_t i = 0; i < _hb.nlease; i++) {
        void *owner = _hb.lease[i].owner;
        _hb.lease[i].owner = NULL;
        if (!owner) continue;
        id backing = (__bridge_transfer id)owner;
        if ([backing respondsToSelector:@selector(destroyHandles)]) [backing destroyHandles];
    }
    rdna4hb_reclaim block;
    while (rdna4hb_take_block(&_hb, &block)) {
        if (RDNA4R.ok && block.host_mapped && !block.owned_host && block.map && block.mem)
            vkUnmapMemory(RDNA4R.dev, (VkDeviceMemory)block.mem);
        if (RDNA4R.ok && block.mem) vkFreeMemory(RDNA4R.dev, (VkDeviceMemory)block.mem, NULL);
        if (block.owned_host) {
            if (block.map) (void)munmap(block.map, (size_t)block.size);
            @synchronized ([GarnetDevice class]) { RDNA4R.impTotal -= block.size; }
        }
    }
    rdna4hb_destroy(&_hb);
    if (_size) {
        atomic_fetch_sub(&rdna4_heap_reserved, (uint64_t)_size);
        atomic_fetch_sub(&rdna4_alloc_total, (uint64_t)_size);
    }
    rdna4ha_destroy(&_ha);
}
- (void)doesNotRecognizeSelector:(SEL)sel {
    RDNA4LOG_ERR("UNRECOGNIZED selector %s on RDNA4Heap", sel_getName(sel));
    [super doesNotRecognizeSelector:sel];
}
// Logical accounting is driven by the resource's real VkMemoryRequirements. The
// charged size is mr.size; the placement extent also includes bufferImageGranularity.
- (BOOL)rdna4ReserveRequirements:(VkMemoryRequirements)mr offset:(NSUInteger *)off id:(uint64_t *)bid {
    if (!off || !bid || !mr.size || !mr.alignment) return NO;
    NSError *e = nil;
    if (!rdna4_radv_open(&e)) { RDNA4LOG_ERR("heap resource: RADV open failed: %s", e.localizedDescription.UTF8String); return NO; }
    @synchronized (self) {
        uint64_t gran = RDNA4R.lim.bufferImageGranularity ? RDNA4R.lim.bufferImageGranularity : 1;
        uint64_t extent = rdna4_alup((NSUInteger)mr.size, (NSUInteger)gran), align = MAX((uint64_t)mr.alignment, gran);
        if (extent == UINT64_MAX || extent < mr.size || align < gran) return NO;
        rdna4dc_caps caps = rdna4_device_caps();
        if (mr.size > rdna4dc_heap_available(&caps, _ha.used)) return NO;
        uint64_t o;
        if (!rdna4ha_alloc_ext(&_ha, mr.size, extent, align, &o, bid)) return NO;
        *off = (NSUInteger)o;
        return YES;
    }
}
- (void)rdna4Free:(uint64_t)bid { @synchronized (self) { rdna4ha_free(&_ha, bid); } }
- (void)rdna4FreeBlockLocked:(rdna4hb_reclaim)block {
    if (RDNA4R.ok && block.host_mapped && !block.owned_host && block.map && block.mem)
        vkUnmapMemory(RDNA4R.dev, (VkDeviceMemory)block.mem);
    if (RDNA4R.ok && block.mem) vkFreeMemory(RDNA4R.dev, (VkDeviceMemory)block.mem, NULL);
    if (block.owned_host) {
        if (block.map) (void)munmap(block.map, (size_t)block.size);
        @synchronized ([GarnetDevice class]) { RDNA4R.impTotal -= block.size; }
    }
}
- (void)rdna4CollectLocked {
    rdna4hb_reap(&_hb, rdna4_heap_completed_serial());
    for (;;) {
        uint64_t lease = 0, block = 0; void *owner = NULL;
        if (!rdna4hb_finalize_candidate(&_hb, &lease, &block, &owner)) break;
        id backing = (__bridge_transfer id)owner;
        if ([backing respondsToSelector:@selector(destroyHandles)]) [backing destroyHandles];
        if (![backing isKindOfClass:[RDNA4HeapBacking class]] ||
            [(RDNA4HeapBacking *)backing rdna4ConsumeHandleOpen])
            rdna4hb_handle_close(&_hb, block);
        rdna4hb_finalized(&_hb, lease);
    }
    // A lease abandoned before bind has no owner, and an object that already destroyed
    // its handle may have been collected before the central reaper ran.
    for (size_t i = 0; i < _hb.nlease;) {
        rdna4hb_lease *lease = &_hb.lease[i];
        const rdna4hb_block *block = rdna4hb_find_block_const(&_hb, lease->block);
        if (lease->retired && !lease->owner && block && !block->handles) {
            uint64_t lease_id = lease->id;
            rdna4hb_finalized_retired(&_hb, lease_id);
            i = 0;
        } else i++;
    }
    rdna4hb_reclaim block;
    while (rdna4hb_take_reclaimable(&_hb, &block)) [self rdna4FreeBlockLocked:block];
}
- (void)rdna4EvictRetiredBlocksLocked {
    [self rdna4CollectLocked];
    for (;;) {
        uint64_t lease = 0, block = 0; void *owner = NULL;
        if (!rdna4hb_evict_candidate(&_hb, &lease, &block, &owner)) break;
        id backing = (__bridge_transfer id)owner;
        if ([backing respondsToSelector:@selector(destroyHandles)]) [backing destroyHandles];
        if (![backing isKindOfClass:[RDNA4HeapBacking class]] ||
            [(RDNA4HeapBacking *)backing rdna4ConsumeHandleOpen])
            rdna4hb_handle_close(&_hb, block);
        rdna4hb_finalized_retired(&_hb, lease);
    }
    [self rdna4CollectLocked];
}
- (BOOL)rdna4AcquireBackingForBuffer:(VkBuffer)buffer image:(VkImage)image requirements:(VkMemoryRequirements)mr
                           dedicated:(BOOL)dedicated hostImport:(BOOL)hostImport
                                 mem:(VkDeviceMemory *)mem offset:(uint64_t *)offset map:(uint8_t **)map
                               block:(uint64_t *)block lease:(uint64_t *)lease rangeSize:(uint64_t *)rangeSize
                               error:(NSError **)err {
    if (!mem || !offset || !map || !block || !lease || !rangeSize || (!buffer && !image) || (buffer && image) || !mr.size ||
        (hostImport && !buffer)) {
        if (err) *err = rdna4_err(22, @"invalid heap backing request");
        return NO;
    }
    @synchronized (self) {
        // A completion callback cannot reach the heap, so reap finished retirement
        // before deciding that capacity is exhausted.
        [self rdna4CollectLocked];
        // Optimal images always keep the old DEVICE_LOCAL route and use the
        // texture CPU staging path. Only Shared/Managed heap buffers are mapped.
        const BOOL hostMapped = buffer && _st < 2;
        uint64_t granularity = RDNA4R.lim.bufferImageGranularity ? RDNA4R.lim.bufferImageGranularity : 1;
        for (int attempt = 0; attempt < 5; attempt++) {
            rdna4hb_res res = {0};
            int action = rdna4hb_alloc(&_hb, mr.size, mr.alignment, granularity, mr.memoryTypeBits,
                                       hostMapped, hostImport, dedicated, NULL, &res);
            if (action == RDNA4HB_R_OK) {
                rdna4hb_block *found = rdna4hb_find_block(&_hb, res.block);
                if (!found) {
                    rdna4hb_release(&_hb, res.lease, 0);
                    rdna4hb_reap(&_hb, 0);
                    [self rdna4CollectLocked];
                    if (err) *err = rdna4_err(22, @"heap backing block disappeared");
                    return NO;
                }
                if (found->host_mapped != hostMapped || (hostImport && !found->owned_host) ||
                    (hostMapped && !hostImport && found->owned_host)) {
                    rdna4hb_release(&_hb, res.lease, 0);
                    rdna4hb_reap(&_hb, 0);
                    [self rdna4CollectLocked];
                    if (err) *err = rdna4_err(22, @"heap backing class mismatch");
                    return NO;
                }
                *mem = (VkDeviceMemory)found->mem;
                *map = (uint8_t *)found->map;
                *offset = res.offset;
                *block = res.block;
                *lease = res.lease;
                *rangeSize = res.range_size;
                return YES;
            }
            if (action == RDNA4HB_R_RETRY) { [self rdna4EvictRetiredBlocksLocked]; continue; }
            if (action != RDNA4HB_R_NEED_BLOCK) {
                if (err) *err = rdna4_err(22, [NSString stringWithFormat:@"no physical heap backing for %llu bytes (backing %llu of %llu)",
                                               (unsigned long long)mr.size, (unsigned long long)_hb.backing, (unsigned long long)_hb.size]);
                return NO;
            }

            uint64_t blockSize = res.block_size;
            if (!blockSize || blockSize > _hb.size - _hb.backing) {
                [self rdna4CollectLocked];
                blockSize = res.block_size;
                if (!blockSize || blockSize > _hb.size - _hb.backing) {
                    if (err) *err = rdna4_err(22, @"physical heap backing capacity exhausted");
                    return NO;
                }
            }

            VkDeviceMemory newMemory = VK_NULL_HANDLE;
            uint8_t *newMap = NULL;
            void *ownedHost = NULL;
            uint64_t allocationSize = blockSize;
            uint32_t memoryType = 0;
            VkMemoryHostPointerPropertiesEXT hostProps = { .sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT };
            if (hostImport) {
                uint64_t hostSpan = 0;
                ownedHost = rdna4nc_owned_allocate(blockSize, RDNA4R.hostAlign, &hostSpan);
                if (!ownedHost || !hostSpan) {
                    if (err) *err = rdna4_err(22, @"cannot reserve owned CPU memory for a heap backing block");
                    return NO;
                }
                if (hostSpan > _hb.size - _hb.backing) {
                    (void)munmap(ownedHost, (size_t)hostSpan);
                    if (err) *err = rdna4_err(22, @"aligned heap import block exceeds its remaining capacity");
                    return NO;
                }
                allocationSize = hostSpan;
                VkResult hr = rdna4_vkGetMHPP(RDNA4R.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, ownedHost, &hostProps);
                int mt = hr == VK_SUCCESS && hostProps.memoryTypeBits
                    ? rdna4_find_mem(hostProps.memoryTypeBits & mr.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                    : -1;
                if (mt < 0) {
                    (void)munmap(ownedHost, (size_t)hostSpan);
                    if (err) *err = rdna4_err(22, [NSString stringWithFormat:@"host import has no coherent heap memory type (vk %d, bits 0x%x / 0x%x)",
                                                   hr, hostProps.memoryTypeBits, mr.memoryTypeBits]);
                    return NO;
                }
                memoryType = (uint32_t)mt;
                newMap = ownedHost;
            } else {
                int mt = rdna4_find_mem(mr.memoryTypeBits,
                    hostMapped ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                               : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
                if (mt < 0) {
                    if (err) *err = rdna4_err(22, hostMapped ? @"no host-visible coherent memory type for the heap block"
                                                             : @"no device-local memory type for the heap block");
                    return NO;
                }
                memoryType = (uint32_t)mt;
            }

            VkMemoryDedicatedAllocateInfo dedicatedInfo = { .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
                .buffer = buffer, .image = image };
            VkImportMemoryHostPointerInfoEXT importInfo = { .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, .pHostPointer = ownedHost };
            VkMemoryAllocateFlagsInfo flags = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
                .flags = RDNA4R.bufferAddress ? VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT : 0 };
            if (dedicated) {
                dedicatedInfo.pNext = hostImport ? &importInfo : NULL;
                flags.pNext = &dedicatedInfo;
            } else if (hostImport) {
                flags.pNext = &importInfo;
            }
            VkMemoryAllocateInfo allocation = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &flags,
                .allocationSize = allocationSize, .memoryTypeIndex = memoryType };
            VkResult vr = vkAllocateMemory(RDNA4R.dev, &allocation, NULL, &newMemory);
            if (vr != VK_SUCCESS) {
                if (ownedHost) (void)munmap(ownedHost, (size_t)allocationSize);
                if (err) *err = rdna4_err(22, [NSString stringWithFormat:@"vkAllocateMemory(heap block %llu) = %d",
                                               (unsigned long long)allocationSize, vr]);
                return NO;
            }
            if (hostMapped && !hostImport) {
                vr = vkMapMemory(RDNA4R.dev, newMemory, 0, VK_WHOLE_SIZE, 0, (void **)&newMap);
                if (vr != VK_SUCCESS) {
                    vkFreeMemory(RDNA4R.dev, newMemory, NULL);
                    if (err) *err = rdna4_err(24, [NSString stringWithFormat:@"vkMapMemory(heap block) = %d", vr]);
                    return NO;
                }
            }
            uint64_t newBlock = 0;
            if (!rdna4hb_add_block(&_hb, allocationSize, memoryType, hostMapped, dedicated,
                                   res.exclusive, hostImport, newMemory, newMap, &newBlock)) {
                if (hostMapped && !hostImport) vkUnmapMemory(RDNA4R.dev, newMemory);
                vkFreeMemory(RDNA4R.dev, newMemory, NULL);
                if (ownedHost) (void)munmap(ownedHost, (size_t)allocationSize);
                if (err) *err = rdna4_err(22, @"heap backing metadata rejected the Vulkan allocation");
                return NO;
            }
            if (hostImport) @synchronized ([GarnetDevice class]) { RDNA4R.impTotal += allocationSize; }
        }
        if (err) *err = rdna4_err(22, @"heap backing allocator did not converge");
        return NO;
    }
}
- (BOOL)rdna4AcquirePlacementBackingForBuffer:(VkBuffer)buffer image:(VkImage)image requirements:(VkMemoryRequirements)mr
                                       offset:(NSUInteger)placeOffset hostImport:(BOOL)hostImport
                            requiresDedicated:(BOOL)requiresDedicated
                                          mem:(VkDeviceMemory *)mem offset:(uint64_t *)offset map:(uint8_t **)map
                                        block:(uint64_t *)block lease:(uint64_t *)lease rangeSize:(uint64_t *)rangeSize
                                        error:(NSError **)err {
    if (!mem || !offset || !map || !block || !lease || !rangeSize || (!buffer && !image) || (buffer && image) || !mr.size ||
        (hostImport && !buffer) || _type != MTLHeapTypePlacement) {
        if (err) *err = rdna4_err(22, @"invalid placement heap backing request");
        return NO;
    }
    @synchronized (self) {
        [self rdna4CollectLocked];
        const BOOL needHostMap = buffer && _st < 2;
        uint64_t granularity = RDNA4R.lim.bufferImageGranularity ? RDNA4R.lim.bufferImageGranularity : 1;
        for (int attempt = 0; attempt < 2; attempt++) {
            rdna4hb_res res = {0};
            int action = rdna4hb_place(&_hb, (uint64_t)placeOffset, mr.size, mr.alignment, granularity,
                                       mr.memoryTypeBits, needHostMap ? 1 : 0, hostImport ? 1 : 0,
                                       requiresDedicated ? 1 : 0, &res);
            if (action == RDNA4HB_R_OK) {
                rdna4hb_block *found = rdna4hb_find_block(&_hb, res.block);
                if (!found || (needHostMap && !found->host_mapped) || (hostImport ? !found->owned_host : found->owned_host)) {
                    rdna4hb_release(&_hb, res.lease, 0);
                    rdna4hb_reap(&_hb, 0);
                    if (err) *err = rdna4_err(22, @"placement heap backing class mismatch");
                    return NO;
                }
                *mem = (VkDeviceMemory)found->mem;
                *map = needHostMap ? (uint8_t *)found->map : NULL;
                *offset = res.offset;
                *block = res.block;
                *lease = res.lease;
                *rangeSize = res.range_size;
                return YES;
            }
            if (action != RDNA4HB_R_NEED_BLOCK) {
                if (err) *err = rdna4_err(22, action == RDNA4HB_R_INVALID
                    ? [NSString stringWithFormat:@"placement offset %lu violates alignment %llu / granularity %llu or dedicated requirements",
                       (unsigned long)placeOffset, (unsigned long long)mr.alignment, (unsigned long long)granularity]
                    : [NSString stringWithFormat:@"placement range [%lu, +%llu) exceeds heap size %llu or incompatible backing",
                       (unsigned long)placeOffset, (unsigned long long)mr.size, (unsigned long long)_hb.size]);
                return NO;
            }

            uint64_t blockSize = res.block_size;
            VkDeviceMemory newMemory = VK_NULL_HANDLE;
            uint8_t *newMap = NULL;
            void *ownedHost = NULL;
            uint64_t allocationSize = blockSize;
            uint32_t memoryType = 0;
            int fallbackMt = -1;
            BOOL blockHostMapped = NO;
            VkMemoryHostPointerPropertiesEXT hostProps = { .sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT };
            if (hostImport) {
                uint64_t hostSpan = 0;
                ownedHost = rdna4nc_owned_allocate(blockSize, RDNA4R.hostAlign, &hostSpan);
                if (!ownedHost || !hostSpan || hostSpan > _hb.size - _hb.backing) {
                    if (ownedHost && hostSpan) (void)munmap(ownedHost, (size_t)hostSpan);
                    if (err) *err = rdna4_err(22, @"cannot reserve owned CPU memory for placement heap backing");
                    return NO;
                }
                allocationSize = hostSpan;
                VkResult hr = rdna4_vkGetMHPP(RDNA4R.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, ownedHost, &hostProps);
                int mt = hr == VK_SUCCESS && hostProps.memoryTypeBits
                    ? rdna4_find_mem(hostProps.memoryTypeBits & mr.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                    : -1;
                if (mt < 0) {
                    (void)munmap(ownedHost, (size_t)hostSpan);
                    if (err) *err = rdna4_err(22, @"host import has no coherent memory type for placement heap");
                    return NO;
                }
                memoryType = (uint32_t)mt;
                newMap = ownedHost;
                blockHostMapped = YES;
            } else if (_st < 2) {
                int mt = rdna4_find_mem(mr.memoryTypeBits,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
                int hostMt = rdna4_find_mem(mr.memoryTypeBits,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
                if (mt >= 0 && hostMt >= 0 && hostMt != mt) fallbackMt = hostMt;
                if (mt < 0) mt = hostMt;
                blockHostMapped = mt >= 0;
                if (mt < 0 && !needHostMap) mt = rdna4_find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
                if (mt < 0) {
                    if (err) *err = rdna4_err(22, @"no compatible host-visible or device-local memory type for placement heap");
                    return NO;
                }
                memoryType = (uint32_t)mt;
            } else {
                int mt = rdna4_find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
                if (mt < 0) {
                    if (err) *err = rdna4_err(22, @"no device-local memory type for placement heap");
                    return NO;
                }
                memoryType = (uint32_t)mt;
            }

            VkImportMemoryHostPointerInfoEXT importInfo = { .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, .pHostPointer = ownedHost };
            VkMemoryAllocateFlagsInfo flags = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
                .pNext = hostImport ? &importInfo : NULL,
                .flags = RDNA4R.bufferAddress ? VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT : 0 };
            VkMemoryAllocateInfo allocation = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &flags,
                .allocationSize = allocationSize, .memoryTypeIndex = memoryType };
            VkResult vr = vkAllocateMemory(RDNA4R.dev, &allocation, NULL, &newMemory);
            if (vr != VK_SUCCESS && fallbackMt >= 0) {
                memoryType = (uint32_t)fallbackMt;
                allocation.memoryTypeIndex = memoryType;
                vr = vkAllocateMemory(RDNA4R.dev, &allocation, NULL, &newMemory);
            }
            if (vr != VK_SUCCESS) {
                if (ownedHost) (void)munmap(ownedHost, (size_t)allocationSize);
                if (err) *err = rdna4_err(22, [NSString stringWithFormat:@"vkAllocateMemory(placement heap %llu) = %d",
                                               (unsigned long long)allocationSize, vr]);
                return NO;
            }
            if (blockHostMapped && !hostImport) {
                vr = vkMapMemory(RDNA4R.dev, newMemory, 0, VK_WHOLE_SIZE, 0, (void **)&newMap);
                if (vr != VK_SUCCESS) {
                    vkFreeMemory(RDNA4R.dev, newMemory, NULL);
                    if (err) *err = rdna4_err(24, [NSString stringWithFormat:@"vkMapMemory(placement heap) = %d", vr]);
                    return NO;
                }
            }
            uint64_t newBlock = 0;
            if (!rdna4hb_add_placement_block(&_hb, allocationSize, memoryType, blockHostMapped ? 1 : 0,
                                             hostImport ? 1 : 0, newMemory, newMap, &newBlock)) {
                if (blockHostMapped && !hostImport) vkUnmapMemory(RDNA4R.dev, newMemory);
                vkFreeMemory(RDNA4R.dev, newMemory, NULL);
                if (ownedHost) (void)munmap(ownedHost, (size_t)allocationSize);
                if (err) *err = rdna4_err(22, @"placement heap metadata rejected the Vulkan allocation");
                return NO;
            }
            if (hostImport) @synchronized ([GarnetDevice class]) { RDNA4R.impTotal += allocationSize; }
        }
        if (err) *err = rdna4_err(22, @"placement heap backing allocator did not converge");
        return NO;
    }
}
- (BOOL)rdna4BindSucceeded:(uint64_t)block lease:(uint64_t)lease owner:(id)owner {
    @synchronized (self) {
        if (!rdna4hb_handle_open(&_hb, block)) {
            RDNA4LOG_ERR("heap: failed to retain backing block %llu", (unsigned long long)block);
            return NO;
        }
        rdna4hb_lease *found = rdna4hb_find_lease(&_hb, lease);
        if (!found || found->block != block) {
            rdna4hb_handle_close(&_hb, block);
            RDNA4LOG_ERR("heap: backing lease %llu disappeared before bind", (unsigned long long)lease);
            return NO;
        }
        if (found->placement) {
            if ([owner isKindOfClass:[RDNA4HeapBacking class]])
                [(RDNA4HeapBacking *)owner rdna4MarkHandleOpen];
            return YES;
        }
        if (!owner) {
            if (!found->dedicated) return YES;
            rdna4hb_handle_close(&_hb, block);
            RDNA4LOG_ERR("heap: dedicated block %llu has no lifecycle owner", (unsigned long long)block);
            return NO;
        }
        void *retained = (__bridge_retained void *)owner;
        if (!rdna4hb_lease_owner(&_hb, lease, retained)) {
            id released = (__bridge_transfer id)retained;
            (void)released;
            rdna4hb_handle_close(&_hb, block);
            RDNA4LOG_ERR("heap: failed to attach the lifecycle owner to lease %llu", (unsigned long long)lease);
            return NO;
        }
        if ([owner isKindOfClass:[RDNA4HeapBacking class]])
            [(RDNA4HeapBacking *)owner rdna4MarkHandleOpen];
        return YES;
    }
}
- (void)rdna4AbandonBacking:(uint64_t)lease {
    if (!lease) return;
    @synchronized (self) {
        rdna4hb_lease *found = rdna4hb_find_lease(&_hb, lease);
        if (found && found->owner) {
            void *owner = found->owner;
            found->owner = NULL;
            id retained = (__bridge_transfer id)owner;
            if ([retained isKindOfClass:[RDNA4HeapBacking class]])
                (void)[(RDNA4HeapBacking *)retained rdna4ConsumeHandleOpen];
            if ([retained respondsToSelector:@selector(destroyHandles)]) [retained destroyHandles];
        }
        rdna4hb_release(&_hb, lease, 0);
        rdna4hb_reap(&_hb, 0);
        [self rdna4CollectLocked];
    }
}
- (void)rdna4CloseBackingHandle:(uint64_t)block {
    @synchronized (self) {
        rdna4hb_handle_close(&_hb, block);
        [self rdna4CollectLocked];
    }
}
- (void)rdna4DetachBacking:(RDNA4HeapBacking *)backing block:(uint64_t)block lease:(uint64_t)lease {
    @synchronized (self) {
        rdna4hb_lease *found = rdna4hb_find_lease(&_hb, lease);
        BOOL deferred = found && found->dedicated && found->owner == (__bridge void *)backing;
        if (deferred) {
            // The lease already retains the handle owner. The collector destroys
            // the Vulkan handles and closes the block reference only after the
            // retirement serial clears.
            if (!found->retired) rdna4hb_release(&_hb, lease, rdna4_heap_release_serial());
            [self rdna4CollectLocked];
            return;
        }
        [backing destroyHandles];
        BOOL shouldClose = !backing || [backing rdna4ConsumeHandleOpen];
        if (block && shouldClose) rdna4hb_handle_close(&_hb, block);
        found = rdna4hb_find_lease(&_hb, lease);
        void *owner = found ? found->owner : NULL;
        if (owner && rdna4hb_lease_owner_clear(&_hb, lease, (__bridge void *)backing)) {
            id retained = (__bridge_transfer id)owner;
            if ([retained respondsToSelector:@selector(destroyHandles)]) [retained destroyHandles];
            found = rdna4hb_find_lease(&_hb, lease);
            if (found && found->retired && !found->dedicated)
                rdna4hb_finalized_retired(&_hb, lease);
        }
        [self rdna4CollectLocked];
    }
}
- (void)rdna4RetireResource:(id)resource logical:(uint64_t)bid lease:(uint64_t)lease {
    (void)resource;
    @synchronized (self) {
        if (bid) rdna4ha_free(&_ha, bid);
        if (lease) rdna4hb_release(&_hb, lease, rdna4_heap_release_serial());
        [self rdna4CollectLocked];
    }
}
- (VkDeviceMemory)rdna4BackingMemory:(uint64_t)block {
    @synchronized (self) {
        const rdna4hb_block *found = rdna4hb_find_block_const(&_hb, block);
        return found ? (VkDeviceMemory)found->mem : VK_NULL_HANDLE;
    }
}
- (uint8_t *)rdna4BackingMap:(uint64_t)block {
    @synchronized (self) {
        const rdna4hb_block *found = rdna4hb_find_block_const(&_hb, block);
        return found ? (uint8_t *)found->map : NULL;
    }
}
- (BOOL)rdna4Block:(uint64_t)block offset:(uint64_t)offset size:(uint64_t)size aliases:(id)other {
    if (!block || !size || !other || ![other respondsToSelector:@selector(rdna4BackingBlock)] ||
        ![other respondsToSelector:@selector(rdna4BackingOffset)] || ![other respondsToSelector:@selector(rdna4BackingSize)] ||
        ![other respondsToSelector:@selector(heap)] || [other heap] != self) return NO;
    uint64_t otherBlock = [other rdna4BackingBlock], otherOffset = [other rdna4BackingOffset], otherSize = [other rdna4BackingSize];
    if (otherBlock != block || !otherSize) return NO;
    return offset < otherOffset ? otherOffset - offset < size : offset - otherOffset < otherSize;
}
- (uint64_t)rdna4BackingBytes { @synchronized (self) { return _hb.backing; } }
- (uint64_t)rdna4BackingInUse { @synchronized (self) { return rdna4hb_live_bytes(&_hb); } }
- (void)rdna4Reap { @synchronized (self) { [self rdna4CollectLocked]; } }
- (BOOL)rdna4OptionsOK:(NSUInteger)o what:(const char *)what {
    unsigned st = (unsigned)((o >> 4) & 0xF), cc = (unsigned)(o & 0xF), hz = (unsigned)((o >> 8) & 3);
    if (st != _st || cc != _cc || (hz && hz != _hz)) {
        RDNA4LOG_WARN("heap %s NIL: requested storage %u cache %u hazard %u does not match the heap's %u / %u / %u (Apple's validation layer aborts on this)", what, st, cc, hz, _st, _cc, _hz); return NO; }
    return YES;
}
- (id)newBufferWithLength:(NSUInteger)len options:(MTLResourceOptions)o {
    if (_type != MTLHeapTypeAutomatic) { RDNA4_ONCE("heap newBufferWithLength:options: on a placement heap: nil (use offset:)"); return nil; }
    if (!len || len > _size || ![self rdna4OptionsOK:o what:"newBufferWithLength:options:"]) { if (!len || len > _size) RDNA4LOG_WARN("heap newBufferWithLength: NIL: length %lu (heap %lu)", (unsigned long)len, (unsigned long)_size); return nil; }
    NSError *e = nil; NSUInteger ro = ((NSUInteger)_st << 4) | _cc | ((NSUInteger)_hz << 8);
    RDNA4Buffer *b = [[RDNA4Buffer alloc] initWithDevice:_dev length:len options:ro heap:self error:&e];
    if (!b) { RDNA4LOG_ERR("heap newBuffer: failed: %s", e.localizedDescription.UTF8String); return nil; }
    [self rdna4TrackResource:b];
    return b;
}
- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)td {
    if (_type != MTLHeapTypeAutomatic) { RDNA4_ONCE("heap newTextureWithDescriptor: on a placement heap: nil (use offset:)"); return nil; }
    if (!td || ![self rdna4OptionsOK:((NSUInteger)td.storageMode << 4) | (NSUInteger)td.cpuCacheMode | ((NSUInteger)td.hazardTrackingMode << 8) what:"newTextureWithDescriptor:"]) return nil;
    MTLTextureDescriptor *c = [td copy]; c.hazardTrackingMode = (MTLHazardTrackingMode)_hz;
    NSError *e = nil; RDNA4Texture *t = [[RDNA4Texture alloc] initWithDevice:_dev descriptor:c heap:self
                                                                             error:&e];
    if (!t) { RDNA4LOG_WARN("heap newTextureWithDescriptor NIL: pf %lu %lux%lu (%s)", (unsigned long)td.pixelFormat, (unsigned long)td.width, (unsigned long)td.height, e.localizedDescription.UTF8String); return nil; }
    [self rdna4TrackResource:t];
    return t;
}
// offset (placement) variants: valid only on MTLHeapTypePlacement; nil on MTLHeapTypeAutomatic.
- (id)newBufferWithLength:(NSUInteger)l options:(MTLResourceOptions)o offset:(NSUInteger)off {
    if (_type != MTLHeapTypePlacement) { RDNA4_ONCE("heap newBufferWithLength:options:offset: on an automatic heap: nil (as an Apple-silicon Mac)"); return nil; }
    if (!l || off >= _size || l > _size - off || ![self rdna4OptionsOK:o what:"newBufferWithLength:options:offset:"]) {
        if (!l || off >= _size || l > _size - off) RDNA4LOG_WARN("placement heap newBuffer: NIL: length %lu offset %lu (heap %lu)", (unsigned long)l, (unsigned long)off, (unsigned long)_size);
        return nil;
    }
    NSError *e = nil; NSUInteger ro = ((NSUInteger)_st << 4) | _cc | ((NSUInteger)_hz << 8);
    RDNA4Buffer *b = [[RDNA4Buffer alloc] initWithDevice:_dev length:l options:ro heap:self offset:off error:&e];
    if (!b) { RDNA4LOG_ERR("placement heap newBuffer: failed: %s", e.localizedDescription.UTF8String); return nil; }
    [self rdna4TrackResource:b];
    return b;
}
- (id)newBufferWithLength:(NSUInteger)l options:(MTLResourceOptions)o atOffset:(NSUInteger)off { return [self newBufferWithLength:l options:o offset:off]; }
- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)off {
    if (_type != MTLHeapTypePlacement) { RDNA4_ONCE("heap newTextureWithDescriptor:offset: on an automatic heap: nil (as an Apple-silicon Mac)"); return nil; }
    if (!d || off >= _size || ![self rdna4OptionsOK:((NSUInteger)d.storageMode << 4) | (NSUInteger)d.cpuCacheMode | ((NSUInteger)d.hazardTrackingMode << 8) what:"newTextureWithDescriptor:offset:"]) {
        if (!d || off >= _size) RDNA4LOG_WARN("placement heap newTexture: NIL: offset %lu (heap %lu)", (unsigned long)off, (unsigned long)_size);
        return nil;
    }
    MTLTextureDescriptor *c = [d copy]; c.hazardTrackingMode = (MTLHazardTrackingMode)_hz;
    NSError *e = nil; RDNA4Texture *t = [[RDNA4Texture alloc] initWithDevice:_dev descriptor:c heap:self offset:off error:&e];
    if (!t) { RDNA4LOG_WARN("placement heap newTexture NIL: pf %lu %lux%lu offset %lu (%s)", (unsigned long)d.pixelFormat, (unsigned long)d.width, (unsigned long)d.height, (unsigned long)off, e.localizedDescription.UTF8String); return nil; }
    [self rdna4TrackResource:t];
    return t;
}
- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)d atOffset:(NSUInteger)off { return [self newTextureWithDescriptor:d offset:off]; }
- (id<MTLAccelerationStructure>)newAccelerationStructureWithSize:(NSUInteger)size {
    return rdna4_rt_heap_new_as_with_size(self, _dev, size, NO, 0);
}
- (id<MTLAccelerationStructure>)newAccelerationStructureWithDescriptor:(MTLAccelerationStructureDescriptor *)d {
    return rdna4_rt_heap_new_as_with_descriptor(self, _dev, d, NO, 0);
}
- (id<MTLAccelerationStructure>)newAccelerationStructureWithSize:(NSUInteger)size offset:(NSUInteger)off {
    return rdna4_rt_heap_new_as_with_size(self, _dev, size, YES, off);
}
- (id<MTLAccelerationStructure>)newAccelerationStructureWithSize:(NSUInteger)size atOffset:(NSUInteger)off {
    return rdna4_rt_heap_new_as_with_size(self, _dev, size, YES, off);
}
- (id<MTLAccelerationStructure>)newAccelerationStructureWithDescriptor:(MTLAccelerationStructureDescriptor *)d offset:(NSUInteger)off {
    return rdna4_rt_heap_new_as_with_descriptor(self, _dev, d, YES, off);
}
- (id<MTLAccelerationStructure>)newAccelerationStructureWithDescriptor:(MTLAccelerationStructureDescriptor *)d atOffset:(NSUInteger)off {
    return rdna4_rt_heap_new_as_with_descriptor(self, _dev, d, YES, off);
}
- (NSUInteger)maxAvailableSizeWithAlignment:(NSUInteger)al {
    @synchronized (self) {
        // Query correctness is part of the allocation contract: retire cleared
        // ranges and central-block reclamation first, then bound the logical
        // answer by the physical ranges that could actually satisfy a request.
        [self rdna4CollectLocked];
        rdna4dc_caps caps = rdna4_device_caps();
        uint64_t granularity = RDNA4R.lim.bufferImageGranularity ? RDNA4R.lim.bufferImageGranularity : 1;
        if (_type == MTLHeapTypePlacement) {
            uint64_t avail = rdna4hb_placement_max_available(&_hb, al, granularity);
            uint64_t budget = rdna4dc_heap_available(&caps, rdna4hb_placement_used(&_hb));
            return (NSUInteger)rdna4dc_min(avail, budget);
        }
        uint64_t logical = rdna4ha_maxavail(&_ha, al);
        uint64_t budget = rdna4dc_heap_available(&caps, _ha.used);
        // A Shared/Managed heap has two backing classes: host-mapped buffers and
        // DEVICE_LOCAL optimal images. The query has no resource kind, so report
        // the largest range that either class can satisfy.
        uint64_t physical = rdna4hb_max_available(&_hb, al, granularity, 0);
        if (_st < 2) physical = MAX(physical, rdna4hb_max_available(&_hb, al, granularity, 1));
        return (NSUInteger)rdna4dc_min(rdna4dc_min(logical, budget), physical);
    }
}
- (NSUInteger)setPurgeableState:(NSUInteger)st {
    @synchronized (self) { NSUInteger prev = _purge; if (st >= 2 && st <= 4) _purge = st; RDNA4_ONCE("heap setPurgeableState: tracked as a flag only (no memory is ever reclaimed)"); return prev; }
}
- (void)rdna4TrackResource:(id)resource { @synchronized (self) { if (resource) [_members addObject:resource]; } }
- (NSArray *)rdna4LiveResources { @synchronized (self) { return _members.allObjects; } }
- (id)device { return _dev; }
- (NSString *)label { return _lbl; }
- (void)setLabel:(NSString *)l { _lbl = [l copy]; }
- (NSUInteger)size { return _size; }
- (NSUInteger)usedSize { @synchronized (self) { return _type == MTLHeapTypePlacement ? (NSUInteger)rdna4hb_placement_used(&_hb) : (NSUInteger)_ha.used; } }
- (NSUInteger)currentAllocatedSize { return _size; }
- (NSUInteger)allocatedSize { return _size; }
- (MTLStorageMode)storageMode { return (MTLStorageMode)_st; }
- (MTLCPUCacheMode)cpuCacheMode { return (MTLCPUCacheMode)_cc; }
- (MTLHazardTrackingMode)hazardTrackingMode { return (MTLHazardTrackingMode)_hz; }
- (NSUInteger)resourceOptions { return ((NSUInteger)_st << 4) | _cc | ((NSUInteger)_hz << 8); }
- (NSUInteger)unfilteredResourceOptions { return [self resourceOptions]; }
- (MTLHeapType)type { return _type; }
- (NSUInteger)protectionOptions { return 0; }
- (uint64_t)gpuAddress { RDNA4_ONCE("heap gpuAddress: no GPU virtual address is exported; 0"); return 0; }
- (uint64_t)memoryPoolId { return 0; }
// -[_MTLHeap description] calls -formattedDescription: (found on an Apple-silicon Mac: it sends -usedSize and more); a plain string here, NOT [self description] (recursion)
- (NSString *)formattedDescription:(NSUInteger)indent { (void)indent; return [NSString stringWithFormat:@"<RDNA4Heap: %p> size %lu used %lu type %lu storage %u cache %u hazard %u label %@", (__bridge void *)self, (unsigned long)_size, (unsigned long)[self usedSize], (unsigned long)_type, _st, _cc, _hz, _lbl]; }
@end

// ---------------------------------------------------------------------------------------------------------------
// 11e binding model (metal2vulkan docs/REFLECTION.md, "Descriptor ABI (binding map)"; src/reflect/mod.rs constants).
// Every descriptor-backed Metal resource is a descriptor in ONE set; within the set (default ABI):
//   [[buffer(n)]]  -> binding n (0..31), storage buffer        [[texture(n)]] sampled  -> binding 32+n (32..159), sampled image
//   [[sampler(n)]] -> binding 160+n (n 0..15), sampler; a constexpr sampler ("StaticSampler") takes the first free binding in 160..191
//   writable [[texture(n)]] -> binding 480+n (480..607), storage image     synthetic (BufferAddressTable etc.) >= 640
//   compute dispatch grid -> 48-byte push-constant block (12 u32) + spec ids 0,1,2 = local size (KERNEL_LOCAL_SIZE_SPEC_IDS)
//   [[stage_in]] vertex attributes -> Vulkan vertex-input Location (= attribute index), buffer index = binding.
// Each stage is translated independently and both use set 0, so vertex buffer(0) and fragment buffer(0) would collide. Metal
// keeps the two argument tables apart, so the bundle keeps them apart too: the FRAGMENT module's DescriptorSet decorations are
// rewritten 0 -> 1 when the pipeline is built (the equivalent of TransformOptions::with_descriptor_layout(set = 1) that
// REFLECTION.md offers per independently translated stage); binding numbers inside a set are untouched. Vertex = set 0, fragment
// = set 1, compute = set 0. Static samplers and the compute dispatch contract come from spvcache/<sha>.meta.json (add-air.py).
// ---------------------------------------------------------------------------------------------------------------

// ---- samplers ----
static _Atomic uint64_t rdna4_sampler_allocations;

/*
 * actual = live VkSampler handles.
 * reserved = worst-case handles held by all live public argument configs.
 * reservedActual = live handles inside those reservations.
 * committed = actual + (reserved - reservedActual), so an admitted
 * configuration can subsequently materialize all K handles without another
 * capacity decision. Direct/static samplers consume the same actual budget.
 */
static pthread_mutex_t rdna4_sampler_budget_lock = PTHREAD_MUTEX_INITIALIZER;
static rdna4_argument_sampler_budget rdna4_sampler_budget_state;

static uint64_t rdna4_sampler_committed_locked(void) {
    uint64_t committed = UINT64_MAX;
    if (rdna4_argument_limit_snapshot_budget_committed(
            &rdna4_sampler_budget_state,
            atomic_load(&rdna4_sampler_allocations), &committed) !=
        RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK)
        return UINT64_MAX;
    return committed;
}

static BOOL rdna4_sampler_budget_charge(RDNA4SamplerStorage *storage,
                                        NSError **err) {
    pthread_mutex_lock(&rdna4_sampler_budget_lock);
    const BOOL reserved = storage && storage->argumentReserved;
    if (storage && storage->handles >=
                       RDNA4_ARGUMENT_LIMIT_DEFAULT_OBJECTS_PER_CONFIG) {
        pthread_mutex_unlock(&rdna4_sampler_budget_lock);
        if (err) *err = rdna4_err(72, @"A sampler configuration exceeded three backend handles");
        return NO;
    }

    const uint64_t actual = atomic_load(&rdna4_sampler_allocations);
    int admitted = 0;
    if (reserved) {
        if (atomic_load_explicit(&rdna4_argument_limit_full_open_state,
                                 memory_order_acquire) != 1) {
            pthread_mutex_unlock(&rdna4_sampler_budget_lock);
            if (err) *err = rdna4_err(73, @"Argument-limit source has not passed the same-handle full-open crosscheck");
            return NO;
        }
        rdna4_argument_public_sampler_status status =
            rdna4_argument_limit_snapshot_budget_charge_reserved(
                &RDNA4ALS, &rdna4_sampler_budget_state, &admitted);
        if (status != RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK || !admitted) {
            pthread_mutex_unlock(&rdna4_sampler_budget_lock);
            if (err) *err = rdna4_err(72, @"A sampler reservation was unavailable");
            return NO;
        }
    } else if (RDNA4ALS.state == RDNA4_ARGUMENT_LIMIT_STATE_READY &&
               atomic_load_explicit(&rdna4_argument_limit_full_open_state,
                                    memory_order_acquire) == 1) {
        rdna4_argument_public_sampler_status status =
            rdna4_argument_limit_snapshot_budget_can_charge_direct(
                &RDNA4ALS, &rdna4_sampler_budget_state, actual, 1,
                &admitted);
        if (status != RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK || !admitted) {
            pthread_mutex_unlock(&rdna4_sampler_budget_lock);
            if (err) *err = rdna4_err(72, @"Backend sampler allocation limit reached");
            return NO;
        }
    } else if (actual >= RDNA4R.lim.maxSamplerAllocationCount) {
        /* Tier1 fallback: an unavailable or unverified export is never used. */
        pthread_mutex_unlock(&rdna4_sampler_budget_lock);
        if (err) *err = rdna4_err(72, @"Backend sampler allocation limit reached");
        return NO;
    }

    atomic_store(&rdna4_sampler_allocations, actual + 1u);
    if (storage) ++storage->handles;
    pthread_mutex_unlock(&rdna4_sampler_budget_lock);
    return YES;
}

static void rdna4_sampler_budget_release(RDNA4SamplerStorage *storage) {
    pthread_mutex_lock(&rdna4_sampler_budget_lock);
    if (storage && storage->argumentReserved) {
        int released = 0;
        rdna4_argument_public_sampler_status status =
            rdna4_argument_limit_snapshot_budget_release_reserved(
                &RDNA4ALS, &rdna4_sampler_budget_state, &released);
        if (status != RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK || !released)
            RDNA4LOG_WARN("argument sampler budget: reserved release failed (%d)", status);
    }
    const uint64_t actual = atomic_load(&rdna4_sampler_allocations);
    if (actual) atomic_store(&rdna4_sampler_allocations, actual - 1u);
    if (storage && storage->handles) --storage->handles;
    pthread_mutex_unlock(&rdna4_sampler_budget_lock);
}

static BOOL rdna4_sampler_budget_reserve_configuration(
    RDNA4SamplerStorage *storage, uint32_t liveUniqueConfigurations,
    NSError **err) {
    if (!storage || storage->argumentReserved) return YES;
    if (RDNA4ALS.state != RDNA4_ARGUMENT_LIMIT_STATE_READY) return YES;
    if (atomic_load_explicit(&rdna4_argument_limit_full_open_state,
                             memory_order_acquire) != 1) {
        if (err) *err = rdna4_err(73, @"Argument-limit source has not passed the same-handle full-open crosscheck");
        return NO;
    }

    pthread_mutex_lock(&rdna4_sampler_budget_lock);
    int admitted = 0;
    rdna4_argument_public_sampler_status admission =
        rdna4_argument_limit_snapshot_budget_reserve_configuration(
            &RDNA4ALS, &rdna4_sampler_budget_state,
            liveUniqueConfigurations, storage->handles,
            atomic_load(&rdna4_sampler_allocations), &admitted);
    if (admission != RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK || !admitted) {
        pthread_mutex_unlock(&rdna4_sampler_budget_lock);
        if (err) *err = rdna4_err(73, admission ==
            RDNA4_ARGUMENT_PUBLIC_SAMPLER_AT_LIMIT
                ? @"maxArgumentBufferSamplerCount reached"
                : @"Backend sampler object budget reached before argument admission");
        RDNA4LOG("argument sampler admission: REJECT live unique %u, actual %llu, reserved %llu/%llu, existing handles %u, limit %u, status %d",
                 liveUniqueConfigurations,
                 (unsigned long long)atomic_load(&rdna4_sampler_allocations),
                 (unsigned long long)rdna4_sampler_budget_state.reserved_actual_objects,
                 (unsigned long long)rdna4_sampler_budget_state.reserved_objects,
                 storage->handles, RDNA4ALS.public_sampler_limit, admission);
        return NO;
    }
    storage->argumentReserved = YES;
    RDNA4LOG("argument sampler admission: ADMIT live unique %u -> %u, actual %llu, committed %llu/%u",
             liveUniqueConfigurations, liveUniqueConfigurations + 1u,
             (unsigned long long)atomic_load(&rdna4_sampler_allocations),
             (unsigned long long)rdna4_sampler_committed_locked(),
             RDNA4ALS.public_sampler_limit);
    pthread_mutex_unlock(&rdna4_sampler_budget_lock);
    return YES;
}

static void rdna4_sampler_budget_release_configuration(
    RDNA4SamplerStorage *storage) {
    if (!storage || !storage->argumentReserved) return;
    pthread_mutex_lock(&rdna4_sampler_budget_lock);
    int released = 0;
    rdna4_argument_public_sampler_status status =
        rdna4_argument_limit_snapshot_budget_release_configuration(
            &RDNA4ALS, &rdna4_sampler_budget_state, storage->handles,
            &released);
    if (status == RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK && released)
        storage->argumentReserved = NO;
    else
        RDNA4LOG_WARN("argument sampler budget: configuration release failed (%d, handles %u, reserved %llu/%llu)",
                      status, storage->handles,
                      (unsigned long long)rdna4_sampler_budget_state.reserved_actual_objects,
                      (unsigned long long)rdna4_sampler_budget_state.reserved_objects);
    pthread_mutex_unlock(&rdna4_sampler_budget_lock);
}

GARNET_INTERNAL VkSampler rdna4_make_sampler(const RDNA4SampCfg *c, BOOL comparison,
                                    BOOL normalizedCoordinates,
                                    RDNA4SamplerStorage *budgetStorage,
                                    NSError **err) {
    if (!rdna4_radv_open(err)) return VK_NULL_HANDLE;
    VkSamplerCreateInfo sc;
    const char *invalid = normalizedCoordinates
        ? rdna4_sampler_info_normalized(c, comparison, RDNA4R.samplerAnisotropy,
            RDNA4R.lim.maxSamplerAnisotropy, RDNA4R.samplerMirrorClamp, &sc)
        : rdna4_sampler_info(c, comparison, RDNA4R.samplerAnisotropy,
            RDNA4R.lim.maxSamplerAnisotropy, RDNA4R.samplerMirrorClamp, &sc);
    if (invalid) { if (err) *err = rdna4_err(71, [NSString stringWithUTF8String:invalid]); return VK_NULL_HANDLE; }
    if (!rdna4_sampler_budget_charge(budgetStorage, err))
        return VK_NULL_HANDLE;
    VkSampler s = VK_NULL_HANDLE;
    VkResult r = vkCreateSampler(RDNA4R.dev, &sc, NULL, &s);
    if (r != VK_SUCCESS) {
        rdna4_sampler_budget_release(budgetStorage);
        if (err) *err = rdna4_err(72, [NSString stringWithFormat:@"vkCreateSampler = %d", r]);
        return VK_NULL_HANDLE;
    }
    return s;
}

GARNET_INTERNAL void rdna4_destroy_sampler(VkSampler sampler,
                                  RDNA4SamplerStorage *budgetStorage) {
    if (RDNA4R.ok && sampler) {
        vkDestroySampler(RDNA4R.dev, sampler, NULL);
        rdna4_sampler_budget_release(budgetStorage);
    }
}

@implementation RDNA4SamplerStorage
- (void)dealloc {
    rdna4_destroy_sampler(normal, self);
    rdna4_destroy_sampler(comparison, self);
    rdna4_destroy_sampler(normalizedNormal, self);
}
@end

static NSMapTable<NSData *, RDNA4SamplerStorage *> *rdna4_sampler_configs;
static NSUInteger rdna4_argument_sampler_configs;

static NSData *rdna4_sampler_config_key(const RDNA4SampCfg *c) {
    uint32_t words[] = { (uint32_t)c->minF, (uint32_t)c->magF, (uint32_t)c->mipF,
        (uint32_t)c->as, (uint32_t)c->at, (uint32_t)c->ar, (uint32_t)c->border,
        (uint32_t)c->cmp, (uint32_t)c->unnorm, 0, 0, c->anisotropy };
    float low = c->lodMin == 0 ? 0 : c->lodMin, high = c->lodMax == 0 ? 0 : c->lodMax;
    memcpy(&words[9], &low, sizeof low);
    memcpy(&words[10], &high, sizeof high);
    return [NSData dataWithBytes:words length:sizeof words];
}

static void rdna4_sampler_storage_release(RDNA4SamplerStorage *storage,
                                           BOOL argumentBuffers) {
    if (!storage) return;
    @synchronized ([RDNA4SamplerStorage class]) {
        if (!storage->references) return;
        if (argumentBuffers && !--storage->argumentReferences) {
            --rdna4_argument_sampler_configs;
            rdna4_sampler_budget_release_configuration(storage);
        }
        if (!--storage->references) {
            NSData *key = rdna4_sampler_config_key(&storage->config);
            [rdna4_sampler_configs removeObjectForKey:key];
        }
    }
}

static const char *rdna4_sampler_lod_clamp_invalid(double lodMin, double lodMax) {
    if (!isfinite(lodMin) || !isfinite(lodMax) || lodMin < 0 || lodMax < lodMin)
        return "sampler LOD clamps must be finite, nonnegative and ordered";
    return NULL;
}

static VkSampler rdna4_make_sampler_bounded(const RDNA4SampCfg *config,
    BOOL comparison, BOOL normalized, RDNA4SamplerStorage *storage, NSError **err) {
    RDNA4SampCfg bounded = *config;
    // rdna4_sampler_info caps maxLod at 1000. Cap minLod identically so a
    // finite request above that backend bound still creates an ordered sampler.
    if (!bounded.unnorm && bounded.mipF != 0) {
        if (bounded.lodMin > 1000.0f) bounded.lodMin = 1000.0f;
        if (bounded.lodMax > 1000.0f) bounded.lodMax = 1000.0f;
    }
    return rdna4_make_sampler(&bounded, comparison, normalized, storage, err);
}

static const char *rdna4_sampler_descriptor_invalid(MTLSamplerDescriptor *d) {
    if (!d) return "nil sampler descriptor";
    // Check the full-width properties before any narrowing cast. A value such as
    // 0x100000000 would otherwise truncate to a legal Metal enum value.
    if ((NSUInteger)d.minFilter > 1 || (NSUInteger)d.magFilter > 1)
        return "invalid sampler min/mag filter";
    if ((NSUInteger)d.mipFilter > 2)
        return "invalid sampler mip filter";
    if ((NSUInteger)d.sAddressMode > 5 || (NSUInteger)d.tAddressMode > 5 ||
        (NSUInteger)d.rAddressMode > 5)
        return "unsupported sampler address mode";
    if ((NSUInteger)d.borderColor > 2)
        return "invalid sampler border color";
    if ((NSUInteger)d.compareFunction > 7)
        return "invalid sampler comparison function";
    if (d.maxAnisotropy < 1 || d.maxAnisotropy > 16)
        return "sampler maxAnisotropy must be in 1..16";
    return rdna4_sampler_lod_clamp_invalid(d.lodMinClamp, d.lodMaxClamp);
}

// Both public samplers and effective LOD bindings acquire one canonical config.
// Publish ownership only after admission and handle creation have succeeded.
static RDNA4SamplerStorage *rdna4_sampler_storage_acquire(
    const RDNA4SampCfg *config, BOOL argumentBuffers, NSError **err) {
    if (!rdna4_radv_open(err)) return nil;
    VkSamplerCreateInfo info;
    const char *invalid = rdna4_sampler_info(config, NO, RDNA4R.samplerAnisotropy,
        RDNA4R.lim.maxSamplerAnisotropy, RDNA4R.samplerMirrorClamp, &info);
    if (invalid) {
        if (err) *err = rdna4_err(71, [NSString stringWithUTF8String:invalid]);
        return nil;
    }
    @synchronized ([RDNA4SamplerStorage class]) {
        if (!rdna4_sampler_configs)
            rdna4_sampler_configs = [NSMapTable strongToWeakObjectsMapTable];
        NSData *key = rdna4_sampler_config_key(config);
        RDNA4SamplerStorage *storage = [rdna4_sampler_configs objectForKey:key];
        if (!storage) {
            storage = [RDNA4SamplerStorage new];
            storage->config = *config;
        }
        const BOOL firstArgumentConfiguration = argumentBuffers &&
            storage->argumentReferences == 0;
        if (firstArgumentConfiguration && RDNA4ALS.state == RDNA4_ARGUMENT_LIMIT_STATE_READY) {
            if (rdna4_argument_sampler_configs > UINT32_MAX ||
                !rdna4_sampler_budget_reserve_configuration(storage,
                    (uint32_t)rdna4_argument_sampler_configs, err)) return nil;
        }
        if (!storage->normal) {
            storage->normal = rdna4_make_sampler_bounded(config, NO, NO, storage, err);
            if (!storage->normal) {
                if (firstArgumentConfiguration)
                    rdna4_sampler_budget_release_configuration(storage);
                return nil;
            }
        }
        [rdna4_sampler_configs setObject:storage forKey:key];
        if (argumentBuffers && !storage->argumentReferences++)
            ++rdna4_argument_sampler_configs;
        ++storage->references;
        return storage;
    }
}

static VkSampler rdna4_sampler_storage_handle(RDNA4SamplerStorage *storage,
    BOOL comparison, BOOL normalized, NSError **err) {
    if (!storage) {
        if (err) *err = rdna4_err(71, @"sampler storage is unavailable");
        return VK_NULL_HANDLE;
    }
    if (!normalized) {
        if (!comparison) return storage->normal;
        @synchronized (storage) {
            // A pixel source has no legacy unnormalized comparison representation.
            if (storage->config.unnorm)
                return rdna4_make_sampler_bounded(&storage->config, YES, NO, storage, err);
            if (!storage->comparison)
                storage->comparison = rdna4_make_sampler_bounded(&storage->config, YES, NO, storage, err);
            return storage->comparison;
        }
    }
    @synchronized (storage) {
        if (!storage->config.unnorm) {
            if (!comparison) return storage->normal;
            if (!storage->comparison)
                storage->comparison = rdna4_make_sampler_bounded(&storage->config, YES, YES, storage, err);
            return storage->comparison;
        }
        if (!comparison) {
            if (!storage->normalizedNormal)
                storage->normalizedNormal = rdna4_make_sampler_bounded(&storage->config, NO, YES, storage, err);
            return storage->normalizedNormal;
        }
        if (!storage->comparison)
            storage->comparison = rdna4_make_sampler_bounded(&storage->config, YES, YES, storage, err);
        return storage->comparison;
    }
}

// A direct binding can override the sampler's LOD clamps without changing the
// public sampler/token identity. The wrapper points at either the shared base
// storage or a shared internal variant keyed by the effective sampler config.

@implementation RDNA4SamplerBinding
+ (instancetype)bindingWithBaseStorage:(RDNA4SamplerStorage *)base
                         lodMinClamp:(float)lodMin
                         lodMaxClamp:(float)lodMax
                               error:(NSError **)err {
    if (!base) {
        if (err) *err = rdna4_err(71, @"sampler binding has no base storage");
        return nil;
    }
    const char *clampInvalid = rdna4_sampler_lod_clamp_invalid(lodMin, lodMax);
    if (clampInvalid) {
        if (err) *err = rdna4_err(71, [NSString stringWithUTF8String:clampInvalid]);
        return nil;
    }
    RDNA4SampCfg config = base->config;
    config.lodMin = lodMin;
    config.lodMax = lodMax;
    BOOL override = config.lodMin != base->config.lodMin ||
        config.lodMax != base->config.lodMax;
    RDNA4SamplerStorage *storage = rdna4_sampler_storage_acquire(&config, NO, err);
    if (!storage) return nil;
    RDNA4SamplerBinding *binding = [RDNA4SamplerBinding new];
    if (!binding) {
        rdna4_sampler_storage_release(storage, NO);
        if (err) *err = rdna4_err(72, @"sampler binding allocation failed");
        return nil;
    }
    binding->_storage = storage;
    binding->_lodMinClamp = config.lodMin;
    binding->_lodMaxClamp = config.lodMax;
    binding->_lodClampOverride = override;
    return binding;
}
- (void)dealloc {
    rdna4_sampler_storage_release(_storage, NO);
}
- (RDNA4SamplerStorage *)rdna4Storage { return _storage; }
- (BOOL)rdna4HasLODClampOverride { return _lodClampOverride; }
- (float)rdna4LODMinClamp { return _lodMinClamp; }
- (float)rdna4LODMaxClamp { return _lodMaxClamp; }
- (VkSampler)vkSampler { return _storage->normal; }
- (VkSampler)rdna4SamplerComparison:(BOOL)comparison error:(NSError **)err {
    return rdna4_sampler_storage_handle(_storage, comparison, NO, err);
}
- (VkSampler)rdna4SamplerComparison:(BOOL)comparison
               normalizedCoordinates:(BOOL)normalized
                               error:(NSError **)err {
    return rdna4_sampler_storage_handle(_storage, comparison, normalized, err);
}
- (BOOL)rdna4PixelCoordinates { return _storage->config.unnorm != 0; }
@end

@implementation RDNA4SamplerState
- (instancetype)initWithDevice:(id)dev descriptor:(MTLSamplerDescriptor *)d error:(NSError **)err {
    self = [super init]; if (!self) return nil;
    _dev = dev; _lbl = [d.label copy];
    const char *descriptorInvalid = rdna4_sampler_descriptor_invalid(d);
    if (descriptorInvalid) {
        if (err) *err = rdna4_err(71, [NSString stringWithUTF8String:descriptorInvalid]);
        return nil;
    }
    RDNA4SampCfg c = { .minF = (int)d.minFilter, .magF = (int)d.magFilter, .mipF = (int)d.mipFilter, .as = (int)d.sAddressMode, .at = (int)d.tAddressMode,
        .ar = (int)d.rAddressMode, .border = (int)d.borderColor, .cmp = (int)d.compareFunction, .unnorm = !d.normalizedCoordinates,
        .lodMin = (float)d.lodMinClamp, .lodMax = (float)d.lodMaxClamp, .anisotropy = (unsigned)d.maxAnisotropy };
    _storage = rdna4_sampler_storage_acquire(&c, d.supportArgumentBuffers, err);
    if (!_storage) return nil;
    _argumentBuffers = d.supportArgumentBuffers;
    RDNA4LOGR("RDNA4SamplerState %p: vk %p (min %d mag %d mip %d addr %d/%d/%d cmp %d %s)", (__bridge void *)self, (void *)_storage->normal, c.minF, c.magF, c.mipF, c.as, c.at, c.ar, c.cmp, c.unnorm ? "unnormalized" : "normalized");
    return self;
}
- (void)dealloc {
    rdna4_sampler_storage_release(_storage, _argumentBuffers);
}
RDNA4_DNR(RDNA4SamplerState)
- (RDNA4SamplerStorage *)rdna4ArgumentIdentity { return _storage; }
- (RDNA4SamplerBinding *)rdna4Binding:(NSError **)err {
    @synchronized (self) {
        if (!_baseBinding)
            _baseBinding = [RDNA4SamplerBinding bindingWithBaseStorage:_storage
                lodMinClamp:_storage->config.lodMin lodMaxClamp:_storage->config.lodMax
                error:err];
        return _baseBinding;
    }
}
- (RDNA4SamplerBinding *)rdna4BindingWithLODMinClamp:(float)lodMin
                                          lodMaxClamp:(float)lodMax
                                                error:(NSError **)err {
    return [RDNA4SamplerBinding bindingWithBaseStorage:_storage lodMinClamp:lodMin
        lodMaxClamp:lodMax error:err];
}
- (VkSampler)vkSampler { return _storage->normal; }
- (BOOL)rdna4PixelCoordinates { return _storage->config.unnorm != 0; }
- (VkSampler)rdna4SamplerComparison:(BOOL)comparison error:(NSError **)err {
    return rdna4_sampler_storage_handle(_storage, comparison, NO, err);
}
- (VkSampler)rdna4SamplerComparison:(BOOL)comparison normalizedCoordinates:(BOOL)normalized error:(NSError **)err {
    return rdna4_sampler_storage_handle(_storage, comparison, normalized, err);
}
- (BOOL)supportArgumentBuffers { return _argumentBuffers; }
- (MTLResourceID)gpuResourceID API_AVAILABLE(macos(13.0)) {
    MTLResourceID identifier = { 0 };
    if (!_argumentBuffers) {
        RDNA4_ONCE("RDNA4SamplerState.gpuResourceID: sampler created with supportArgumentBuffers=NO; returning 0");
        return identifier;
    }
    identifier._impl = rdna4_argument_resource_token_bindless(self, _dev, NO, YES);
    return identifier;
}
- (id)device { return _dev; }
- (NSString *)label { return _lbl; }
- (void)setLabel:(NSString *)l { _lbl = [l copy]; }
@end

// ---- gap census: depth/stencil state and fences ----
// MTLDepthStencilState: SkyLight and QuartzCore create one each at start-up (-newDepthStencilStateWithDescriptor:, census rdt1), and the compositor sends
// -setDepthStencilState: on its encoders. The render passes here have colour attachments only (no depth/stencil exist), for which Metal makes the
// state a no-op, so this object records the descriptor and nothing else.
// bundle 10: the state IS used now (a pipeline with a depth/stencil attachment reads it at every draw): compare, depth write, the two stencil faces (compare, ops, masks).
GARNET_INTERNAL RDNA4DSInfo rdna4_dsinfo_default(void) { return (RDNA4DSInfo){ RDNA4DS_CMP_ALWAYS, 0, { RDNA4DS_CMP_ALWAYS, 0, 0, 0 }, { RDNA4DS_CMP_ALWAYS, 0, 0, 0 }, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu }; }
@implementation RDNA4DepthStencilState
- (instancetype)initWithDevice:(id)dev descriptor:(MTLDepthStencilDescriptor *)d {
    self = [super init]; if (!self) return nil;
    _dev = dev; _d = [d copy]; _lbl = [d.label copy];
    _info = rdna4_dsinfo_default();
    _info.cmp = (unsigned long)d.depthCompareFunction; _info.write = d.depthWriteEnabled ? 1 : 0;   // bundle 10
    MTLStencilDescriptor *fs = d.frontFaceStencil, *bs = d.backFaceStencil;   // nil = Always / Keep / masks all ones
    if (fs) { _info.f = (rdna4ds_face_in_t){ (unsigned long)fs.stencilCompareFunction, (unsigned long)fs.stencilFailureOperation, (unsigned long)fs.depthFailureOperation, (unsigned long)fs.depthStencilPassOperation }; _info.fRead = fs.readMask; _info.fWrite = fs.writeMask; }
    if (bs) { _info.b = (rdna4ds_face_in_t){ (unsigned long)bs.stencilCompareFunction, (unsigned long)bs.stencilFailureOperation, (unsigned long)bs.depthFailureOperation, (unsigned long)bs.depthStencilPassOperation }; _info.bRead = bs.readMask; _info.bWrite = bs.writeMask; }
    RDNA4LOG("RDNA4DepthStencilState %p: depth compare %lu write %d; stencil front cmp %lu ops %lu/%lu/%lu masks %x/%x, back cmp %lu ops %lu/%lu/%lu masks %x/%x (used by pipelines with a depth/stencil attachment)", (__bridge void *)self,
           _info.cmp, _info.write, _info.f.cmp, _info.f.fail, _info.f.dfail, _info.f.pass, _info.fRead, _info.fWrite, _info.b.cmp, _info.b.fail, _info.b.dfail, _info.b.pass, _info.bRead, _info.bWrite);
    return self;
}
RDNA4_DNR(RDNA4DepthStencilState)
- (MTLDepthStencilDescriptor *)rdna4Descriptor { return _d; }
- (RDNA4DSInfo)rdna4Info { return _info; }
- (id)device { return _dev; }
- (NSString *)label { return _lbl; }
- (void)setLabel:(NSString *)l { _lbl = [l copy]; }
@end

// A fence update is a checkpoint at an encoded command-buffer position. The
// producer segment's Vulkan fence completes the checkpoint; a consumer segment
// that waits on it is not submitted until then. This is deliberately stronger
// than a scoped stage dependency: the backend keeps a conservative full barrier.


@implementation RDNA4FenceCheckpoint
- (void)rdna4Ready {
    int want = RDNA4FenceCheckpointPending;
    (void)atomic_compare_exchange_strong_explicit(&state, &want, RDNA4FenceCheckpointReady,
                                                  memory_order_release, memory_order_relaxed);
}
- (void)rdna4Fail:(VkResult)result {
    atomic_store_explicit(&fault, (int)result, memory_order_relaxed);
    int current = atomic_load_explicit(&state, memory_order_relaxed);
    while (current == RDNA4FenceCheckpointPending) {
        int want = RDNA4FenceCheckpointPending;
        if (atomic_compare_exchange_weak_explicit(&state, &want, RDNA4FenceCheckpointFailed,
                                                  memory_order_release, memory_order_relaxed)) break;
        current = want;
    }
}
@end

// MTLFence identity plus generation state. A wait captures the latest update
// encoded at that point. A new fence starts at a ready generation-zero
// checkpoint, so waiting before any update is a completed no-op dependency; a
// later update creates a new pending checkpoint and never retargets that wait.
@implementation RDNA4Fence
- (instancetype)initWithDevice:(id)dev {
    self = [super init];
    if (!self) return nil;
    _dev = dev;
    pthread_mutex_init(&_mu, NULL);
    _latestCheckpoint = [RDNA4FenceCheckpoint new];
    _latestCheckpoint->generation = 0;
    [_latestCheckpoint rdna4Ready];
    return self;
}
- (void)dealloc { pthread_mutex_destroy(&_mu); }
RDNA4_DNR(RDNA4Fence)
- (RDNA4FenceCheckpoint *)rdna4UpdateCheckpointForCB:(uint64_t)cbid {
    pthread_mutex_lock(&_mu);
    if (_latestGeneration == UINT64_MAX) {
        pthread_mutex_unlock(&_mu);
        return nil;
    }
    uint64_t generation = _latestGeneration + 1;
    RDNA4FenceCheckpoint *checkpoint = [RDNA4FenceCheckpoint new];
    checkpoint->generation = generation;
    checkpoint->producerCBId = cbid;
    _latestGeneration = generation;
    _latestCheckpoint = checkpoint;
    pthread_mutex_unlock(&_mu);
    return checkpoint;
}
- (RDNA4FenceCheckpoint *)rdna4UpdateCheckpoint { return [self rdna4UpdateCheckpointForCB:0]; }
- (RDNA4FenceCheckpoint *)rdna4WaitCheckpoint {
    pthread_mutex_lock(&_mu);
    RDNA4FenceCheckpoint *checkpoint = _latestCheckpoint;
    pthread_mutex_unlock(&_mu);
    return checkpoint;
}
- (id)device { return _dev; }
- (NSString *)label { return _lbl; }
- (void)setLabel:(NSString *)l { _lbl = [l copy]; }
@end

// Indirect resources must survive command execution, even without a bound descriptor.

// ---------------------------------------------------------------------------------------------------------------
// Metal 3.2 Residency Sets (Task 4.2): RDNA4ResidencySnapshot & RDNA4ResidencySet.
//
// Wave 2B GarnetDevice.m integration:
//   1. Include order in GarnetDevice.m: place after `#include "rdna4_gl_drawable_impl.m"`
//      (after RDNA4Buffer, RDNA4Texture, RDNA4Heap, and RDNA4Residency*Internal protocols in
//      rdna4_command_queue_impl.inc are defined):
//        #include "rdna4_residency_set_impl.m"
//        #include "rdna4_io_queue_impl.m"
//   2. Device factory selector on @implementation GarnetDevice:
//        - (id)newResidencySetWithDescriptor:(id)descriptor error:(NSError **)error {
//            return [[RDNA4ResidencySet alloc] initWithDevice:self descriptor:descriptor error:error];
//        }
//   3. Optional 2-line Tier 2 texture hook in `rdna4_argument_table_commands.inc` (`rdna4PrepareArguments:`):
//        NSSet *residency = [self rdna4ResidencyDeclaredResources];
//        if (residency.count) resources = [resources setByAddingObjectsFromSet:residency];
//      (Indirect BDA buffers are already merged directly in `rdna4_command_buffer_impl.inc` `rdna4FillSet:`.)
//   4. Protocol MTLResidencySet is attached automatically in +initialize via rdna4_add_protocols()
//      and may also be listed in GarnetDevice +load census.
// ---------------------------------------------------------------------------------------------------------------

static BOOL rdna4_residency_alloc_valid(id obj, id dev, uint64_t *sizeOut) {
    if (sizeOut) *sizeOut = 0;
    if (!obj || !dev) return NO;
    if (![obj respondsToSelector:@selector(device)] || [obj device] != dev) return NO;
    BOOL knownType = [obj isKindOfClass:[RDNA4Buffer class]] ||
                     [obj isKindOfClass:[RDNA4Texture class]] ||
                     [obj isKindOfClass:[RDNA4Heap class]];
    if (!knownType) {
        Class asCls = objc_getClass("GarnetAccelerationStructure");
        Class spCls = objc_getClass("RDNA4SparseHeap");
        if ((asCls && [obj isKindOfClass:asCls]) ||
            (spCls && [obj isKindOfClass:spCls]))
            knownType = YES;
    }
    if (!knownType) {
        Protocol *pAlloc = objc_getProtocol("MTLAllocation");
        Protocol *pRes = objc_getProtocol("MTLResource");
        Protocol *pHeap = objc_getProtocol("MTLHeap");
        Protocol *pAS = objc_getProtocol("MTLAccelerationStructure");
        if ((pAlloc && [obj conformsToProtocol:pAlloc]) ||
            (pRes && [obj conformsToProtocol:pRes]) ||
            (pHeap && [obj conformsToProtocol:pHeap]) ||
            (pAS && [obj conformsToProtocol:pAS]))
            knownType = YES;
    }
    if (!knownType) return NO;
    if (sizeOut) {
        if ([obj respondsToSelector:@selector(allocatedSize)])
            *sizeOut = (uint64_t)((NSUInteger (*)(id, SEL))objc_msgSend)(obj, @selector(allocatedSize));
        else if ([obj respondsToSelector:@selector(size)])
            *sizeOut = (uint64_t)((NSUInteger (*)(id, SEL))objc_msgSend)(obj, @selector(size));
    }
    return YES;
}


@implementation RDNA4ResidencySnapshot
- (instancetype)initWithAllocations:(NSArray *)allocations
                      allocatedSize:(uint64_t)allocatedSize
                         generation:(uint64_t)generation {
    self = [super init];
    if (!self) return nil;
    _allocations = allocations ? [allocations copy] : @[];
    _allocatedSize = allocatedSize;
    _generation = generation;
    return self;
}

- (NSArray *)rdna4Allocations { return _allocations; }
- (uint64_t)rdna4AllocatedSize { return _allocatedSize; }
- (uint64_t)rdna4Generation { return _generation; }

- (void)rdna4AddBuffersToSet:(NSMutableSet *)buffers device:(id)device {
    if (!buffers || !device) return;
    for (id obj in _allocations) {
        if ([obj isKindOfClass:[RDNA4Buffer class]]) {
            RDNA4Buffer *b = (RDNA4Buffer *)obj;
            if ([b device] == device && ![b isAliasable]) [buffers addObject:b];
        } else if ([obj isKindOfClass:[RDNA4Heap class]]) {
            RDNA4Heap *h = (RDNA4Heap *)obj;
            if ([h device] != device) continue;
            for (id res in [h rdna4LiveResources]) {
                if ([res isKindOfClass:[RDNA4Buffer class]] && [(RDNA4Buffer *)res device] == device &&
                    ![res isAliasable])
                    [buffers addObject:res];
            }
        }
    }
}
@end


@implementation RDNA4ResidencySet
+ (void)initialize {
    if (self == [RDNA4ResidencySet class]) {
        static dispatch_once_t once;
        dispatch_once(&once, ^{
            const char *n[] = { "MTLResidencySet" };
            rdna4_add_protocols(self, n, 1, "RDNA4ResidencySet");
        });
    }
}
RDNA4_DNR(RDNA4ResidencySet)

- (instancetype)initWithDevice:(id)dev descriptor:(id)desc error:(NSError **)err {
    self = [super init];
    if (!self) return nil;
    if (!dev) {
        if (err) *err = rdna4_err(130, @"residency set requires a valid device");
        return nil;
    }
    NSUInteger initialCap = 0;
    NSString *lbl = nil;
    if (desc) {
        SEL capSel = sel_registerName("initialCapacity");
        if ([desc respondsToSelector:capSel])
            initialCap = ((NSUInteger (*)(id, SEL))objc_msgSend)(desc, capSel);
        SEL lblSel = sel_registerName("label");
        if ([desc respondsToSelector:lblSel]) {
            id raw = ((id (*)(id, SEL))objc_msgSend)(desc, lblSel);
            if ([raw isKindOfClass:[NSString class]]) lbl = [(NSString *)raw copy];
        }
    }
    if (!rdna4rs_validate_capacity((uint64_t)initialCap) || !rdna4rs_init(&_state, (uint64_t)initialCap)) {
        if (err) *err = rdna4_err(130,
            [NSString stringWithFormat:@"residency set initialCapacity %lu is invalid or cannot be allocated",
             (unsigned long)initialCap]);
        return nil;
    }
    _dev = dev;
    _lbl = lbl;
    pthread_mutex_init(&_mu, NULL);
    _stagedObjects = [NSMutableDictionary dictionary];
    _committedObjects = [NSMutableDictionary dictionary];
    _snapshot = [[RDNA4ResidencySnapshot alloc] initWithAllocations:@[] allocatedSize:0 generation:0];
    return self;
}

- (void)dealloc {
    pthread_mutex_lock(&_mu);
    rdna4rs_destroy(&_state);
    [_stagedObjects removeAllObjects];
    [_committedObjects removeAllObjects];
    _snapshot = nil;
    pthread_mutex_unlock(&_mu);
    pthread_mutex_destroy(&_mu);
}

- (id)device { return _dev; }
- (NSString *)label { return _lbl; }

- (uint64_t)allocatedSize {
    pthread_mutex_lock(&_mu);
    uint64_t sz = [_snapshot rdna4AllocatedSize];
    pthread_mutex_unlock(&_mu);
    return sz;
}

- (NSUInteger)allocationCount {
    pthread_mutex_lock(&_mu);
    NSUInteger n = [_snapshot rdna4Allocations].count;
    pthread_mutex_unlock(&_mu);
    return n;
}

- (NSArray *)allAllocations {
    pthread_mutex_lock(&_mu);
    NSArray *arr = [_snapshot rdna4Allocations];
    pthread_mutex_unlock(&_mu);
    return arr;
}

- (BOOL)containsAllocation:(id)allocation {
    if (!allocation) return NO;
    uintptr_t key = (uintptr_t)(__bridge void *)allocation;
    pthread_mutex_lock(&_mu);
    BOOL present = rdna4rs_contains(&_state, key) != 0;
    pthread_mutex_unlock(&_mu);
    return present;
}

- (void)addAllocation:(id)allocation {
    uint64_t sz = 0;
    if (!rdna4_residency_alloc_valid(allocation, _dev, &sz)) return;
    uintptr_t key = (uintptr_t)(__bridge void *)allocation;
    pthread_mutex_lock(&_mu);
    if (rdna4rs_add(&_state, key, sz)) _stagedObjects[@(key)] = allocation;
    pthread_mutex_unlock(&_mu);
}

- (void)addAllocations:(const id __unsafe_unretained *)allocations count:(NSUInteger)count {
    if (!allocations || !count) return;
    for (NSUInteger i = 0; i < count; i++) [self addAllocation:allocations[i]];
}

- (void)removeAllocation:(id)allocation {
    if (!allocation) return;
    uintptr_t key = (uintptr_t)(__bridge void *)allocation;
    int removedPending = 0;
    pthread_mutex_lock(&_mu);
    if (rdna4rs_remove(&_state, key, &removedPending) && removedPending)
        [_stagedObjects removeObjectForKey:@(key)];
    pthread_mutex_unlock(&_mu);
}

- (void)removeAllocations:(const id __unsafe_unretained *)allocations count:(NSUInteger)count {
    if (!allocations || !count) return;
    for (NSUInteger i = 0; i < count; i++) [self removeAllocation:allocations[i]];
}

- (void)removeAllAllocations {
    pthread_mutex_lock(&_mu);
    rdna4rs_remove_all(&_state);
    [_stagedObjects removeAllObjects];
    pthread_mutex_unlock(&_mu);
}

- (void)commit {
    pthread_mutex_lock(&_mu);
    rdna4rs_diff_t diff = {0};
    if (!rdna4rs_commit(&_state, &diff)) {
        pthread_mutex_unlock(&_mu);
        RDNA4LOG_ERR("RDNA4ResidencySet %p: commit failed (out of memory); preserving prior snapshot", (__bridge void *)self);
        return;
    }
    for (size_t i = 0; i < diff.added_count; i++) {
        NSNumber *k = @(diff.added[i]);
        id obj = _stagedObjects[k];
        if (obj) _committedObjects[k] = obj;
    }
    for (size_t i = 0; i < diff.removed_count; i++) {
        [_committedObjects removeObjectForKey:@(diff.removed[i])];
    }
    [_stagedObjects removeAllObjects];

    NSMutableArray *arr = [NSMutableArray arrayWithCapacity:_state.committed_count];
    for (size_t i = 0; i < _state.committed_count; i++) {
        id obj = _committedObjects[@(_state.committed[i].key)];
        if (obj) [arr addObject:obj];
    }
    _snapshot = [[RDNA4ResidencySnapshot alloc] initWithAllocations:arr
                                                      allocatedSize:_state.committed_size
                                                         generation:_state.generation];
    rdna4rs_diff_destroy(&diff);
    pthread_mutex_unlock(&_mu);
}

- (void)requestResidency {
    pthread_mutex_lock(&_mu);
    rdna4rs_request_residency(&_state);
    pthread_mutex_unlock(&_mu);
}

- (void)endResidency {
    pthread_mutex_lock(&_mu);
    rdna4rs_end_residency(&_state);
    pthread_mutex_unlock(&_mu);
}

- (id<RDNA4ResidencySnapshotInternal>)rdna4CommittedSnapshot {
    pthread_mutex_lock(&_mu);
    RDNA4ResidencySnapshot *snap = _snapshot;
    pthread_mutex_unlock(&_mu);
    return snap;
}

- (BOOL)rdna4IsResidentRequested {
    pthread_mutex_lock(&_mu);
    BOOL active = _state.resident_requested != 0;
    pthread_mutex_unlock(&_mu);
    return active;
}
@end
