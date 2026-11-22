// ---------------------------------------------------------------------------------------------------------------
// P1 "memory pooling" (rdna4_pool.h decides, this block does the Vulkan calls). ON only when /private/tmp/rdna4m-pool exists, latched ONCE when the device is
// created; OFF = every call site below runs the code that was there before (the `else` branches are the old lines, unchanged).
//  * RDNA4Buffer / RDNA4Texture memory: <= 256 KiB from 4 MiB slabs (bound at the slab offset; host-visible slabs mapped once), 256 KiB..64 MiB from a recycle
//    cache keyed by (memory type, size), the rest (and anything VkMemoryDedicatedRequirements marks dedicated) on its own allocation as before.
//  * Nothing freed is reused until every command buffer that existed at the free has completed (device-wide fence, see RDNA4CommandBuffer: serial taken at creation
//    in -initWithQueue:, closed in the completion block of -submitCommandBuffers:count: or at dealloc if it was never submitted).
//  * Descriptor pools of a command buffer go to a fence-gated free list (cap 32) instead of being destroyed.
// IOSurface / host-pointer imports, the scanout slots (RDNA4S.mem) and heaps' budgets are untouched.
// ---------------------------------------------------------------------------------------------------------------
#define RDNA4_POOL_FILE "/private/tmp/rdna4m-pool"
#define RDNA4_OPT_NOZERO (1ULL << 40)   // internal marker in RDNA4Buffer's options: the contents need not be zeroed (the upload ring)
typedef struct { int kind; VkDeviceMemory mem; uint8_t *map; uint64_t slab, off, size; uint32_t key; } RDNA4Mem;   // kind 0 = own allocation, 1 = slab range, 2 = whole (recycled / recyclable)
// The heap needs retirement serials even when the memory pool is OFF. Keep a second
// fence-only timeline under the same mutex, while the pool timeline remains gated by
// its own switch.
static struct { rdna4p_pool p; BOOL ready; } RDNA4HT;
static struct { BOOL on; pthread_mutex_t mu; rdna4p_pool p; BOOL reqOK; uint64_t lastTrim, lastSig; unsigned unchanged;
    PFN_vkGetBufferMemoryRequirements2 bmr2; PFN_vkGetImageMemoryRequirements2 imr2; PFN_vkResetDescriptorPool rdp; } RDNA4P = { .mu = PTHREAD_MUTEX_INITIALIZER };
static void rdna4_pool_latch(void) {
    static int done; if (done) return; done = 1;
    RDNA4P.on = access(RDNA4_POOL_FILE, F_OK) == 0 || access(RDNA4_POOL_FILE "-off", F_OK) != 0;   //: DEFAULT ON; /private/tmp/rdna4m-pool-off disables
    rdna4p_init(&RDNA4P.p, RDNA4P.on);
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
    for (;;) {
        rdna4p_rel o; pthread_mutex_lock(&RDNA4P.mu); int got = rdna4p_pop_release(&RDNA4P.p, &o); pthread_mutex_unlock(&RDNA4P.mu);
        if (!got) return freed;
        vkFreeMemory(RDNA4R.dev, (VkDeviceMemory)o.mem, NULL);
        freed++;
    }
}
static void rdna4_pool_tick(void) {
    if (!RDNA4P.on) return;
    pthread_mutex_lock(&RDNA4P.mu); rdna4p_trim(&RDNA4P.p, rdna4_now()); pthread_mutex_unlock(&RDNA4P.mu);
    rdna4_pool_drain();
}
static void rdna4_pool_report(unsigned tick) {
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
            *m = (RDNA4Mem){ 2, (VkDeviceMemory)r.mem, (uint8_t *)r.map, 0, 0, mr.size, key };
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
            VkDeviceMemory wm; uint8_t *wmap; VkResult vr = rdna4_pool_vkalloc(mt, mr.size, map, &wm, &wmap);
            if (vr != VK_SUCCESS) return vr;
            *m = (RDNA4Mem){ 2, wm, wmap, 0, 0, mr.size, key };   // fresh from the kernel: already zero
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
    rdna4_pool_drain();
    memset(m, 0, sizeof *m);
}
static uint64_t rdna4_fence_open(void) {
    pthread_mutex_lock(&RDNA4P.mu);
    if (!RDNA4HT.ready) { rdna4p_init(&RDNA4HT.p, 1); RDNA4HT.ready = YES; }
    uint64_t s = rdna4p_submit(&RDNA4HT.p);
    if (!s) {
        pthread_mutex_unlock(&RDNA4P.mu);
        RDNA4LOG("heap retirement timeline unavailable (pending allocation failed); command buffer refused");
        return 0;
    }
    if (RDNA4P.on) {
        uint64_t pooled = rdna4p_submit(&RDNA4P.p);
        if (!pooled) {
            BOOL cancelled = rdna4p_cancel_last(&RDNA4HT.p, s);
            pthread_mutex_unlock(&RDNA4P.mu);
            RDNA4LOG("pool timeline unavailable (pending allocation failed); heap serial %llu %s",
                     (unsigned long long)s, cancelled ? "rolled back" : "ROLLBACK FAILED");
            return 0;
        }
        if (pooled != s) {
            BOOL poolCancelled = rdna4p_cancel_last(&RDNA4P.p, pooled);
            BOOL heapCancelled = rdna4p_cancel_last(&RDNA4HT.p, s);
            pthread_mutex_unlock(&RDNA4P.mu);
            RDNA4LOG("timeline serial mismatch: heap %llu pool %llu; rollback %s/%s",
                     (unsigned long long)s, (unsigned long long)pooled,
                     heapCancelled ? "ok" : "FAILED", poolCancelled ? "ok" : "FAILED");
            return 0;
        }
    }
    pthread_mutex_unlock(&RDNA4P.mu);
    return s;
}
static void rdna4_fence_close(uint64_t s) {
    if (!s) return;
    pthread_mutex_lock(&RDNA4P.mu);
    if (RDNA4P.on) rdna4p_done(&RDNA4P.p, s);
    if (RDNA4HT.ready) rdna4p_done(&RDNA4HT.p, s);
    pthread_mutex_unlock(&RDNA4P.mu);
}
// Descriptor pools: a segment's pools go back here at that segment's fence instead of being destroyed.
static BOOL rdna4_dp_give(VkDescriptorPool dp) {
    if (!RDNA4P.on || !RDNA4P.rdp) return NO;
    pthread_mutex_lock(&RDNA4P.mu); int ok = rdna4p_h_push(&RDNA4P.p, (uint64_t)(uintptr_t)dp, 32); pthread_mutex_unlock(&RDNA4P.mu); return ok ? YES : NO;
}
static void rdna4_dp_created(void) { if (!RDNA4P.on) return; pthread_mutex_lock(&RDNA4P.mu); RDNA4P.p.dpCreate++; pthread_mutex_unlock(&RDNA4P.mu); }
static VkDescriptorPool rdna4_dp_take(void) {   // VK_NULL_HANDLE: none cleared by the fence; the caller creates one
    if (!RDNA4P.on || !RDNA4P.rdp) return VK_NULL_HANDLE;
    pthread_mutex_lock(&RDNA4P.mu); VkDescriptorPool dp = (VkDescriptorPool)(uintptr_t)rdna4p_h_pop(&RDNA4P.p); pthread_mutex_unlock(&RDNA4P.mu);
    if (!dp) return VK_NULL_HANDLE;
    if (RDNA4P.rdp(RDNA4R.dev, dp, 0) != VK_SUCCESS) { vkDestroyDescriptorPool(RDNA4R.dev, dp, NULL); return VK_NULL_HANDLE; }
    return dp;
}

// ---------------------------------------------------------------------------------------------------------------
// P5b "per-draw redundancy" (rdna4_drawcache.h decides, this block holds the latch, the device-wide render-pass cache and the counters). ON only when
// /private/tmp/rdna4m-drawopt exists, latched ONCE at device creation; OFF = every call site runs the code that was there before.
//  * RDNA4RenderEncoder: re-uses a stage's VkDescriptorSet while the exact slot contents its set layout reads are unchanged; skips vkCmdBindPipeline for the pipeline
//    already bound in the current render pass; remembers the last (topology, cull, front, pso) -> VkPipeline answer.
//  * Render passes are cached device-wide by their attachment descriptions (<= 64, never destroyed while the device lives; beyond the cap created / destroyed as before).
// ---------------------------------------------------------------------------------------------------------------
#define RDNA4_DRAWOPT_FILE "/private/tmp/rdna4m-drawopt"
static struct { BOOL on; pthread_mutex_t mu; rdna4dc_rpcache *rp; _Atomic uint64_t draws, setsReused, setsAlloc, pipesSkipped, pipesBound, pcHits, rpHits, rpCreated, rpUncached; uint64_t lastSig; } RDNA4DO = { .mu = PTHREAD_MUTEX_INITIALIZER };
static _Atomic int rdna4_dw_logged;   // P5b: how many "scanout: display write" lines rdna4s_disp_account has printed (it prints the pipeline-name list of the first 20 only)
static void rdna4_drawopt_latch(void) {
    static int done; if (done) return; done = 1;
    RDNA4DO.on = access(RDNA4_DRAWOPT_FILE, F_OK) == 0 || access(RDNA4_DRAWOPT_FILE "-off", F_OK) != 0;   //: DEFAULT ON; /private/tmp/rdna4m-drawopt-off disables
    if (RDNA4DO.on) { RDNA4DO.rp = (rdna4dc_rpcache *)calloc(1, sizeof *RDNA4DO.rp); if (!RDNA4DO.rp) RDNA4DO.on = NO; }
    if (!RDNA4DO.on) { RDNA4LOG("T1 drawopt: OFF (no %s at device creation; latched for the life of this process)", RDNA4_DRAWOPT_FILE); return; }
    RDNA4LOG("T1 drawopt: ON (%s present at device creation; latched for the life of this process): descriptor-set reuse, pipeline-bind skip, variant cache, render-pass cache (<= %d)", RDNA4_DRAWOPT_FILE, RDNA4DC_RP_CAP);
}
static void rdna4_drawopt_report(unsigned tick) {
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
// P4 import cache (rdna4_impcache.h decides, this block does the Vulkan / CF calls). ON by default since build 16 (F2); the kill file /private/tmp/rdna4m-noimpcache switches it OFF, latched ONCE at device creation;
// OFF = -initWithDevice:descriptor:iosurface:plane: and -dealloc run the per-texture import exactly as before (`_impShared` stays NO).
//  * One VkDeviceMemory import per live IOSurface (key id + base + import size), shared by all its textures; kept 2 s after the last texture died, <= 128 MiB unused (512 MiB in WindowServer).
//  * The cache holds its own CFRetain on the IOSurface for as long as it keeps the import (NOT IOSurfaceIncrementUseCount: that stays per texture).
//  * An import given up is vkFreeMemory'd only once the P1 fence has cleared the serial taken when it was given up (stamp 0 / completed 0 when the pool is OFF = as today).
// ---------------------------------------------------------------------------------------------------------------
#define RDNA4_IMPCACHE_KILL_FILE "/private/tmp/rdna4m-noimpcache"   // build 16 (F2): the cache is ON by default; this file switches it OFF (latched once per process). The old presence file rdna4m-impcache is no longer read.
static struct { BOOL on; pthread_mutex_t mu; rdna4ic_cache c; _Atomic uint64_t baseless; uint64_t lastSig; } RDNA4IC = { .mu = PTHREAD_MUTEX_INITIALIZER };
static void rdna4_imp_latch(void) {
    static int done; if (done) return; done = 1;
    RDNA4IC.on = rdna4ic_default_on(access(RDNA4_IMPCACHE_KILL_FILE, F_OK) == 0) ? YES : NO;   // build 16 (F2): default ON
    rdna4ic_init(&RDNA4IC.c, RDNA4IC.on);
    rdna4ic_set_cap(&RDNA4IC.c, rdna4ic_cap_for_process(rdna4_is_ws() ? 1 : 0));
    if (RDNA4IC.on) RDNA4LOG("P4 impcache: ON (default since build 16; kill file %s absent at device creation; latched for the life of this process): one shared import per IOSurface, kept %llu ms after its last texture, <= %llu MiB unused, frees fence-gated (%s)",
        RDNA4_IMPCACHE_KILL_FILE, (unsigned long long)(RDNA4IC_IDLE_NS / 1000000ULL), (unsigned long long)(RDNA4IC.c.capBytes >> 20), RDNA4P.on ? "P1 fence" : "pool OFF: kernel idle wait as today");
    else RDNA4LOG("P4 impcache: OFF (kill file %s present at device creation; latched for the life of this process)", RDNA4_IMPCACHE_KILL_FILE);
}
static void rdna4_imp_fence(uint64_t *stamp, uint64_t *completed) {
    *stamp = 0; *completed = 0;
    if (!RDNA4P.on) return;
    pthread_mutex_lock(&RDNA4P.mu); *stamp = RDNA4P.p.submitted; *completed = RDNA4P.p.completed; pthread_mutex_unlock(&RDNA4P.mu);
}
// Frees every released import the fence allows (vkFreeMemory is the slow kernel call: no mutex held).
static BOOL rdna4_imp_drain(void) {
    if (!RDNA4IC.on) return NO;
    uint64_t st, comp; rdna4_imp_fence(&st, &comp);
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
static void rdna4_imp_tick(void) {
    if (!RDNA4IC.on) return;
    uint64_t st, comp; rdna4_imp_fence(&st, &comp);
    pthread_mutex_lock(&RDNA4IC.mu); rdna4ic_trim(&RDNA4IC.c, rdna4_now(), st); pthread_mutex_unlock(&RDNA4IC.mu);
    rdna4_imp_drain();
}
// Every import allocation goes through here so the F1 test hook can refuse it: RDNA4M_TEST_IMPORT_FAIL_EVERY=<n> (root + RDNA4M_ALLOW=1) fails every n-th attempt like a kernel that is out of import budget.
static unsigned long rdna4f1_ctr; static unsigned rdna4f1_every = (unsigned)-1;
static VkResult rdna4_import_alloc(const VkMemoryAllocateInfo *ma, VkDeviceMemory *out) {
    if (rdna4f1_every == (unsigned)-1) { const char *e = getenv("RDNA4M_TEST_IMPORT_FAIL_EVERY"); rdna4f1_every = (rdna4_allow() && e) ? (unsigned)atoi(e) : 0; }
    if (rdna4f1_inject(rdna4f1_every, &rdna4f1_ctr)) { *out = VK_NULL_HANDLE; return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
    return vkAllocateMemory(RDNA4R.dev, ma, NULL, out);
}
// The shared import of `s` (ma = the import allocate info). *hit YES = an existing import was reused.
static VkResult rdna4_imp_get(IOSurfaceRef s, void *base, size_t ialloc, const VkMemoryAllocateInfo *ma, VkDeviceMemory *out, BOOL *hit) {
    uint64_t st, comp; rdna4_imp_fence(&st, &comp); *hit = NO;
    uint32_t id = IOSurfaceGetID(s); void *m = NULL;
    pthread_mutex_lock(&RDNA4IC.mu); int h = rdna4ic_acquire(&RDNA4IC.c, id, (uint64_t)(uintptr_t)base, ialloc, rdna4_now(), st, &m); pthread_mutex_unlock(&RDNA4IC.mu);
    if (h) { *out = (VkDeviceMemory)m; *hit = YES; return VK_SUCCESS; }
    VkDeviceMemory mem = VK_NULL_HANDLE; VkResult r = rdna4_import_alloc(ma, &mem);
    if (r != VK_SUCCESS) {   // out of kernel imports (256 MiB per client): give back the unused cached ones the fence has cleared, once (F1: the caller then falls back to a GPU-only texture, never nil)
        pthread_mutex_lock(&RDNA4IC.mu);
        rdna4ic_flush(&RDNA4IC.c, st);
        pthread_mutex_unlock(&RDNA4IC.mu);
        if (rdna4_imp_drain()) r = rdna4_import_alloc(ma, &mem);
    }
    if (r != VK_SUCCESS) { *out = VK_NULL_HANDLE; return r; }
    // Another wrapper may have imported the same surface while allocation ran without the mutex.
    pthread_mutex_lock(&RDNA4IC.mu);
    h = rdna4ic_acquire(&RDNA4IC.c, id, (uint64_t)(uintptr_t)base, ialloc, rdna4_now(), st, &m);
    if (!h) rdna4ic_add(&RDNA4IC.c, id, (uint64_t)(uintptr_t)base, ialloc, (void *)mem, (void *)CFRetain(s));
    pthread_mutex_unlock(&RDNA4IC.mu);
    if (h) { vkFreeMemory(RDNA4R.dev, mem, NULL); *out = (VkDeviceMemory)m; *hit = YES; return VK_SUCCESS; }
    *out = mem; return VK_SUCCESS;
}
static void rdna4_imp_put(VkDeviceMemory mem) {
    uint64_t st, comp; rdna4_imp_fence(&st, &comp);
    pthread_mutex_lock(&RDNA4IC.mu); rdna4ic_unref(&RDNA4IC.c, (void *)mem, rdna4_now(), st); pthread_mutex_unlock(&RDNA4IC.mu);
    rdna4_imp_drain();
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
static void rdna4_scan_cc_counters(uint64_t *h, uint64_t *m, uint64_t *mm);
static void rdna4_imp_report(unsigned tick) {
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

