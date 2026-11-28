// garnet_resources.m - GarnetMetal modular compilation unit.
#if !defined(GARNET_INTERNAL) && !defined(GARNET_METAL_MODULAR_BUILD)
#define GARNET_INTERNAL static
#endif
#ifdef GARNET_METAL_MODULAR_BUILD
#include "garnet_metal_internal.h"
#endif
#include "garnet_nocopy.h"
#include "garnet_buffer_ranges.h"
#include "garnet_resources_internal.h"

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
GARNET_INTERNAL RDNA4HeapTimelineState RDNA4HT;
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
GARNET_INTERNAL VkResult rdna4_mem_alloc(uint32_t mt, VkMemoryRequirements mr, BOOL map, BOOL zero, VkBuffer b, VkImage i, RDNA4Mem *m) {
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
GARNET_INTERNAL void rdna4_mem_free(RDNA4Mem *m) {
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
GARNET_INTERNAL RDNA4ImpCacheState RDNA4IC = { .mu = PTHREAD_MUTEX_INITIALIZER };
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
GARNET_INTERNAL VkResult rdna4_import_alloc(const VkMemoryAllocateInfo *ma, VkDeviceMemory *out) {
    if (rdna4f1_every == (unsigned)-1) { const char *e = garnet_getenv("GARNET_TEST_IMPORT_FAIL_EVERY", "RDNA4M_TEST_IMPORT_FAIL_EVERY"); rdna4f1_every = (rdna4_allow() && e) ? (unsigned)atoi(e) : 0; }
    if (rdna4f1_inject(rdna4f1_every, &rdna4f1_ctr)) { *out = VK_NULL_HANDLE; return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
    return vkAllocateMemory(RDNA4R.dev, ma, NULL, out);
}
// The shared import of `s` (ma = the import allocate info). *hit YES = an existing import was reused.
GARNET_INTERNAL VkResult rdna4_imp_get(IOSurfaceRef s, void *base, size_t ialloc, const VkMemoryAllocateInfo *ma, VkDeviceMemory *out, BOOL *hit) {
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
GARNET_INTERNAL void rdna4_imp_put(VkDeviceMemory mem) {
    uint64_t st, comp; rdna4_imp_fence(&st, &comp);
    pthread_mutex_lock(&RDNA4IC.mu);
    rdna4ic_unref(&RDNA4IC.c, (void *)mem, rdna4_now(), st);
    rdna4li_retire_orphans(&RDNA4IC.li, &RDNA4IC.c, st);
    pthread_mutex_unlock(&RDNA4IC.mu);
    rdna4_imp_drain();
}
GARNET_INTERNAL BOOL rdna4_linimg_get(const rdna4li_key *k, VkImage *outImg, int *outInited) {
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
GARNET_INTERNAL VkImage rdna4_linimg_add(const rdna4li_key *k, VkImage freshImg, int *outInited) {
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
GARNET_INTERNAL void rdna4_linimg_mark_inited(VkImage img) {
    if (!RDNA4IC.on || !img) return;
    pthread_mutex_lock(&RDNA4IC.mu);
    rdna4li_mark_inited(&RDNA4IC.li, (void *)img);
    pthread_mutex_unlock(&RDNA4IC.mu);
}
GARNET_INTERNAL BOOL rdna4_linimg_is_inited(VkImage img) {
    if (!RDNA4IC.on || !img) return NO;
    pthread_mutex_lock(&RDNA4IC.mu);
    int v = rdna4li_is_inited(&RDNA4IC.li, (void *)img);
    pthread_mutex_unlock(&RDNA4IC.mu);
    return v != 0;
}

GARNET_INTERNAL void rdna4_linimg_put(VkImage img) {
    if (!img) return;
    uint64_t st, comp; rdna4_imp_fence(&st, &comp);
    pthread_mutex_lock(&RDNA4IC.mu);
    int found = rdna4li_unref(&RDNA4IC.li, (void *)img, rdna4_now(), st);
    pthread_mutex_unlock(&RDNA4IC.mu);
    if (!found) vkDestroyImage(RDNA4R.dev, img, NULL);
    else rdna4_imp_drain();
}
// Base-less (protected) IOSurfaces: log once per id (a ring of the last 64 ids), count every texture.
GARNET_INTERNAL void rdna4_baseless_note(uint32_t id) {
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

GARNET_INTERNAL RDNA4VramSurfState RDNA4VS = { .mu = PTHREAD_MUTEX_INITIALIZER, .flushCond = PTHREAD_COND_INITIALIZER };

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

GARNET_INTERNAL void rdna4_vs_wait_flush_done(uint64_t vsId) {
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

GARNET_INTERNAL void rdna4_vs_put(uint64_t vsId) {
    if (!RDNA4VS.on || !vsId) return;
    uint64_t st, comp; rdna4_imp_fence(&st, &comp);
    pthread_mutex_lock(&RDNA4VS.mu);
    (void)garnet_vs_unref(&RDNA4VS.c, vsId, rdna4_now(), st);
    pthread_mutex_unlock(&RDNA4VS.mu);
    rdna4_vs_evict_if_needed();
}

GARNET_INTERNAL BOOL rdna4_vs_try_bind(IOSurfaceRef s, void *base, size_t ialloc, size_t alloc, NSUInteger poff,
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
GARNET_INTERNAL void *rdna4nc_owned_allocate(uint64_t len, uint64_t hostAlign, uint64_t *span) {
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
GARNET_INTERNAL int rdna4nc_host_readonly(const void *p, NSUInteger len) {
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

