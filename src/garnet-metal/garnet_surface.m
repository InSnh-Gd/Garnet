// garnet_surface.m - GarnetMetal modular compilation unit.
#if !defined(GARNET_INTERNAL) && !defined(GARNET_METAL_MODULAR_BUILD)
#define GARNET_INTERNAL static
#endif
#if !defined(GARNET_DISPLAY_LATE_ONLY) && !defined(GARNET_PRESENT_CAPTURE_ONLY)
#ifdef GARNET_METAL_MODULAR_BUILD
#include "garnet_metal_internal.h"
#endif
#endif

#if !defined(GARNET_DISPLAY_LATE_ONLY) && !defined(GARNET_PRESENT_CAPTURE_ONLY)
// ---------------------------------------------------------------------------------------------------------------
// Build 16 (app-fix round 1; an internal design note P5 + P1). garnet_ledger.h / garnet_ioalias.h decide, this block holds the process-wide state and the IOSurface calls.
//  * Step 0, the LIVE-IMPORT LEDGER: every IOSurface texture is entered when it comes alive and removed in -dealloc; the T1 tick logs "T1 ledger: ..." (live textures, distinct surface ids, bytes, textures with no
//    command buffer in flight, the top 5 by size / format / planes with the age of the oldest). Logging only.
//  * F3, the USE-COUNT POLICY: a texture NO LONGER holds IOSurfaceIncrementUseCount for its life (Apple's Metal does not: measured on the host Mac). A command buffer use-counts a surface from the first touch of a texture of it
//    until the command buffer is finished with the GPU (rdna4PoolDone / dealloc). Kill file /private/tmp/rdna4m-nousecount = never (Apple-identical), latched once per process.
//  * P1, the ALIAS GATE: applications act on the per-command-buffer aliasing decisions; WindowServer only counts them (it keeps build 14's behaviour exactly). Kill file /private/tmp/rdna4m-noioalias.
// ---------------------------------------------------------------------------------------------------------------
#define RDNA4_NOUSECOUNT_FILE "/private/tmp/rdna4m-nousecount"
#define RDNA4_NOIOALIAS_FILE  "/private/tmp/rdna4m-noioalias"
GARNET_INTERNAL RDNA4LedgerState RDNA4LED = { .mu = PTHREAD_MUTEX_INITIALIZER };
GARNET_INTERNAL void rdna4_led_latch(void) {
    static dispatch_once_t o;
    dispatch_once(&o, ^{
        rdna4l_init(&RDNA4LED.L);
        RDNA4LED.ucMode = rdna4uc_mode(access(RDNA4_NOUSECOUNT_FILE, F_OK) == 0);
        RDNA4LED.iaActs = rdna4ia_enabled(rdna4_is_ws() ? 1 : 0, access(RDNA4_NOIOALIAS_FILE, F_OK) == 0);
        RDNA4LOG("build 16: use-count policy %s (kill file " RDNA4_NOUSECOUNT_FILE " %s); IOSurface wrapper aliasing (P1) %s (%s%s)", RDNA4LED.ucMode == RDNA4UC_INFLIGHT ? "IN-FLIGHT ONLY (a surface is use-counted while a command buffer that touched a texture of it is in flight)" : "NONE (Apple-identical)",
               RDNA4LED.ucMode == RDNA4UC_INFLIGHT ? "absent" : "PRESENT", RDNA4LED.iaActs ? "ACTS" : "OFF", rdna4_is_ws() ? "WindowServer: counted only" : "application", rdna4_is_ws() ? "" : RDNA4LED.iaActs ? "" : ", kill file " RDNA4_NOIOALIAS_FILE " present");
    });
}
GARNET_INTERNAL void rdna4_led_add(id t, IOSurfaceRef s, uint64_t bytes, uint32_t pf, uint32_t planes, BOOL nobase) {
    rdna4_led_latch();
    pthread_mutex_lock(&RDNA4LED.mu); rdna4l_add(&RDNA4LED.L, (uintptr_t)(__bridge void *)t, (uint32_t)IOSurfaceGetID(s), bytes, pf, planes, nobase ? 1 : 0, rdna4_now()); pthread_mutex_unlock(&RDNA4LED.mu);
}
GARNET_INTERNAL void rdna4_led_del(id t) { pthread_mutex_lock(&RDNA4LED.mu); rdna4l_del(&RDNA4LED.L, (uintptr_t)(__bridge void *)t); pthread_mutex_unlock(&RDNA4LED.mu); }
GARNET_INTERNAL void rdna4_led_touch(id t, int delta) {   // +1: a command buffer touched it for the first time; -1: that command buffer is finished
    pthread_mutex_lock(&RDNA4LED.mu);
    if (delta > 0) rdna4l_touch(&RDNA4LED.L, (uintptr_t)(__bridge void *)t); else rdna4l_untouch(&RDNA4LED.L, (uintptr_t)(__bridge void *)t);
    pthread_mutex_unlock(&RDNA4LED.mu);
}
GARNET_INTERNAL void rdna4_uc_inc(void *s, void *c) { (void)c; CFRetain(s); IOSurfaceIncrementUseCount((IOSurfaceRef)s); atomic_fetch_add(&RDNA4LED.ucTake, 1); }
GARNET_INTERNAL void rdna4_uc_dec(void *s, void *c) { (void)c; IOSurfaceDecrementUseCount((IOSurfaceRef)s); CFRelease(s); atomic_fetch_add(&RDNA4LED.ucRelease, 1); }
GARNET_INTERNAL BOOL rdna4_ioalias_acts(void) { rdna4_led_latch(); return RDNA4LED.iaActs ? YES : NO; }
GARNET_INTERNAL void rdna4_led_report(unsigned tick) {
    rdna4_led_latch();
    rdna4l_snap sn; char line[1400]; uint64_t imp; unsigned long long tk = atomic_load(&RDNA4LED.ucTake), rl = atomic_load(&RDNA4LED.ucRelease);
    @synchronized ([GarnetDevice class]) { imp = RDNA4R.impTotal; }
    pthread_mutex_lock(&RDNA4LED.mu);
    rdna4l_snapshot(&RDNA4LED.L, rdna4_now(), &sn);
    const uint64_t sig = sn.live * 1000003ULL + RDNA4LED.L.added + RDNA4LED.L.removed * 31 + sn.texBytes + tk + rl + atomic_load(&RDNA4LED.f1Fallbacks);
    const BOOL emit = sn.live > 0 || sig != RDNA4LED.lastSig || tick % 6 == 5;
    if (emit && (sn.live > 0 || RDNA4LED.L.added > 0)) {
        RDNA4LED.lastSig = sig;
        int n = rdna4l_fmt(&sn, imp, &RDNA4LED.L, line, sizeof line);
        if (n > 0 && (size_t)n < sizeof line) snprintf(line + n, sizeof line - (size_t)n, " | usecount %s takes=%llu releases=%llu held_now=%lld | ioalias %s acted flush/reupload=%llu/%llu ws_would flush/reupload=%llu/%llu | f1_fallbacks=%llu [%s pid %d, 10 s tick]",
            RDNA4LED.ucMode == RDNA4UC_INFLIGHT ? "IN-FLIGHT" : "NONE", tk, rl, (long long)(tk - rl), RDNA4LED.iaActs ? "ON" : "OFF", (unsigned long long)atomic_load(&RDNA4LED.iaFlush), (unsigned long long)atomic_load(&RDNA4LED.iaReup),
            (unsigned long long)atomic_load(&RDNA4LED.iaWsFlush), (unsigned long long)atomic_load(&RDNA4LED.iaWsReup), (unsigned long long)atomic_load(&RDNA4LED.f1Fallbacks), getprogname(), (int)getpid());
        pthread_mutex_unlock(&RDNA4LED.mu);
        RDNA4LOG("%s", line);
        return;
    }
    pthread_mutex_unlock(&RDNA4LED.mu);
}

// One-shot command buffer on the device queue, waited to completion (11e: replaceRegion:, getBytes:). The pool and the queue are
// externally synchronised by RDNA4R.qlock. A failed wait retains the recording
// block's resources as well as its command buffer until process exit.
GARNET_INTERNAL BOOL rdna4_oneshot(NSError **err, void (^rec)(VkCommandBuffer cmd),
                          BOOL *unresolved) {
    if (unresolved) *unresolved = NO;
    if (!rdna4_radv_open(err)) return NO;
    if (atomic_load(&rdna4_submit_fault) != VK_SUCCESS) {
        if (err) *err = rdna4_err(61, @"An earlier GPU submission is unresolved; restart the process before submitting again");
        return NO;
    }
    VkCommandBuffer c = VK_NULL_HANDLE; VkFence f = VK_NULL_HANDLE; VkResult r;
    BOOL submitAttempted = NO;
    VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = RDNA4R.opool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    rdna4_qlock();
    r = vkAllocateCommandBuffers(RDNA4R.dev, &ai, &c);
    pthread_mutex_unlock(&RDNA4R.qlock);
    if (r != VK_SUCCESS) { if (err) *err = rdna4_err(60, [NSString stringWithFormat:@"one-shot vkAllocateCommandBuffers = %d", r]); return NO; }
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    r = vkBeginCommandBuffer(c, &bi);
    if (r == VK_SUCCESS) { rec(c); r = vkEndCommandBuffer(c); }
    VkFenceCreateInfo fc = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (r == VK_SUCCESS) r = vkCreateFence(RDNA4R.dev, &fc, NULL, &f);
    if (r == VK_SUCCESS) {
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &c };
        rdna4_qlock();
        r = (VkResult)atomic_load(&rdna4_submit_fault);
        if (r == VK_SUCCESS) {
            submitAttempted = YES;
            r = vkQueueSubmit(RDNA4R.q, 1, &si, f);
            if (r != VK_SUCCESS) atomic_store(&rdna4_submit_fault, (int)r);
        }
        pthread_mutex_unlock(&RDNA4R.qlock);
    }
    if (r == VK_SUCCESS) {
        int slices = 0; r = VK_TIMEOUT;
        while (r == VK_TIMEOUT && slices++ < 30) r = vkWaitForFences(RDNA4R.dev, 1, &f, VK_TRUE, 2000000000ULL);
    }
    if (submitAttempted && r != VK_SUCCESS) {
        rdna4_quarantine([rec copy], r);
        if (unresolved) *unresolved = YES;
    }
    else {
        if (f) vkDestroyFence(RDNA4R.dev, f, NULL);
        rdna4_qlock(); vkFreeCommandBuffers(RDNA4R.dev, RDNA4R.opool, 1, &c); pthread_mutex_unlock(&RDNA4R.qlock);
    }
    if (r != VK_SUCCESS) { if (err) *err = rdna4_err(61, [NSString stringWithFormat:@"one-shot submit/wait = %d%s", r, r == VK_ERROR_DEVICE_LOST ? " (DEVICE_LOST)" : r == VK_TIMEOUT ? " (TIMEOUT: HUNG?)" : ""]); return NO; }
    return YES;
}

// ---------------------------------------------------------------------------------------------------------------
// S5.2a D-copy present (an internal design note section 3, native #12). CoreDisplay's GPUPass renders into 3 IOSurfaces; at the end of each command buffer
// that wrote one, rdna4Finish appends a GPU copy (import -> a visible-VRAM scanout slot) and the completion block presents that slot (VUPDATE-latched flip) through
// the six radv_darwin_scanout_* exports of the loaded RADV (same per-process RDNA4N connection). The state machine is the pure garnet_dispflip.h (host test:
// test-dispflip.c). Default OFF where anything is missing; kill switch /private/tmp/rdna4m-noflip (read once per process); one mutex around every scanout call.
// ---------------------------------------------------------------------------------------------------------------
#define RDNA4_NOFLIP_FILE "/private/tmp/rdna4m-noflip"
#define RDNA4_PRESENTALL_FILE "/private/tmp/rdna4m-presentall"   // native #12 P2 kill switch: present EVERY display write (the pre-final-pass-rule behaviour); polled every 1 s
#define RDNA4_DAMAGE_FILE "/private/tmp/rdna4m-damage"   // native #12 D2: partial-update emulation + SkyLight composite passes count as frames; DEFAULT OFF; polled every 1 s
#define RDNA4_HOLD_FILE "/private/tmp/rdna4m-hold"   // native #12 P3 present hold: present = ALL mode, content "1".."8" = H in ms, else 3; polled every 1 s
#define RDNA4_NOHOLD_FILE "/private/tmp/rdna4m-nohold"   // kill switch disabling default FINAL present hold; polled every 1 s
static struct { _Atomic int mode, ms; uint64_t held, held_sup, held_pres, hold_ns; } RDNA4H = { .mode = RDNA4DF_HOLD_MODE_FINAL, .ms = RDNA4DF_HOLD_DEFAULT_MS };   // switch state (atomics); counters are written by the present queue under RDNA4S.mu


GARNET_INTERNAL RDNA4ScanoutState RDNA4S = { .mu = PTHREAD_MUTEX_INITIALIZER };
// ---- bundle 13 (M6 Stage 1b) + bundle 15 (M6 Stage 2): the HDMI instances (1 = the monitor A, 2 = the monitor B) beside the DP's RDNA4S, as an ARRAY indexed by the kernel instance number ([0] is never used: the DP is RDNA4S). Same locking (RDNA4S.mu protects everything
// below), same state machine type (garnet_dispflip.h, a FULL-COPY machine: damage mode is the DP's alone), each with its own 3 slots in visible VRAM, registered and presented through Mesa's radv_darwin_rdna4n_call (selectors 22..26). The decisions
// are garnet_m6x.h (host-tested). An instance is enabled only when rdna4x_enabled(); acquired lazily; ANY failure leaves THAT instance OFF with the DP (and the other display) untouched. Slot size, copy rectangle and geometry come from the
// instance's DESCRIPTOR (rdna4x_desc), never from the DP's plane.
GARNET_INTERNAL RDNA4XScanoutState RDNA4X;   // RDNA4X.i[inst]
static const char *rdna4s_reason_name(int r);
static BOOL rdna4x_status_locked(int inst, struct rdna4n_scan_status *st);
// the enable MASK the routing decisions take: bit i = instance i is enabled and not OFF. RDNA4S.mu held.
static uint32_t rdna4x_onmask_locked(void) { uint32_t m = 0; for (int i = 1; i <= 2; i++) if (RDNA4XI(i).enabled && RDNA4XI(i).sm.state != RDNA4DF_OFF) m |= 1u << i; return m; }
// the raw selector calls (RDNA4S.mu held by the caller; every one returns 0 or -errno)
static int rdna4x_k_acquire(int inst, uint64_t o[5], uint32_t *nout) {
    const uint64_t in[2] = { (uint64_t)inst, 0 }; uint32_t no = 5; *nout = 0;
    int rc = RDNA4X.call(RDNA4R.dev, RDNA4N_SEL_SCANX_ACQUIRE, in, 2, NULL, 0, o, &no, NULL, NULL);
    if (rdna4x_acquire_retry4(inst, rc)) { no = 4; rc = RDNA4X.call(RDNA4R.dev, RDNA4N_SEL_SCANX_ACQUIRE, in, 2, NULL, 0, o, &no, NULL, NULL); }   // a 0.0.661 kernel answers four words only (the monitor B may fall back, and ONLY after the five-word shape was refused (-EINVAL), R-S2; the monitor A never)
    if (rc) return rc;
    if (no != 4 && no != 5) return -EIO;
    *nout = no; return 0;
}
static int rdna4x_k_register(int inst, VkDeviceMemory mem, uint32_t pitch, uint32_t w, uint32_t h, uint32_t *slot, uint64_t *mc) {
    uint32_t handle = 0; int rc = RDNA4X.bohandle(RDNA4R.dev, mem, &handle); if (rc) return rc;
    const struct rdna4n_scan_reg reg = { .handle = handle, .offset = 0, .pitch_bytes = pitch, .height = h, .width = w, .format = RDNA4N_SCAN_FMT_ARGB8888 };
    const uint64_t in[1] = { (uint64_t)inst }; uint64_t o[2] = { 0, 0 }; uint32_t no = 2;
    rc = RDNA4X.call(RDNA4R.dev, RDNA4N_SEL_SCANX_REGISTER, in, 1, &reg, sizeof reg, o, &no, NULL, NULL);
    if (rc) return rc;
    if (no != 2) return -EIO;
    *slot = (uint32_t)o[0]; *mc = o[1]; return 0;
}
static int rdna4x_k_present(int inst, uint32_t slot, uint64_t o[3]) { const uint64_t in[3] = { (uint64_t)inst, slot, 0 }; uint32_t no = 3; return RDNA4X.call(RDNA4R.dev, RDNA4N_SEL_SCANX_PRESENT, in, 3, NULL, 0, o, &no, NULL, NULL) ?: (no == 3 ? 0 : -EIO); }
static int rdna4x_k_status(int inst, struct rdna4n_scan_status *st, uint64_t o[4]) {
    const uint64_t in[1] = { (uint64_t)inst }; uint32_t no = 4; size_t sz = sizeof *st; memset(st, 0, sizeof *st);
    const int rc = RDNA4X.call(RDNA4R.dev, RDNA4N_SEL_SCANX_STATUS, in, 1, NULL, 0, o, &no, st, &sz);
    return rc ?: (sz == sizeof *st && no == 4 ? 0 : -EIO);
}
static int rdna4x_k_release(int inst, uint64_t o[2]) { const uint64_t in[1] = { (uint64_t)inst }; uint32_t no = 2; return RDNA4X.call(RDNA4R.dev, RDNA4N_SEL_SCANX_RELEASE, in, 1, NULL, 0, o, &no, NULL, NULL) ?: (no == 2 ? 0 : -EIO); }
// Release instance `inst` (the kernel puts A back, polled and verified) and stop presenting to it. Idempotent. RDNA4S.mu held.
static void rdna4x_do_release_locked(int inst) {
    rdna4xi_t *const x = &RDNA4XI(inst); const rdna4x_desc_t *const d = rdna4x_desc((uint32_t)inst);
    if (RDNA4X.call && x->sm.acquired && !x->sm.released) {
        x->sm.released = 1; uint64_t o[2] = { 0, 0 }; const int rc = rdna4x_k_release(inst, o);
        RDNA4LOG("scanout (instance %d, %s): release rc %d, restore of A verified %llu, plane MC after 0x%llx", inst, d ? d->name : "?", rc, (unsigned long long)o[0], (unsigned long long)o[1]);
    }
    if (x->sm.state == RDNA4DF_ACTIVE) x->sm.state = RDNA4DF_OFF;   // a released instance presents nothing more (the DP failing closed takes the HDMI displays with it)
}
static void rdna4x_release_all_locked(void) { for (int i = 1; i <= 2; i++) rdna4x_do_release_locked(i); }
// Fail closed: THAT instance only. The DP and the other display are never touched.
static void rdna4x_off_locked(int inst, int reason, const char *why) {
    rdna4xi_t *const x = &RDNA4XI(inst); const rdna4x_desc_t *const d = rdna4x_desc((uint32_t)inst);
    const int rel = rdna4df_fail(&x->sm, reason);
    RDNA4LOG("scanout (instance %d, %s): FAIL CLOSED / OFF (%s): %s", inst, d ? d->name : "?", rdna4s_reason_name(reason), why);
    if (rel) { x->sm.released = 0; rdna4x_do_release_locked(inst); }
}

GARNET_INTERNAL uint32_t rdna4_acc_port;   // bundle 11: the accelerator port WindowServer's admitted device was created with (the table is a property of its parent, the Metal nub); set once, never released here

// ---- bundle 11 (M6 Stage 1a): the kernel's IOSurface ID -> display table (decisions: garnet_m6route.h) --------------------------------------------------------------------------------------------------
// The table is the registry property "Garnet,M6Surf" of the Metal nub (the accelerator's parent: the channel Garnet,Ready already uses in WindowServer). Read OUTSIDE RDNA4S.mu (a registry read is an IPC), stored under it.
static int rdna4s_m6_read(rdna4m6_t *tmp) {
    const uint32_t port = rdna4_acc_port;
    if (!port) return -1;
    io_registry_entry_t parent = 0;
    if (IORegistryEntryGetParentEntry((io_registry_entry_t)port, kIOServicePlane, &parent) != KERN_SUCCESS || !parent) return -1;
    CFTypeRef v = IORegistryEntryCreateCFProperty(parent, CFSTR(RDNA4M6_PROP_SURF), kCFAllocatorDefault, 0);
    IOObjectRelease(parent);
    if (!v) return -2;
    int rc = -2;
    if (CFGetTypeID(v) == CFDataGetTypeID()) rc = rdna4m6_parse(tmp, CFDataGetBytePtr((CFDataRef)v), (size_t)CFDataGetLength((CFDataRef)v));
    CFRelease(v);
    return rc;
}
// Caller holds RDNA4S.mu; it is dropped around the registry read and held again on return.
static void rdna4s_m6_refresh_locked(void) {
    pthread_mutex_unlock(&RDNA4S.mu);
    rdna4m6_t tmp; memset(&tmp, 0, sizeof tmp);
    const int rc = rdna4s_m6_read(&tmp);
    pthread_mutex_lock(&RDNA4S.mu);
    RDNA4S.m6.refreshes++;
    if (rc == 0) { if (rdna4m6_store(&RDNA4S.m6, &tmp) != 0 && RDNA4S.m6.store_refused <= 3) RDNA4LOG("m6: an OLDER table read (generation %u < cached %u) was not stored (out-of-order refresh)", tmp.gen, RDNA4S.m6.gen); }   // bundle 13 (R2): never store an older generation over a newer one
    else { RDNA4S.m6.refresh_fail++; if (RDNA4S.m6.refresh_fail <= 3) RDNA4LOG("m6: the kernel's surface table could not be read (rc %d, accelerator port 0x%x)", rc, rdna4_acc_port); }
}
// bundle 12 (queue 290 S3): an unknown ID is refreshed ASYNCHRONOUSLY - the registry read is an IPC, and it must never run (or sleep) on the serial present queue. At most one read is in flight; the result is stored
// under RDNA4S.mu like every refresh. The completion that asked is not blocked: it is queued again RDNA4M6_RETRY_NS later (rdna4s_complete_run).
static _Atomic int rdna4s_m6_refreshing;
static void rdna4s_m6_refresh_async(void) {
    if (atomic_exchange(&rdna4s_m6_refreshing, 1)) return;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        rdna4m6_t tmp; memset(&tmp, 0, sizeof tmp);
        const int rc = rdna4s_m6_read(&tmp);
        pthread_mutex_lock(&RDNA4S.mu);
        RDNA4S.m6.refreshes++;
        if (rc == 0) { if (rdna4m6_store(&RDNA4S.m6, &tmp) != 0 && RDNA4S.m6.store_refused <= 3) RDNA4LOG("m6: an OLDER table read (generation %u < cached %u) was not stored (out-of-order refresh)", tmp.gen, RDNA4S.m6.gen); }   // bundle 13 (R2)
        else { RDNA4S.m6.refresh_fail++; if (RDNA4S.m6.refresh_fail <= 3) RDNA4LOG("m6: the kernel's surface table could not be read (rc %d, accelerator port 0x%x)", rc, rdna4_acc_port); }
        pthread_mutex_unlock(&RDNA4S.mu);
        atomic_store(&rdna4s_m6_refreshing, 0);
    });
}
// RDNA4S.mu held: a frame that will not be presented because of the table (SKIP / DROP): counted once, logged (the first 12), the slot released without touching last_seq (rdna4df_skip_content).
static void rdna4s_m6_skip_locked(int slot, uint32_t sid, int plan, rdna4df_t *sm) {   // bundle 13: the machine of the frame's instance
    const int v = rdna4m6_final_peek(&RDNA4S.m6, sid);
    (void)rdna4m6_final_count(&RDNA4S.m6, sid, (plan == RDNA4M6_C_DROP || plan == RDNA4X_C_DROP) ? RDNA4M6_SKIP_UNKNOWN : v);
    rdna4df_skip_content(sm, slot);
    static int lg; if (lg < 12) { lg++; RDNA4LOG("m6: frame for IOSurface %u NOT presented (%s)", sid, plan == RDNA4M6_C_DROP ? "the kernel has not seen it on any pipe (or the table stayed stale)" : v == RDNA4M6_SKIP_OTHER ? "the kernel maps it to another display" : "the kernel saw it on two pipes: AMBIGUOUS"); }
}
GARNET_INTERNAL void rdna4_m6_report(void) {
    pthread_mutex_lock(&RDNA4S.mu);
    const int on = RDNA4S.m6.latch;
    if (on) rdna4s_m6_refresh_locked();
    rdna4m6_t c = RDNA4S.m6;
    pthread_mutex_unlock(&RDNA4S.mu);
    if (!on) return;
    RDNA4LOG("m6: surfaces for instance 1/2: %u/%u (kernel table %u IDs: instance 0 %u, ambiguous %u; refreshes %llu failed %llu); frames completed without a present: at the gate instance 1 %llu instance 2 %llu ambiguous %llu, at the present instance 1 %llu instance 2 %llu ambiguous %llu unknown %llu; tentative at the gate %llu; presented to instance 0 %llu",
           rdna4m6_count_inst(&c, RDNA4M6_INST_MONA), rdna4m6_count_inst(&c, RDNA4M6_INST_MONB), c.n, rdna4m6_count_inst(&c, RDNA4M6_INST_DP), rdna4m6_count_ambig(&c), (unsigned long long)c.refreshes, (unsigned long long)c.refresh_fail,
           (unsigned long long)c.gate_skip[RDNA4M6_INST_MONA], (unsigned long long)c.gate_skip[RDNA4M6_INST_MONB], (unsigned long long)c.gate_ambig, (unsigned long long)c.fin_skip[RDNA4M6_INST_MONA], (unsigned long long)c.fin_skip[RDNA4M6_INST_MONB],
           (unsigned long long)c.fin_ambig, (unsigned long long)c.fin_unknown, (unsigned long long)c.gate_tentative, (unsigned long long)c.fin_ok);
}

static const char *rdna4s_reason_name(int r) {
    static const char *n[] = { "none/released-on-request", "kill-file", "missing-export", "ENOSYS", "plane-geometry", "scanout-error", "plane-lost", "slot-alloc" };
    return r >= 0 && r < (int)(sizeof n / sizeof n[0]) ? n[r] : "?";
}
GARNET_INTERNAL BOOL rdna4s_testmode(void) { const char *e = getenv("RDNA4M_TEST_DISPFLIP"); return rdna4_allow() && e && !strcmp(e, "1"); }

// Release the plane (idempotent in the kernel) and stop the keep-alive. Called with the mutex held, at most once per process by the state machine's rule.
static void rdna4s_do_release_locked(void) {
    rdna4x_release_all_locked();   // bundle 13/15: the HDMI instances' A go back with the DP's console (the keep-alive ends with the timer below: a monitor B left acquired would be restored by the kernel's 5 s watchdog anyway)
    if (RDNA4S.fr) { uint64_t o[2] = { 0, 0 }; int rc = RDNA4S.fr(RDNA4R.dev, o);
        RDNA4LOG("scanout: release rc %d, console restore verified %llu, plane MC after 0x%llx", rc, (unsigned long long)o[0], (unsigned long long)o[1]); }
    if (RDNA4S.timer) { dispatch_source_cancel(RDNA4S.timer); RDNA4S.timer = nil; }
}
static void rdna4s_free_slots(void);
// Fail closed: disable D-copy for the process and release the plane if it was taken.
static void rdna4s_off_locked(int reason, const char *why) {
    int rel = rdna4df_fail(&RDNA4S.sm, reason);
    RDNA4LOG("scanout: FAIL CLOSED / OFF (%s): %s", rdna4s_reason_name(reason), why);
    if (!RDNA4S.sm.acquired) rdna4s_free_slots();
    if (rel) rdna4s_do_release_locked();
}
static const char *rdna4s_hold_mode_name(int mode) {
    if (mode == RDNA4DF_HOLD_MODE_ALL) return "ALL";
    if (mode == RDNA4DF_HOLD_MODE_FINAL) return "FINAL";
    return "OFF";
}
static void rdna4s_poll_hold_switch(BOOL forceLog) {
    int mode = RDNA4DF_HOLD_MODE_FINAL, ms = RDNA4DF_HOLD_DEFAULT_MS;
    if (access(RDNA4_NOHOLD_FILE, F_OK) == 0) {
        mode = RDNA4DF_HOLD_MODE_OFF;
    } else {
        int fd = open(RDNA4_HOLD_FILE, O_RDONLY);
        if (fd >= 0) {
            char b[32]; ssize_t n = read(fd, b, sizeof b); close(fd);
            mode = RDNA4DF_HOLD_MODE_ALL;
            ms = rdna4df_hold_parse(b, n > 0 ? (int)n : 0);
        }
    }
    int pmode = atomic_load(&RDNA4H.mode), pms = atomic_load(&RDNA4H.ms);
    if (forceLog || mode != pmode || (mode != RDNA4DF_HOLD_MODE_OFF && ms != pms)) {
        if (mode != RDNA4DF_HOLD_MODE_OFF) atomic_store(&RDNA4H.ms, ms);
        atomic_store(&RDNA4H.mode, mode);
        const char *why = mode == RDNA4DF_HOLD_MODE_OFF ? (RDNA4_NOHOLD_FILE " exists")
                        : mode == RDNA4DF_HOLD_MODE_ALL ? (RDNA4_HOLD_FILE " exists")
                        : ("default FINAL hold (" RDNA4_NOHOLD_FILE " and " RDNA4_HOLD_FILE " absent)");
        RDNA4LOG("scanout: present hold %s (%s)%s%d%s", rdna4s_hold_mode_name(mode), why,
                 mode != RDNA4DF_HOLD_MODE_OFF ? ": holds until commit + " : "",
                 mode != RDNA4DF_HOLD_MODE_OFF ? ms : 0,
                 mode != RDNA4DF_HOLD_MODE_OFF ? " ms so a later command buffer for the same surface supersedes it" : "");
    }
}

// First display-surface candidate: kill switch, dlsym of the six exports, query, visible-pool memory type. Never retried.
static void rdna4s_bind_locked(void) {
    if (RDNA4S.bound) return;
    RDNA4S.bound = 1; RDNA4S.tStart = rdna4_now();
    struct stat sb;
    BOOL killed = stat(RDNA4_NOFLIP_FILE, &sb) == 0;
    rdna4df_init(&RDNA4S.sm, killed);
    RDNA4S.sm.presentall = stat(RDNA4_PRESENTALL_FILE, &sb) == 0;
    if (RDNA4S.sm.presentall) RDNA4LOG("scanout: final-pass rule DISABLED at bind: " RDNA4_PRESENTALL_FILE " exists (every display write is copied and presented)");
    if (killed) { rdna4s_free_slots(); RDNA4LOG("scanout: D-copy OFF: kill switch " RDNA4_NOFLIP_FILE " exists (read once per process)"); return; }
    rdna4s_poll_hold_switch(YES);
    #define SYM(f, n) do { RDNA4S.f = (void *)dlsym(RDNA4R.lib, "radv_darwin_scanout_" n); if (!RDNA4S.f) { miss = YES; RDNA4LOG("scanout: export radv_darwin_scanout_" n " NOT found in the loaded RADV"); } } while (0)
    BOOL miss = NO;
    SYM(fq, "query"); SYM(fa, "acquire"); SYM(fg, "register"); SYM(fp, "present"); SYM(fs, "status"); SYM(fr, "release");
    #undef SYM
    if (miss) { RDNA4S.fr = NULL; rdna4s_off_locked(RDNA4DF_R_NOSYM, "D-copy needs all six radv_darwin_scanout_* exports; the kernel v1 copy continues"); return; }
    struct rdna4n_scan_query q; memset(&q, 0, sizeof q);
    int rc = RDNA4S.fq(RDNA4R.dev, &q);
    if (rc) { char m[96]; snprintf(m, sizeof m, "scanout_query rc %d (-ENOSYS %d = not a native kext / ABI minor < 1)", rc, -ENOSYS); rdna4s_off_locked(rc == -ENOSYS ? RDNA4DF_R_NOSYS : RDNA4DF_R_ERROR, m); return; }
    RDNA4S.m6.latch = (q.flags & RDNA4N_SCANQ_M6) != 0;
    if (RDNA4S.m6.latch) RDNA4LOG("scanout: the kernel latch garnet-m6 is ON (scan_query flag RDNA4N_SCANQ_M6): only surfaces the kernel maps to instance 0 are presented; the table is the Metal nub's property %s (accelerator port 0x%x)", RDNA4M6_PROP_SURF, rdna4_acc_port);
    RDNA4LOG("scanout: query: %ux%u plane, pitch %u px, hubp_format %u sw_mode %u, refresh %u mHz, flags 0x%x (LIT %d NATIVE %d ACQUIRED %d GEOM_OK %d), console MC 0x%llx",
           q.plane_w, q.plane_h, q.pitch_px, q.hubp_format, q.sw_mode, q.refresh_mhz, q.flags, !!(q.flags & RDNA4N_SCANQ_LIT), !!(q.flags & RDNA4N_SCANQ_NATIVE), !!(q.flags & RDNA4N_SCANQ_ACQUIRED),
           !!(q.flags & RDNA4N_SCANQ_GEOM_OK), (unsigned long long)q.console_mc);
    if (!(q.flags & RDNA4N_SCANQ_GEOM_OK) || !q.plane_w || !q.plane_h || q.pitch_px < q.plane_w) { rdna4s_off_locked(RDNA4DF_R_GEOM, "the live plane is not linear ARGB8888 with a plausible pitch (GEOM_OK clear)"); return; }
    RDNA4S.pw = q.plane_w; RDNA4S.ph = q.plane_h; RDNA4S.pitch = q.pitch_px * 4;
    RDNA4S.slotBytes = ((uint64_t)RDNA4S.pitch * RDNA4S.ph + 65535) & ~(uint64_t)65535;
    {   // bundle 13/15 (M6 Stage 1b/2): is HDMI instance 1 (the monitor A) / 2 (the monitor B) enabled? ALL of: the kernel latches (both Stage-1b ones; the monitor A also garnet-m6flip1), both Mesa exports, THAT instance's kill file absent. The plane geometry is the
        // descriptor's (cross-checked by the Acquire), NOT the DP's scan query. Anything missing = that instance OFF, the DP and the other display are unaffected.
        RDNA4X.call = (rdna4x_call_t)dlsym(RDNA4R.lib, "radv_darwin_rdna4n_call"); RDNA4X.bohandle = (rdna4x_bohandle_t)dlsym(RDNA4R.lib, "radv_darwin_rdna4n_bo_handle");
        for (int xi = 1; xi <= 2; xi++) {
            const rdna4x_desc_t *const d = rdna4x_desc((uint32_t)xi); struct stat xs_;
            const rdna4x_enable_t xe = { .inst = (uint32_t)xi, .kernel_m6 = (q.flags & RDNA4N_SCANQ_M6) != 0, .kernel_m6flip = (q.flags & RDNA4X_SCANQ_M6FLIP) != 0, .kernel_m6flip1 = (q.flags & RDNA4X_SCANQ_M6FLIP1) != 0,
                                       .have_call = RDNA4X.call != NULL, .have_handle = RDNA4X.bohandle != NULL, .killfile = stat(d->killfile, &xs_) == 0 };
            RDNA4XI(xi).enabled = rdna4x_enabled(&xe);
            rdna4df_init(&RDNA4XI(xi).sm, 0);
            if (xe.kernel_m6flip) RDNA4LOG("scanout: instance %d (the %s): %s (kernel latches m6 %d m6flip %d m6flip1 %d, exports call %d bo_handle %d, kill file %s; geometry %ux%u pitch %u B from the descriptor, slot %llu B)", xi, d->name, RDNA4XI(xi).enabled ? "ENABLED (acquired lazily at the first frame the table maps to it)" : "OFF",
                   xe.kernel_m6, xe.kernel_m6flip, xe.kernel_m6flip1, xe.have_call, xe.have_handle, xe.killfile ? "PRESENT" : "absent", d->w, d->h, d->pitch_bytes, (unsigned long long)d->slot_bytes);
        }
    }
    RDNA4S.vtype = rdna4_find_mem(0xFFFFFFFFu, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    for (uint32_t i = 0; i < RDNA4R.mp.memoryTypeCount; i++)
        RDNA4LOG("scanout: RADV memory type %u: flags 0x%x heap %u (%llu MiB)%s", i, RDNA4R.mp.memoryTypes[i].propertyFlags, RDNA4R.mp.memoryTypes[i].heapIndex,
               (unsigned long long)(RDNA4R.mp.memoryHeaps[RDNA4R.mp.memoryTypes[i].heapIndex].size >> 20), (int)i == RDNA4S.vtype ? "  <- visible-pool VRAM (DEVICE_LOCAL|HOST_VISIBLE|HOST_COHERENT), used for the scanout slots" : "");
    if (RDNA4S.vtype < 0) { rdna4s_off_locked(RDNA4DF_R_ALLOC, "no DEVICE_LOCAL|HOST_VISIBLE|HOST_COHERENT memory type in RADV's list"); return; }
    RDNA4LOG("scanout: bound: plane %ux%u pitch %u B, slot %llu B (rounded to 64 KiB), visible-pool memory type %d; visible-pool free space is NOT queryable here (VK_EXT_memory_budget not used): SUSPECTED unknown, the first slot allocation reports it by succeeding or failing", RDNA4S.pw, RDNA4S.ph, RDNA4S.pitch, (unsigned long long)RDNA4S.slotBytes, RDNA4S.vtype);
}

// Caller backtrace test (once per texture creation): any frame resolving (dladdr) to a symbol with both "DisplaySurface" and "GetMTLTexture" (CoreDisplay's
// DisplaySurface::GetMTLTexture). The first few candidates log their frames so a miss can be diagnosed.
static BOOL rdna4s_bt_signal(BOOL logframes) {
    NSArray<NSNumber *> *a = [NSThread callStackReturnAddresses];
    BOOL hit = NO; char line[1600]; size_t o = 0; line[0] = 0;
    for (NSUInteger i = 0; i < a.count; i++) {
        Dl_info di; void *p = (void *)(uintptr_t)a[i].unsignedLongLongValue;
        if (!dladdr(p, &di)) continue;
        const char *sn = di.dli_sname ? di.dli_sname : "";
        if (strstr(sn, "DisplaySurface") && strstr(sn, "GetMTLTexture")) hit = YES;
        if (logframes && i < 16 && o + 200 < sizeof line) { const char *im = di.dli_fname ? strrchr(di.dli_fname, '/') : NULL; o += (size_t)snprintf(line + o, sizeof line - o, " [%lu]%s:%.70s", (unsigned long)i, im ? im + 1 : "?", sn); }
    }
    if (logframes) RDNA4LOG("scanout: caller backtrace:%s", line);
    return hit;
}

// Called at the end of -initWithDevice:descriptor:iosurface:plane: (RADV is open). YES = this texture is one of CoreDisplay's display surfaces.
// P4: the verdict (positive and negative) is cached per IOSurface id (key id + base + size + pitch + alloc, 64 entries, LRU) so the backtrace test (dladdr per frame) runs once per surface.
GARNET_INTERNAL void rdna4_scan_cc_counters(uint64_t *h, uint64_t *m, uint64_t *mm) {
    pthread_mutex_lock(&RDNA4S.mu); *h = RDNA4S.cc.hits; *m = RDNA4S.cc.misses; *mm = RDNA4S.cc.mismatches; pthread_mutex_unlock(&RDNA4S.mu);
}
GARNET_INTERNAL BOOL rdna4s_classify(size_t w, size_t h, uint32_t spf, NSUInteger usage, size_t bpr, size_t alloc, unsigned sid, const void *base) {
    if (spf != RDNA4S_FOURCC_BGRA || !(usage & MTLTextureUsageRenderTarget) || w < 1024 || h < 600) return NO;   // cheap prefilter: no lock, no log
    BOOL ok = NO;
    pthread_mutex_lock(&RDNA4S.mu);
    rdna4s_bind_locked();
    if (RDNA4S.sm.state != RDNA4DF_OFF) {
        int cv = 0;
        if (rdna4cc_lookup(&RDNA4S.cc, sid, (uint64_t)(uintptr_t)base, w, h, bpr, alloc, &cv)) ok = cv != 0;
        else {
            // bundle 15: the surface is a candidate when its size is the DP's plane OR an ENABLED HDMI instance's own geometry (the monitor A's 1920x1080 never matches the DP's plane)
            const uint32_t cmask = rdna4x_classify_mask(RDNA4S.pw, RDNA4S.ph, RDNA4S.pitch, rdna4x_onmask_locked(), (uint32_t)w, (uint32_t)h, (uint32_t)bpr);
            BOOL size = cmask != 0u, pitch = size && alloc >= (size_t)bpr * h;
            // Reviewer: the backtrace only when the geometry can match, and only POSITIVE verdicts are cached: a display surface whose first texture came from
            // another caller must still be found when CoreDisplay's GetMTLTexture asks for it later (a cached NO would freeze the screen on that surface).
            BOOL bt = (size && pitch) ? rdna4s_bt_signal(RDNA4S.ncand++ < 3) : NO, test = rdna4s_testmode();
            if (size && pitch) RDNA4LOG("scanout: display-surface candidate: IOSurface id %u %zux%zu bpr %zu, signals: format_BGRA=1 size_eq_plane=%d usage_RenderTarget=1 pitch_eq_plane=%d caller_DisplaySurface_GetMTLTexture=%d%s",
                   sid, w, h, bpr, size, pitch, bt, test ? " (RDNA4M_TEST_DISPFLIP: backtrace signal waived)" : "");
            ok = size && pitch && (bt || test);
            if (ok) rdna4cc_put(&RDNA4S.cc, sid, (uint64_t)(uintptr_t)base, w, h, bpr, alloc, 1);
            if (ok) { RDNA4S.ndisp++; RDNA4LOG("scanout: display surface #%d identified: IOSurface id %u (expect 3 per WindowServer; counted once per surface id, later textures of the same surface hit the classify cache)", RDNA4S.ndisp, sid); }
        }
    }
    if (ok) {   // bundle 15: remember the surface's geometry for the size gate (a cache hit records it too)
        unsigned k = 0; for (; k < 64u; k++) if (RDNA4S.sg[k].sid == sid) break;
        if (k == 64u) { k = RDNA4S.sgi++ % 64u; }
        RDNA4S.sg[k].sid = sid; RDNA4S.sg[k].w = (uint32_t)w; RDNA4S.sg[k].h = (uint32_t)h; RDNA4S.sg[k].bpr = (uint32_t)bpr;
    }
    pthread_mutex_unlock(&RDNA4S.mu);
    return ok;
}

static BOOL rdna4s_status_locked(struct rdna4n_scan_status *st) {
    int rc = RDNA4S.fs(RDNA4R.dev, st);
    if (rc) { char m[64]; snprintf(m, sizeof m, "scanout_status rc %d", rc); rdna4s_off_locked(RDNA4DF_R_ERROR, m); return NO; }
    if (!st->acquired || (st->flags & (RDNA4N_SCANST_RESTORING | RDNA4N_SCANST_STORM))) {
        char m[96]; snprintf(m, sizeof m, "the kernel ended the acquisition (acquired %u flags 0x%x watchdog_restores %llu)", st->acquired, st->flags, (unsigned long long)st->watchdog_restores);
        rdna4s_off_locked(RDNA4DF_R_PLANELOST, m); return NO; }
    return YES;
}
GARNET_INTERNAL _Atomic uint64_t rdna4s_imgcopies;   // P6: display copies recorded from an image source (a display surface with no CPU mapping)
static void rdna4s_log_counters_locked(const struct rdna4n_scan_status *st) {
    RDNA4LOG("scanout: counters: presents %llu (kernel presents %llu) latched %llu replaced %llu repeats %llu refused %llu watchdog_restores %llu drops %llu gpu_fail %llu stale_skipped %llu superseded_skipped %llu multi_disp_cbs %llu inflight %d display_surfaces %d hold %s H %d held %llu held_superseded %llu held_presented %llu hold_total_ms %.1f hold_mean_ms %.3f image_source_copies %llu present_busy %llu retry_presented %llu retry_dropped %llu",
           (unsigned long long)RDNA4S.sm.presents, (unsigned long long)st->presents, (unsigned long long)st->latched, (unsigned long long)st->replaced, (unsigned long long)st->repeats,
           (unsigned long long)st->refused, (unsigned long long)st->watchdog_restores, (unsigned long long)RDNA4S.sm.drops, (unsigned long long)RDNA4S.sm.gpu_fail, (unsigned long long)RDNA4S.sm.stale, (unsigned long long)RDNA4S.sm.superseded, (unsigned long long)atomic_load(&RDNA4S.multi), rdna4df_inflight_count(&RDNA4S.sm), RDNA4S.ndisp,
           rdna4s_hold_mode_name(atomic_load(&RDNA4H.mode)), atomic_load(&RDNA4H.ms), (unsigned long long)RDNA4H.held, (unsigned long long)RDNA4H.held_sup, (unsigned long long)RDNA4H.held_pres, (double)RDNA4H.hold_ns / 1e6, RDNA4H.held ? (double)RDNA4H.hold_ns / 1e6 / (double)RDNA4H.held : 0.0, (unsigned long long)atomic_load(&rdna4s_imgcopies), (unsigned long long)RDNA4S.sm.present_busy, (unsigned long long)RDNA4S.sm.retry_presented, (unsigned long long)RDNA4S.sm.retry_dropped);
}

static void rdna4s_crc_summary(void);
// native #12 P1: write-class summary of the last 10 s (interval = cumulative minus the previous snapshot), plus the cumulative nonfinal_skipped. Called with the mutex held.
static void rdna4s_class_summary_locked(void) {
    static uint64_t pc[RDNA4DF_NCLS], pn[RDNA4DF_NCLS];
    char l[RDNA4DF_NCLS * 80]; size_t o = 0; uint64_t tn = 0;
    for (int i = 0; i < RDNA4DF_NCLS; i++) { o += (size_t)snprintf(l + o, sizeof l - o, " %s %llu(skipped %llu)", rdna4df_cls_name[i], (unsigned long long)(RDNA4S.sm.cls[i] - pc[i]), (unsigned long long)(RDNA4S.sm.nonfinal[i] - pn[i])); pc[i] = RDNA4S.sm.cls[i]; pn[i] = RDNA4S.sm.nonfinal[i]; tn += RDNA4S.sm.nonfinal[i]; }
    RDNA4LOG("scanout: display writes by class (10 s):%s; nonfinal_skipped total %llu; nonscanout_skipped %llu; presentall %d", l, (unsigned long long)tn, (unsigned long long)RDNA4S.sm.nonscan_total, RDNA4S.sm.presentall);
    for (int i = 0; i < RDNA4DF_MAX_SURF; i++) if (RDNA4S.sm.nonscan_skip[i]) RDNA4LOG("scanout: nonscanout_skipped sid %u: %llu", RDNA4S.sm.nonscan_sid[i], (unsigned long long)RDNA4S.sm.nonscan_skip[i]);
}
// native #12 P1/P2 + D1: called from -rdna4Finish for the command buffer's chosen display write. Counts its class and its extents, logs the first 20 with the pipeline names and the
// first 30 extent samples, and returns whether it may take a slot.
GARNET_INTERNAL BOOL rdna4s_disp_account(uint32_t mask, uint32_t sid, const char *fns, uint64_t cbno, const rdna4df_wr_t *wr, int *pinst, int *ptent) {
    pthread_mutex_lock(&RDNA4S.mu);
    int pi = RDNA4X_PLAN_DP, tent = 0;
    if (RDNA4S.m6.latch) {   // bundle 11: a surface the kernel maps to another display (or saw on two pipes) takes no slot and is never copied; an unknown one is let through TENTATIVELY (the completion path decides)
        // bundle 13: a surface the table maps to the monitor B plans INSTANCE 2 (when enabled and not OFF); an UNKNOWN one only ever goes tentatively to instance 0, NEVER to instance 2
        const uint32_t xon = rdna4x_onmask_locked();
        pi = rdna4x_plan_inst(&RDNA4S.m6, sid, xon, &tent);
        if (pi == RDNA4X_PLAN_DP || pi == RDNA4X_PLAN_MONA || pi == RDNA4X_PLAN_MONB) {
            // bundle 15 / 17 (R-B1): THE SIZE GATE. A surface planned for an instance (a TENTATIVE DP plan included) whose size is not that instance's geometry takes no slot (the copy rectangle and the slot are the descriptor's).
            uint32_t gw = 0, gh = 0, gb = 0; int have = 0;
            for (unsigned k = 0; k < 64u; k++) if (RDNA4S.sg[k].sid == sid && sid != 0u) { gw = RDNA4S.sg[k].w; gh = RDNA4S.sg[k].h; gb = RDNA4S.sg[k].bpr; have = 1; break; }
            if (!have || !rdna4x_plan_geom_ok(pi, gw, gh, gb, RDNA4S.pw, RDNA4S.ph, RDNA4S.pitch)) { RDNA4XI(pi).xs.inst_geom_mismatch++; pthread_mutex_unlock(&RDNA4S.mu); return NO; }
            if (pi != RDNA4X_PLAN_DP) RDNA4XI(pi).plan_count++;
        } else { const int gv = rdna4m6_gate(&RDNA4S.m6, sid);   // counts (gate_ok / gate_tentative / gate_skip / gate_ambig); the verdict is pi's
               (void)gv; }
        if (pi == RDNA4X_PLAN_SKIP) { pthread_mutex_unlock(&RDNA4S.mu); return NO; }
        if (tent) RDNA4XI(0).xs.tentative_frames++;
    }
    if (pinst) *pinst = pi;
    if (ptent) *ptent = tent;
    const int apx = rdna4x_desc((uint32_t)pi) != NULL;
    rdna4df_t *const am = apx ? &RDNA4XI(pi).sm : &RDNA4S.sm;   // the machine of the planned instance accounts the frame (an HDMI machine is a full-copy machine)
    if (apx) am->presentall = RDNA4S.sm.presentall;
    int nf0 = am->nflag_log, ok = rdna4df_account_sid(am, mask, sid, RDNA4S.m6.latch && !tent), c = rdna4df_class(mask);   // bundle 20 (F1): latch on + planned NON-tentatively = the kernel's table maps the surface to this pipe: a scanout surface (no table entry needed)
    if (am->nflag_log > nf0) RDNA4LOG("scanout: scanout-surface flag set #%d: surface %u (a final-GPUPass draw targeted it)", am->nflag_log, sid);
    uint32_t W = RDNA4S.pw, H = RDNA4S.ph;
    if (wr && !apx) rdna4df_dmg_account(&RDNA4S.sm, c, wr, W, H);
    static int logged; 
    if (logged < 20) { logged++; RDNA4LOG("scanout: display write #%d: cb %llu surface %u class %s mask 0x%x -> %s; pipelines (vertex/fragment): %s", logged, (unsigned long long)cbno, sid, rdna4df_cls_name[c], mask, ok ? (c == RDNA4DF_C_FINAL ? "FINAL: copy+present" : "scanout composite/presentall: copy+present") : "NON-FINAL: no copy, no present", fns && fns[0] ? fns : "(none)"); atomic_store(&rdna4_dw_logged, logged); }
    static int dlog;
    if (wr && dlog < 30 && W && H) {
        dlog++; rdna4df_rect_t ra, rp; int ka = rdna4df_dmg_resolve(&wr->all, W, H, &ra), kp = rdna4df_dmg_resolve(&wr->pres, W, H, &rp);
        static const char *kn[3] = { "none", "FULL", "partial" };
        RDNA4LOG("scanout: DIRTY #%d: cb %llu surface %u class %s: all draws %u (full %u, partial %u) -> %s [%d,%d)-[%d,%d) (%lld px = %.1f%%)%s; presentable -> %s [%d,%d)-[%d,%d); first draw viewport %.0f,%.0f %.0fx%.0f scissor %lld,%lld %lldx%lld; sampled source: %s%s id %u %ux%u",
            dlog, (unsigned long long)cbno, sid, rdna4df_cls_name[c], wr->all.draws, wr->all.full_draws, wr->all.part_draws, kn[ka], ra.x0, ra.y0, ra.x1, ra.y1, (long long)rdna4df_rect_area(ra), 100.0 * (double)rdna4df_rect_area(ra) / ((double)W * H),
            wr->all.full ? " (a clear/blit/compute wrote the whole surface)" : "", kn[kp], rp.x0, rp.y0, rp.x1, rp.y1, wr->vp[0], wr->vp[1], wr->vp[2], wr->vp[3], (long long)wr->sc[0], (long long)wr->sc[1], (long long)wr->sc[2], (long long)wr->sc[3],
            wr->src_kind == 0 ? "none" : wr->src_kind == 1 ? "other IOSurface (not a display surface)" : wr->src_kind == 2 ? "ANOTHER display surface" : "THE TARGET ITSELF", "", wr->src_sid, wr->src_w, wr->src_h);
    }
    pthread_mutex_unlock(&RDNA4S.mu);
    return ok != 0;
}
// D1 summary of the last 10 s: per class the cb extent kinds, plus the GPUPass source-texture kinds and the D2 counters. Mutex held.
static void rdna4s_dirty_summary_locked(void) {
    static uint64_t pk[RDNA4DF_NCLS][3], pa[RDNA4DF_NCLS], pdf[RDNA4DF_NCLS], pdp[RDNA4DF_NCLS], ps[4], ppc[RDNA4DF_NCLS];
    char l[RDNA4DF_NCLS * 224]; size_t o = 0; double scr = (double)RDNA4S.pw * RDNA4S.ph;
    for (int i = 0; i < RDNA4DF_NCLS; i++) {
        uint64_t n0 = RDNA4S.sm.dk[i][0] - pk[i][0], n1 = RDNA4S.sm.dk[i][1] - pk[i][1], n2 = RDNA4S.sm.dk[i][2] - pk[i][2], ar = RDNA4S.sm.dk_area[i] - pa[i];
        uint64_t df = RDNA4S.sm.dd_full[i] - pdf[i], dp = RDNA4S.sm.dd_part[i] - pdp[i], pr = RDNA4S.sm.pres_cls[i] - ppc[i];
        if (n0 + n1 + n2 + pr) o += (size_t)snprintf(l + o, sizeof l - o, " [%s: cbs full %llu partial %llu none %llu, avg partial area %.1f%%, draws full %llu partial %llu, presented %llu]", rdna4df_cls_name[i], (unsigned long long)n1, (unsigned long long)n2, (unsigned long long)n0,
            n2 && scr > 0 ? 100.0 * (double)ar / (double)n2 / scr : 0.0, (unsigned long long)df, (unsigned long long)dp, (unsigned long long)pr);
        for (int k = 0; k < 3; k++) pk[i][k] = RDNA4S.sm.dk[i][k];
        pa[i] = RDNA4S.sm.dk_area[i]; pdf[i] = RDNA4S.sm.dd_full[i]; pdp[i] = RDNA4S.sm.dd_part[i]; ppc[i] = RDNA4S.sm.pres_cls[i];
    }
    RDNA4LOG("scanout: display extents (10 s):%s; GPUPass samples: none %llu other-IOSurface %llu another-display-surface %llu the-target-itself %llu", o ? l : " (no display writes)",
        (unsigned long long)(RDNA4S.sm.srck[0] - ps[0]), (unsigned long long)(RDNA4S.sm.srck[1] - ps[1]), (unsigned long long)(RDNA4S.sm.srck[2] - ps[2]), (unsigned long long)(RDNA4S.sm.srck[3] - ps[3]));
    for (int k = 0; k < 4; k++) ps[k] = RDNA4S.sm.srck[k];
    if (RDNA4S.sm.damage || RDNA4S.sm.dm_part || RDNA4S.sm.dm_full || RDNA4S.sm.dm_carry)
        RDNA4LOG("scanout: damage mode %s: partial frames %llu (empty rect %llu) full-copy frames %llu chain restarts %llu carried drops %llu poisoned %llu; region copies %llu, surface rows copied %llu px; chain head slot %d",
            RDNA4S.sm.damage ? "ON" : "off", (unsigned long long)RDNA4S.sm.dm_part, (unsigned long long)RDNA4S.sm.dm_empty, (unsigned long long)RDNA4S.sm.dm_full, (unsigned long long)RDNA4S.sm.dm_restart, (unsigned long long)RDNA4S.sm.dm_carry,
            (unsigned long long)RDNA4S.sm.chain_invalid, (unsigned long long)RDNA4S.sm.dm_regions, (unsigned long long)RDNA4S.sm.dm_copy_px, RDNA4S.sm.head);
}
// bundle 20 (F4): per HDMI instance, every 10 s: the write classes (interval) with the non-final skips, the table occupancy (scan flags of 8, nonscan entries of 8, slots carrying a supersede mark of 3) and the cumulative
// scan_full_refused / surf_untracked. A refused composite write is "skipped" under render-composite; occupancy 8/8 with scan_full_refused rising is the pre-bundle-20 saturation. Mutex held.
static void rdna4x_rubber_summary_locked(int xi) {
    static uint64_t pc[3][RDNA4DF_NCLS], pn[3][RDNA4DF_NCLS];
    rdna4xi_t *const X = &RDNA4XI(xi); char l[RDNA4DF_NCLS * 80]; size_t o = 0;
    for (int i = 0; i < RDNA4DF_NCLS; i++) {
        uint64_t c = X->sm.cls[i] - pc[xi][i], n = X->sm.nonfinal[i] - pn[xi][i]; pc[xi][i] = X->sm.cls[i]; pn[xi][i] = X->sm.nonfinal[i];
        if (c || n) o += (size_t)snprintf(l + o, sizeof l - o, " %s %llu(skipped %llu)", rdna4df_cls_name[i], (unsigned long long)c, (unsigned long long)n);
    }
    RDNA4LOG("scanout (instance %d, %s): display writes by class (10 s):%s; tables: scan flags %d/%d nonscan %d/%d superseded-marked slots %d/%d; scan_full_refused %llu surf_untracked %llu",
           xi, rdna4x_desc((uint32_t)xi)->name, o ? l : " (none)", X->sm.nscan, RDNA4DF_MAX_SURF, rdna4df_nonscan_used(&X->sm), RDNA4DF_MAX_SURF, rdna4df_marked_count(&X->sm), RDNA4DF_MAX_SLOTS,
           (unsigned long long)X->sm.scan_full_refused, (unsigned long long)X->sm.surf_untracked);
}
// bundle 13 (review R3, bundle half): a TENTATIVE frame (unknown surface, planned to instance 0) whose completion says the surface is the MONB's was dropped; its damage must not be lost. The surface was noted
// (rdna4x_recopy_note); here, at the next 1 s keep-alive tick, the surface's CURRENT content (the IOSurface memory: the bundle writes every GPU frame back into it before the completion) is copied to a free slot of THAT instance and presented
// through its own machine. RDNA4S.mu is held on entry and on return, DROPPED around the copy. A present of the instance that completes first repaints the whole surface and cancels it (complete_run). Counted; never blocks.
static void rdna4x_recopy_locked(int inst) {
    rdna4xi_t *const X = &RDNA4XI(inst); const rdna4x_desc_t *const d = rdna4x_desc((uint32_t)inst);
    rdna4x_stats_t *const x = &X->xs;
    const uint32_t sid = x->recopy_sid;
    if (X->sm.state != RDNA4DF_ACTIVE || !rdna4x_status_locked(inst, &X->st)) { rdna4x_recopy_done(x, 0); return; }
    uint32_t fl[RDNA4DF_MAX_SLOTS];
    for (int i = 0; i < RDNA4DF_MAX_SLOTS; i++) fl[i] = X->st.slot[X->sid[i] - d->tag].flags;
    const int slot = rdna4df_pick(&X->sm, fl);
    if (slot < 0) return;                                  // no free slot this tick: still pending, the next tick tries again
    const uint64_t seq = x->recopy_seq;                    // bundle 20 (F3): the NOTED frame's own sequence, not a fresh (newest) one: if any later frame of this display was presented since, this one is stale and is not shown (no backward jump)
    pthread_mutex_unlock(&RDNA4S.mu);
    int ok = 0, shown = 0, present_rc = 0;
    IOSurfaceRef ios = sid ? IOSurfaceLookup(sid) : NULL;
    if (ios && IOSurfaceGetWidth(ios) == d->w && IOSurfaceGetHeight(ios) == d->h && IOSurfaceGetBytesPerRow(ios) == d->pitch_bytes && IOSurfaceLock(ios, kIOSurfaceLockReadOnly, NULL) == kIOReturnSuccess) {   // the INSTANCE'S geometry
        void *dst = NULL;
        if (vkMapMemory(RDNA4R.dev, X->mem[slot], 0, VK_WHOLE_SIZE, 0, &dst) == VK_SUCCESS && dst) { memcpy(dst, IOSurfaceGetBaseAddress(ios), (size_t)rdna4x_frame_bytes(d)); vkUnmapMemory(RDNA4R.dev, X->mem[slot]); ok = 1; }
        IOSurfaceUnlock(ios, kIOSurfaceLockReadOnly, NULL);
    }
    if (ios) CFRelease(ios);
    pthread_mutex_lock(&RDNA4S.mu);
    if (ok && rdna4df_complete_seq(&X->sm, slot, 0, seq)) {
        uint64_t o[3] = { 0, 0, 0 };
        const int rc = rdna4x_k_present(inst, X->sid[slot], o);
        present_rc = rc;
        if (rc == 0) { X->presents_seen++; shown = 1; }
        if (rdna4df_present_result(&X->sm, rc)) { RDNA4LOG("scanout (instance %d): FAIL CLOSED / OFF (scanout-error): re-copy present rc %d", inst, rc); rdna4x_do_release_locked(inst); }
    } else (void)rdna4df_complete_seq(&X->sm, slot, 1, seq);      // not copied (or stale): the slot is released without a present
    rdna4x_recopy_done(x, ok);
    RDNA4LOG("scanout (instance %d, %s): re-copy of IOSurface %u (a tentative DP-planned frame turned out to be this display's): %s", inst, d->name, sid, shown ? "presented" : present_rc == -EBUSY ? "not shown (scanout busy)" : present_rc ? "FAILED (scanout error)" : ok ? "copied but NOT shown (stale: a newer frame of this display was presented since)" : "FAILED (the next present of this display repaints the whole surface)");
}
static void rdna4s_tick(void) {   // 1 s keep-alive (a status call counts as activity: no 5 s idle restore); counters every 10 s
    (void)rdna4s_crc_on();   // keeps the 2 s file poll alive on an idle screen
    rdna4s_poll_hold_switch(NO);
    pthread_mutex_lock(&RDNA4S.mu);
    { struct stat sb; int pa = stat(RDNA4_PRESENTALL_FILE, &sb) == 0; if (pa != RDNA4S.sm.presentall) { RDNA4S.sm.presentall = pa; RDNA4LOG("scanout: final-pass rule %s (" RDNA4_PRESENTALL_FILE " %s)", pa ? "DISABLED: every display write is copied and presented" : "ENABLED: only GPUPass frames are presented", pa ? "exists" : "removed"); } }
    { struct stat sb; int dm = stat(RDNA4_DAMAGE_FILE, &sb) == 0; if (dm != RDNA4S.sm.damage) { RDNA4S.sm.damage = dm; RDNA4S.sm.head = -1; RDNA4LOG("scanout: partial-update emulation %s (" RDNA4_DAMAGE_FILE " %s)", dm ? "ENABLED: SkyLight composite passes count as frames; each frame = previous frame + its dirty rectangle" : "DISABLED: full-surface copies", dm ? "exists" : "removed"); } }
    if (RDNA4S.sm.state == RDNA4DF_ACTIVE && rdna4s_status_locked(&RDNA4S.st)) {
        uint64_t now = rdna4_now();
        if (now - RDNA4S.tLog >= 10ull * 1000000000ull) { RDNA4S.tLog = now; rdna4s_log_counters_locked(&RDNA4S.st); rdna4s_class_summary_locked(); rdna4s_dirty_summary_locked(); rdna4s_crc_summary();
            for (int xi = 1; xi <= 2; xi++) { rdna4xi_t *const X = &RDNA4XI(xi); if (X->sm.state != RDNA4DF_ACTIVE) continue;
                RDNA4LOG("scanout (instance %d, %s): counters: presents %llu (kernel presents %llu latched %llu replaced %llu refused %llu watchdog_restores %llu) drops %llu gpu_fail %llu stale_skipped %llu superseded %llu; planned %llu inst_mismatch %llu inst_geom_mismatch %llu stale_retry %llu stale_drop %llu tentative %llu re-copy noted %llu done %llu failed %llu; status calls %llu (keep-alive %llu); table gen %u (cached %u, refused out-of-order %llu)", xi, rdna4x_descs[xi].name,
                (unsigned long long)X->sm.presents, (unsigned long long)X->st.presents, (unsigned long long)X->st.latched, (unsigned long long)X->st.replaced, (unsigned long long)X->st.refused, (unsigned long long)X->st.watchdog_restores,
                (unsigned long long)X->sm.drops, (unsigned long long)X->sm.gpu_fail, (unsigned long long)X->sm.stale, (unsigned long long)X->sm.superseded, (unsigned long long)X->plan_count, (unsigned long long)X->xs.inst_mismatch, (unsigned long long)X->xs.inst_geom_mismatch, (unsigned long long)X->xs.stale_retry, (unsigned long long)X->xs.stale_drop,
                (unsigned long long)X->xs.tentative_frames, (unsigned long long)X->xs.recopy_noted, (unsigned long long)X->xs.recopy_done, (unsigned long long)X->xs.recopy_fail, (unsigned long long)X->xs.status_calls, (unsigned long long)X->xs.keepalive_calls, X->gen, RDNA4S.m6.gen, (unsigned long long)RDNA4S.m6.store_refused);
                rdna4x_rubber_summary_locked(xi); } }
    }
    for (int xi = 1; xi <= 2; xi++) {   // bundle 13/15: each HDMI instance's 1 s duties (rdna4x_tick_plan, host-tested). The KEEP-ALIVE comes first: a display pipe idles when nothing changes (queue 292), so a STATIC display is a static plane, and the kernel's 5 s
        // idle watchdog would put it back on the bars unless a call counts as activity - Status is that call, once per ACTIVE instance. Then the kill file (this instance's own), a table refresh when the kernel has published a newer table than the cache, and a pending re-copy.
        rdna4xi_t *const X = &RDNA4XI(xi); const rdna4x_desc_t *const d = rdna4x_desc((uint32_t)xi);
        struct stat kb; const int kill = stat(d->killfile, &kb) == 0;
        if (kill && X->enabled) { X->enabled = 0; X->xs.killed++; RDNA4LOG("scanout (instance %d, %s): kill switch %s exists: this instance %s and stays OFF for this process", xi, d->name, d->killfile, X->sm.state == RDNA4DF_ACTIVE ? "is being released (the kernel puts A back)" : "is disabled");
            if (X->sm.state == RDNA4DF_ACTIVE) rdna4x_off_locked(xi, RDNA4DF_R_KILL, "kill switch file present"); }
        rdna4x_tick_t tk = { .x_active = X->sm.state == RDNA4DF_ACTIVE, .killfile = 0, .status_gen_known = X->gen_known, .status_gen = X->gen, .recopy_pending = X->xs.recopy_pending };
        uint32_t act = rdna4x_tick_plan(&tk, &RDNA4S.m6);
        if (act & RDNA4X_T_STATUS) {
            X->xs.keepalive_calls++;
            (void)rdna4x_status_locked(xi, &X->st);
            tk.x_active = X->sm.state == RDNA4DF_ACTIVE; tk.status_gen = X->gen; tk.status_gen_known = X->gen_known;
            act = rdna4x_tick_plan(&tk, &RDNA4S.m6);
            if (act & RDNA4X_T_REFRESH) rdna4s_m6_refresh_async();
            if (act & RDNA4X_T_RECOPY) rdna4x_recopy_locked(xi);
        }
    }
    pthread_mutex_unlock(&RDNA4S.mu);
}
static void rdna4s_atexit(void) {   // best effort; RDNA4N close restores the console FIRST anyway
    if (pthread_mutex_trylock(&RDNA4S.mu)) return;
    if (RDNA4S.sm.acquired && !RDNA4S.sm.released) { RDNA4S.sm.released = 1; rdna4s_do_release_locked(); }
    pthread_mutex_unlock(&RDNA4S.mu);
}
static void rdna4s_free_slots(void) {
    for (int i = 0; i < RDNA4DF_MAX_SLOTS; i++) {
        if (RDNA4S.buf[i]) { vkDestroyBuffer(RDNA4R.dev, RDNA4S.buf[i], NULL); RDNA4S.buf[i] = VK_NULL_HANDLE; }
        if (RDNA4S.mem[i]) { vkFreeMemory(RDNA4R.dev, RDNA4S.mem[i], NULL); RDNA4S.mem[i] = VK_NULL_HANDLE; }
        RDNA4S.map[i] = NULL;
        RDNA4S.allocBytes[i] = 0; RDNA4S.reqBytes[i] = 0; RDNA4S.alignBytes[i] = 0; RDNA4S.memType[i] = -1;
    }
    RDNA4S.preallocBytes = 0;
}
static BOOL rdna4s_alloc_slots_locked(char *why, size_t whylen) {
    int i = 0;
    for (; i < RDNA4DF_MAX_SLOTS; i++) {
        VkBufferCreateInfo bc = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = RDNA4S.slotBytes, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
        VkResult r = vkCreateBuffer(RDNA4R.dev, &bc, NULL, &RDNA4S.buf[i]);
        if (r != VK_SUCCESS) { RDNA4S.buf[i] = VK_NULL_HANDLE; snprintf(why, whylen, "slot %d vkCreateBuffer = %d", i, r); break; }
        VkMemoryRequirements mr; vkGetBufferMemoryRequirements(RDNA4R.dev, RDNA4S.buf[i], &mr);
        int mt = rdna4_find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (mt < 0) { snprintf(why, whylen, "slot %d: no visible-VRAM type in the buffer's bits 0x%x", i, mr.memoryTypeBits); break; }
        VkMemoryAllocateInfo ma = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = (mr.size + 65535) & ~(VkDeviceSize)65535, .memoryTypeIndex = (uint32_t)mt };
        r = vkAllocateMemory(RDNA4R.dev, &ma, NULL, &RDNA4S.mem[i]);
        if (r != VK_SUCCESS) { RDNA4S.mem[i] = VK_NULL_HANDLE; snprintf(why, whylen, "slot %d vkAllocateMemory(%llu B, type %d) = %d (visible pool full?)", i, (unsigned long long)ma.allocationSize, mt, r); break; }
        r = vkBindBufferMemory(RDNA4R.dev, RDNA4S.buf[i], RDNA4S.mem[i], 0);
        if (r != VK_SUCCESS) { snprintf(why, whylen, "slot %d vkBindBufferMemory = %d", i, r); break; }
        RDNA4S.allocBytes[i] = ma.allocationSize;
        RDNA4S.reqBytes[i] = mr.size;
        RDNA4S.alignBytes[i] = mr.alignment;
        RDNA4S.memType[i] = mt;
    }
    if (i < RDNA4DF_MAX_SLOTS) { rdna4s_free_slots(); return NO; }
    RDNA4S.preallocBytes = RDNA4S.slotBytes;
    return YES;
}
GARNET_INTERNAL void rdna4s_prebind_on_radv_open(void) {
    if (!rdna4_is_ws() && !rdna4s_testmode()) return;
    pthread_mutex_lock(&RDNA4S.mu);
    if (!RDNA4S.bound && RDNA4S.sm.state == RDNA4DF_UNBOUND && !RDNA4S.preallocBytes) {
        struct stat sb;
        if (stat(RDNA4_NOFLIP_FILE, &sb) != 0) {
            rdna4s_query_t fq = (rdna4s_query_t)dlsym(RDNA4R.lib, "radv_darwin_scanout_query");
            struct rdna4n_scan_query q; memset(&q, 0, sizeof q);
            if (fq && fq(RDNA4R.dev, &q) == 0 && (q.flags & RDNA4N_SCANQ_GEOM_OK) && q.plane_w && q.plane_h && q.pitch_px >= q.plane_w) {
                uint32_t pitch = q.pitch_px * 4;
                uint64_t slotBytes = ((uint64_t)pitch * q.plane_h + 65535) & ~(uint64_t)65535;
                if (slotBytes) {
                    RDNA4S.slotBytes = slotBytes;
                    char why[160] = "";
                    if (rdna4s_alloc_slots_locked(why, sizeof why)) {
                        RDNA4LOG("scanout: pre-allocated %d visible-VRAM slots (%llu B each, plane %ux%u pitch %u B) at RADV open",
                                 RDNA4DF_MAX_SLOTS, (unsigned long long)slotBytes, q.plane_w, q.plane_h, pitch);
                    } else {
                        RDNA4S.slotBytes = 0;
                        RDNA4LOG("scanout: early slot pre-allocation failed (will retry at acquire): %s", why);
                    }
                }
            }
        }
    }
    pthread_mutex_unlock(&RDNA4S.mu);
}
// Lazy acquire at the first write of a display surface: acquire the plane, 3 slots in visible VRAM, register them. Mutex held.
static BOOL rdna4s_acquire_locked(void) {
    if (RDNA4S.sm.state != RDNA4DF_UNBOUND) return RDNA4S.sm.state == RDNA4DF_ACTIVE;
    uint64_t cmc = 0, fc = 0;
    int rc = RDNA4S.fa(RDNA4R.dev, &cmc, &fc);
    if (rc) { char m[64]; snprintf(m, sizeof m, "scanout_acquire rc %d", rc); rdna4s_off_locked(RDNA4DF_R_ERROR, m); return NO; }
    rdna4df_mark_acquired(&RDNA4S.sm);
    int i = 0; char why[160] = "";
    const BOOL havePrealloc = (RDNA4S.preallocBytes == RDNA4S.slotBytes && RDNA4S.preallocBytes != 0 &&
                               RDNA4S.buf[0] && RDNA4S.buf[1] && RDNA4S.buf[2] &&
                               RDNA4S.mem[0] && RDNA4S.mem[1] && RDNA4S.mem[2]);
    if (havePrealloc) {
        RDNA4LOG("scanout: reusing %d pre-allocated visible-VRAM slots (%llu B each)", RDNA4DF_MAX_SLOTS, (unsigned long long)RDNA4S.slotBytes);
    } else {
        if (RDNA4S.preallocBytes || RDNA4S.buf[0] || RDNA4S.mem[0]) rdna4s_free_slots();
        if (!rdna4s_alloc_slots_locked(why, sizeof why)) { rdna4s_off_locked(RDNA4DF_R_ALLOC, why); return NO; }
    }
    for (; i < RDNA4DF_MAX_SLOTS; i++) {
        uint64_t mc = 0; uint32_t sl = 0;
        rc = RDNA4S.fg(RDNA4R.dev, RDNA4S.mem[i], 0, RDNA4S.pitch, RDNA4S.pw, RDNA4S.ph, &sl, &mc);
        if (rc) { snprintf(why, sizeof why, "slot %d scanout_register rc %d", i, rc); break; }
        if (sl >= RDNA4N_SCAN_MAX_SLOTS) { snprintf(why, sizeof why, "slot %d: kernel slot id %u out of range", i, sl); break; }
        RDNA4S.sid[i] = sl;
        RDNA4LOG("scanout: slot %d: memory type %d, %llu B (buffer req %llu align %llu), kernel slot id %u, MC 0x%llx (64 KiB aligned: %d)", i, RDNA4S.memType[i], (unsigned long long)RDNA4S.allocBytes[i],
               (unsigned long long)RDNA4S.reqBytes[i], (unsigned long long)RDNA4S.alignBytes[i], sl, (unsigned long long)mc, (mc & 0xFFFF) == 0);
    }
    if (i < RDNA4DF_MAX_SLOTS) { rdna4s_off_locked(RDNA4DF_R_ALLOC, why); rdna4s_free_slots(); return NO; }   // released first: nothing was submitted, so freeing is safe
    rdna4df_activate(&RDNA4S.sm, RDNA4DF_MAX_SLOTS);
    RDNA4S.tLog = rdna4_now();
    dispatch_source_t t = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
    dispatch_source_set_timer(t, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), NSEC_PER_SEC, NSEC_PER_SEC / 10);
    dispatch_source_set_event_handler(t, ^{ rdna4s_tick(); });
    RDNA4S.timer = t; dispatch_resume(t);
    if (!RDNA4S.exitSet) { RDNA4S.exitSet = 1; atexit(rdna4s_atexit); }
    RDNA4LOG("scanout ACQUIRED, %d slots (console MC 0x%llx, frame count %llu, %ux%u pitch %u)", RDNA4DF_MAX_SLOTS, (unsigned long long)cmc, (unsigned long long)fc, RDNA4S.pw, RDNA4S.ph, RDNA4S.pitch);
    return YES;
}
// bundle 13/15: Status of HDMI instance `inst` (the keep-alive: a call counts as activity for the kernel's 5 s idle watchdog). RDNA4S.mu held. Stores the table generation (what the cached table is compared with).
static BOOL rdna4x_status_locked(int inst, struct rdna4n_scan_status *st) {
    rdna4xi_t *const X = &RDNA4XI(inst);
    uint64_t so[4] = { 0, 0, 0, 0 };
    const int rc = rdna4x_k_status(inst, st, so);
    X->xs.status_calls++;
    if (rc) { char m[64]; snprintf(m, sizeof m, "scanout status (instance %d) rc %d", inst, rc); rdna4x_off_locked(inst, RDNA4DF_R_ERROR, m); return NO; }
    X->gen = (uint32_t)so[0]; X->gen_known = 1;
    if (!st->acquired || (st->flags & (RDNA4N_SCANST_RESTORING | RDNA4N_SCANST_STORM))) {
        char m[120]; snprintf(m, sizeof m, "the kernel ended instance %d's acquisition (acquired %u flags 0x%x watchdog_restores %llu)", inst, st->acquired, st->flags, (unsigned long long)st->watchdog_restores);
        rdna4x_off_locked(inst, RDNA4DF_R_PLANELOST, m); return NO; }
    return YES;
}
static void rdna4x_free_slots(int inst) {
    rdna4xi_t *const X = &RDNA4XI(inst);
    for (int i = 0; i < RDNA4DF_MAX_SLOTS; i++) {
        if (X->buf[i]) { vkDestroyBuffer(RDNA4R.dev, X->buf[i], NULL); X->buf[i] = VK_NULL_HANDLE; }
        if (X->mem[i]) { vkFreeMemory(RDNA4R.dev, X->mem[i], NULL); X->mem[i] = VK_NULL_HANDLE; }
    }
}
// Lazy acquire of an HDMI instance at the first frame the table maps to it: rdna4x_acquire_run (garnet_m6x.h, host-tested with failure injection at every step) over the real operations below: SCANX_ACQUIRE (+ the geometry cross-check),
// the pool budget (the kernel's FREE visible-VRAM figure), 3 slots of THE INSTANCE'S slot size in visible VRAM, SCANX_REGISTER each. Mutex held. Instance 0 must be ACTIVE (the kernel requires the session to hold it). ANY failure leaves
// THAT instance OFF and releases what it took; NO operation here touches the DP (instance 0 is only ever ACQUIRED first, through its own rdna4s_acquire_locked) or the other display. The slots are freed only while nothing was submitted.
typedef struct { int inst; uint64_t ao[5]; uint32_t nout; char why[160]; } rdna4x_acq_t;
static int rdna4x_op_pool(void *c) {
    const rdna4x_acq_t *a = c; rdna4xi_t *const X = &RDNA4XI(a->inst); const rdna4x_desc_t *const d = rdna4x_desc((uint32_t)a->inst);
    const rdna4x_pool_t pool = { .free_bytes = a->ao[3], .slot_bytes = d->slot_bytes, .slots = RDNA4DF_MAX_SLOTS, .margin_bytes = RDNA4X_MARGIN_BYTES };   // the kernel's FREE visible-VRAM figure (out[3]) - NOT RADV's heap size (review S4)
    const int pv = rdna4x_pool_verdict(&pool);
    if (pv != RDNA4X_POOL_OK) { RDNA4LOG("scanout (instance %d, %s): the free visible VRAM (%llu MiB, the kernel's figure) does not cover this display's %u slots + the %llu MiB margin (need %llu MiB: verdict %d)", a->inst, d->name, (unsigned long long)(pool.free_bytes >> 20), RDNA4DF_MAX_SLOTS, (unsigned long long)(RDNA4X_MARGIN_BYTES >> 20), (unsigned long long)(rdna4x_pool_need(&pool) >> 20), pv); X->xs.alloc_fail++; }
    else RDNA4LOG("scanout (instance %d, %s): free visible VRAM %llu MiB covers this display's %u slots + margin (need %llu MiB)", a->inst, d->name, (unsigned long long)(pool.free_bytes >> 20), RDNA4DF_MAX_SLOTS, (unsigned long long)(rdna4x_pool_need(&pool) >> 20));
    return pv;
}
static int rdna4x_op_acquire(void *c) { rdna4x_acq_t *a = c; rdna4xi_t *const X = &RDNA4XI(a->inst); const int rc = rdna4x_k_acquire(a->inst, a->ao, &a->nout); if (rc) { RDNA4LOG("scanout (instance %d): SCANX_ACQUIRE rc %d", a->inst, rc); X->xs.acquire_fail++; } else { X->gen = (uint32_t)a->ao[2]; X->gen_known = 1; } return rc; }
static int rdna4x_op_geom(void *c) {
    const rdna4x_acq_t *a = c; const rdna4x_desc_t *const d = rdna4x_desc((uint32_t)a->inst);
    const int ok = rdna4x_acq_geom_ok(d, a->nout, a->ao[4]);
    if (!ok) RDNA4LOG("scanout (instance %d, %s): the kernel's plane geometry word 0x%llx (%u output words) is NOT %ux%u pitch %u B: instance OFF", a->inst, d->name, (unsigned long long)a->ao[4], a->nout, d->w, d->h, d->pitch_bytes);
    return ok ? 0 : -1;
}
static void rdna4x_op_mark(void *c) { const rdna4x_acq_t *a = c; rdna4df_mark_acquired(&RDNA4XI(a->inst).sm); }
static int rdna4x_op_alloc(void *c, int i) {
    rdna4x_acq_t *a = c; rdna4xi_t *const X = &RDNA4XI(a->inst); const rdna4x_desc_t *const d = rdna4x_desc((uint32_t)a->inst);
    VkBufferCreateInfo bc = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = d->slot_bytes, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkResult r = vkCreateBuffer(RDNA4R.dev, &bc, NULL, &X->buf[i]);
    if (r != VK_SUCCESS) { X->buf[i] = VK_NULL_HANDLE; snprintf(a->why, sizeof a->why, "%s slot %d vkCreateBuffer = %d", d->name, i, r); X->xs.alloc_fail++; return -1; }
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(RDNA4R.dev, X->buf[i], &mr);
    int mt = rdna4_find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt < 0) { snprintf(a->why, sizeof a->why, "%s slot %d: no visible-VRAM type in the buffer's bits 0x%x", d->name, i, mr.memoryTypeBits); X->xs.alloc_fail++; return -1; }
    VkMemoryAllocateInfo ma = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = (mr.size + 65535) & ~(VkDeviceSize)65535, .memoryTypeIndex = (uint32_t)mt };
    r = vkAllocateMemory(RDNA4R.dev, &ma, NULL, &X->mem[i]);
    if (r != VK_SUCCESS) { X->mem[i] = VK_NULL_HANDLE; snprintf(a->why, sizeof a->why, "%s slot %d vkAllocateMemory(%llu B, type %d) = %d (visible pool full?)", d->name, i, (unsigned long long)ma.allocationSize, mt, r); X->xs.alloc_fail++; return -1; }
    r = vkBindBufferMemory(RDNA4R.dev, X->buf[i], X->mem[i], 0);
    if (r != VK_SUCCESS) { snprintf(a->why, sizeof a->why, "%s slot %d vkBindBufferMemory = %d", d->name, i, r); X->xs.alloc_fail++; return -1; }
    return 0;
}
static int rdna4x_op_register(void *c, int i) {
    rdna4x_acq_t *a = c; rdna4xi_t *const X = &RDNA4XI(a->inst); const rdna4x_desc_t *const d = rdna4x_desc((uint32_t)a->inst); uint64_t mc = 0; uint32_t sl = 0;
    const int rc = rdna4x_k_register(a->inst, X->mem[i], d->pitch_bytes, d->w, d->h, &sl, &mc);   // the registered rectangle is the INSTANCE'S
    if (rc) { snprintf(a->why, sizeof a->why, "%s slot %d SCANX_REGISTER rc %d", d->name, i, rc); X->xs.alloc_fail++; return -1; }
    if (!rdna4x_id_ok((uint32_t)a->inst, sl)) { snprintf(a->why, sizeof a->why, "%s slot %d: kernel slot id %#x is not a tagged instance-%d id", d->name, i, sl, a->inst); X->xs.alloc_fail++; return -1; }
    X->sid[i] = sl;
    RDNA4LOG("scanout (instance %d, %s): slot %d: %llu B, kernel slot id %#x, MC 0x%llx (64 KiB aligned: %d)", a->inst, d->name, i, (unsigned long long)d->slot_bytes, sl, (unsigned long long)mc, (mc & 0xFFFF) == 0);
    return 0;
}
static void rdna4x_op_off(void *c, int reason, const char *why) { rdna4x_acq_t *a = c; rdna4x_off_locked(a->inst, reason, a->why[0] ? a->why : why); }
static void rdna4x_op_free(void *c) { const rdna4x_acq_t *a = c; rdna4x_free_slots(a->inst); }
static void rdna4x_op_activate(void *c) {
    rdna4x_acq_t *a = c; rdna4xi_t *const X = &RDNA4XI(a->inst);
    rdna4df_activate(&X->sm, RDNA4DF_MAX_SLOTS);
    X->sm.presentall = RDNA4S.sm.presentall;
    RDNA4LOG("scanout (instance %d, %s) ACQUIRED, %d slots (console A 0x%llx, frame count %llu, M6 table generation %u)", a->inst, rdna4x_desc((uint32_t)a->inst)->name, RDNA4DF_MAX_SLOTS, (unsigned long long)a->ao[0], (unsigned long long)a->ao[1], X->gen);
}
static BOOL rdna4x_acquire_locked(int inst) {
    rdna4xi_t *const X = &RDNA4XI(inst);
    if (X->sm.state != RDNA4DF_UNBOUND) return X->sm.state == RDNA4DF_ACTIVE;
    if (!X->enabled) { rdna4df_fail(&X->sm, RDNA4DF_R_NOSYS); return NO; }
    if (RDNA4S.sm.state == RDNA4DF_UNBOUND) rdna4s_acquire_locked();
    if (RDNA4S.sm.state != RDNA4DF_ACTIVE) { rdna4x_off_locked(inst, RDNA4DF_R_ERROR, "instance 0 (the DP) is not active: the kernel requires the session to hold it first"); return NO; }
    rdna4x_acq_t acq; memset(&acq, 0, sizeof acq); acq.inst = inst;
    const rdna4x_ops_t ops = { .ctx = &acq, .pool_verdict = rdna4x_op_pool, .k_acquire = rdna4x_op_acquire, .mark_acquired = rdna4x_op_mark, .geom_verdict = rdna4x_op_geom, .alloc_slot = rdna4x_op_alloc, .register_slot = rdna4x_op_register, .off = rdna4x_op_off, .free_slots = rdna4x_op_free, .activate = rdna4x_op_activate };
    return rdna4x_acquire_run(&ops) == 0;
}
GARNET_INTERNAL int rdna4s_damage_on(void) { pthread_mutex_lock(&RDNA4S.mu); int d = RDNA4S.sm.damage; pthread_mutex_unlock(&RDNA4S.mu); return d; }
// At command-buffer encode: a free slot (fresh kernel status says REUSABLE, none in flight, not pinned, not the chain head while damage is on) or -1 (drop counted, or D-copy off).
// `wr` = what this cb wrote to the display surface (for the D2 rectangle). The plan says full copy or base slot + region.
GARNET_INTERNAL int rdna4s_plan(const rdna4df_wr_t *wr, int inst, int tent, rdna4df_plan_t *pl) {
    int slot = -1; pl->slot = -1; pl->base = -1; pl->id = pl->baseid = 0; pl->kind = RDNA4DF_K_FULL; pl->inst = (inst == RDNA4X_PLAN_MONA || inst == RDNA4X_PLAN_MONB) ? inst : 0; pl->tent = tent ? 1 : 0;
    pthread_mutex_lock(&RDNA4S.mu);
    if (inst == RDNA4X_PLAN_MONA || inst == RDNA4X_PLAN_MONB) {   // bundle 13/15: an HDMI display: its own machine, its own slots, always a FULL copy of the whole surface, in THE INSTANCE'S geometry
        rdna4xi_t *const X = &RDNA4XI(inst); const rdna4x_desc_t *const d = rdna4x_desc((uint32_t)inst);
        if (X->sm.state == RDNA4DF_UNBOUND) rdna4x_acquire_locked(inst);
        if (X->sm.state == RDNA4DF_ACTIVE && rdna4x_status_locked(inst, &X->st)) {
            uint32_t fl[RDNA4DF_MAX_SLOTS];
            for (int i = 0; i < RDNA4DF_MAX_SLOTS; i++) fl[i] = X->st.slot[X->sid[i] - d->tag].flags;
            if (RDNA4S.m6.latch && rdna4m6_table_stale(&RDNA4S.m6, X->gen)) rdna4s_m6_refresh_async();   // the kernel published a newer table than the cache holds: re-read it now (the completion also checks)
            const rdna4df_rect_t r = { 0, 0, (int32_t)d->w, (int32_t)d->h };
            slot = rdna4df_plan_ex(&X->sm, fl, RDNA4DF_K_FULL, r, 0, pl);
            pl->inst = inst;
        }
        pthread_mutex_unlock(&RDNA4S.mu);
        return slot;
    }
    if (RDNA4S.sm.state == RDNA4DF_UNBOUND) rdna4s_acquire_locked();
    if (RDNA4S.sm.state == RDNA4DF_ACTIVE && rdna4s_status_locked(&RDNA4S.st)) {
        uint32_t fl[RDNA4DF_MAX_SLOTS];
        for (int i = 0; i < RDNA4DF_MAX_SLOTS; i++) fl[i] = RDNA4S.st.slot[RDNA4S.sid[i]].flags;
        rdna4df_rect_t r = { 0, 0, (int32_t)RDNA4S.pw, (int32_t)RDNA4S.ph }; int k = wr ? rdna4df_dmg_resolve(&wr->pres, RDNA4S.pw, RDNA4S.ph, &r) : RDNA4DF_K_FULL;
        if (k == RDNA4DF_K_NONE) k = RDNA4DF_K_FULL;   // a presentable cb with no recorded draw rectangle: unknown, copy it all
        k = rdna4x_kind_for(tent, RDNA4DF_K_FULL, k);   // bundle 13 (R6): a TENTATIVE frame is a full copy
        slot = rdna4df_plan_ex(&RDNA4S.sm, fl, k, r, tent, pl);   // ... and never becomes the chain head
        pl->inst = 0;
        if (slot >= 0 && pl->base >= 0) { RDNA4S.sm.dm_copy_px += (uint64_t)rdna4df_rect_area(pl->rect); }
    }
    pthread_mutex_unlock(&RDNA4S.mu);
    return slot;
}
GARNET_INTERNAL void rdna4s_record_copy(VkCommandBuffer cmd, VkBuffer src, VkDeviceSize srcOffset, const rdna4df_plan_t *pl) {
    int slot = pl->slot;
    const rdna4x_desc_t *const xd = rdna4x_desc((uint32_t)pl->inst);   // bundle 15: an HDMI plan copies the INSTANCE'S rectangle (its pitch x height), never the DP's
    VkDeviceSize all = xd ? (VkDeviceSize)rdna4x_frame_bytes(xd) : (VkDeviceSize)RDNA4S.pitch * RDNA4S.ph;
    VkBuffer *const dstv = xd ? RDNA4XI(pl->inst).buf : RDNA4S.buf;   // the planned instance's slot buffers
    if (pl->base < 0) { VkBufferCopy bc = { .srcOffset = srcOffset, .dstOffset = 0, .size = all }; vkCmdCopyBuffer(cmd, src, dstv[slot], 1, &bc); return; }
    // D2: the slot first gets the previous frame (VRAM -> VRAM), then the dirty rows of the surface on top of it.
    VkBufferCopy bc = { .srcOffset = 0, .dstOffset = 0, .size = all }; vkCmdCopyBuffer(cmd, RDNA4S.buf[pl->base], RDNA4S.buf[slot], 1, &bc);
    static rdna4df_region_t rg[2048]; static VkBufferCopy vc[2048]; static pthread_mutex_t rmu = PTHREAD_MUTEX_INITIALIZER;   // recording is concurrent across command buffers: one scratch under a lock
    pthread_mutex_lock(&rmu);
    uint32_t n = rdna4df_regions(pl->rect, RDNA4S.pw, RDNA4S.ph, RDNA4S.pitch, 4, rg, 2048);
    if (n == UINT32_MAX) { n = 1; vc[0] = (VkBufferCopy){ srcOffset, 0, all }; }   // cannot happen with 1440 rows; fail towards the whole frame
    else for (uint32_t i = 0; i < n; i++) vc[i] = (VkBufferCopy){ srcOffset + rg[i].src, rg[i].dst, rg[i].size };
    if (n) { VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);   // WAW: the region copy must land after the base copy
        vkCmdCopyBuffer(cmd, src, RDNA4S.buf[slot], n, vc); }
    RDNA4S.sm.dm_regions += n;   // statistic only (a racy increment is acceptable)
    pthread_mutex_unlock(&rmu);
}
// ---- opt-in per-frame CRC diagnostic (NATIVE-S5-FLIP.md "Tearing diagnostic (CRC)"). Off unless /private/tmp/rdna4m-crc exists (polled every 2 s, WindowServer only; the
// file may hold a row step (default 64) and the word "noslot" = skip the slot read, which is the expensive part: the slot is write-combined BAR memory). Per frame that carries a copy: A1 = sampled CRC of the display surface's import memory after the fence, B = the same rows of the
// slot, A2 = the surface again after B, C = the surface again just before scanout_present. Reads only; nothing here changes what is copied or presented.
#define RDNA4_CRC_FILE "/private/tmp/rdna4m-crc"
GARNET_INTERNAL RDNA4CRCState RDNA4C = { .mu = PTHREAD_MUTEX_INITIALIZER };
GARNET_INTERNAL BOOL rdna4s_crc_on(void) {
    if (!rdna4_is_ws()) return NO;
    uint64_t now = rdna4_now(), nx = atomic_load(&RDNA4C.next);
    if (now >= nx && atomic_compare_exchange_strong(&RDNA4C.next, &nx, now + 2000000000ull)) {
        int fd = open(RDNA4_CRC_FILE, O_RDONLY | O_CLOEXEC), was = atomic_load(&RDNA4C.on);
        if (fd >= 0) {
            char b[48] = { 0 }; ssize_t n = read(fd, b, sizeof b - 1); close(fd); long v = n > 0 ? atol(b) : 0;
            atomic_store(&RDNA4C.noslot, n > 0 && strstr(b, "noslot") != NULL);
            atomic_store(&RDNA4C.step, (v >= 1 && v <= 1440) ? (uint32_t)v : RDNA4CRC_DEFAULT_STEP);
            if (!was) { pthread_mutex_lock(&RDNA4C.mu); if (!RDNA4C.warmed) { RDNA4C.warmed = 1; (void)rdna4crc32(0, "", 0); } memset(&RDNA4C.st, 0, sizeof RDNA4C.st); atomic_store(&RDNA4C.logged_p, 0); pthread_mutex_unlock(&RDNA4C.mu);
                atomic_store(&RDNA4C.on, 1); RDNA4LOG("scanout: CRC diagnostic ENABLED (" RDNA4_CRC_FILE " exists; row step %u%s; first 20 mismatches per kind are logged, summary every 10 s)", atomic_load(&RDNA4C.step), atomic_load(&RDNA4C.noslot) ? ", slot read OFF (noslot)" : ""); }
        } else if (was) { atomic_store(&RDNA4C.on, 0); RDNA4LOG("scanout: CRC diagnostic DISABLED (" RDNA4_CRC_FILE " removed)"); }
    }
    return atomic_load(&RDNA4C.on) != 0;
}
// Called from the 10 s counters line: one summary line, then the interval counters restart.
static void rdna4s_crc_summary(void) {
    pthread_mutex_lock(&RDNA4C.mu);
    if (RDNA4C.st.checked || RDNA4C.st.present_checked || atomic_load(&RDNA4C.on)) { char l[400]; rdna4crc_summary(&RDNA4C.st, l, sizeof l); RDNA4LOG("scanout: CRC summary (10 s): %s", l); }
    uint64_t lg = RDNA4C.st.logged; memset(&RDNA4C.st, 0, sizeof RDNA4C.st); RDNA4C.st.logged = lg;
    pthread_mutex_unlock(&RDNA4C.mu);
}
// Per display surface: the highest submission sequence of any command buffer that wrote it (called under the submit lock).. A frame is "later-overwritten" when this holds a higher sequence than its own.
GARNET_INTERNAL void rdna4s_crc_note(uint32_t sid, uint64_t seq) {
    pthread_mutex_lock(&RDNA4C.mu); int k = -1;
    for (int i = 0; i < 8; i++) if (RDNA4C.surf[i].sid == sid) { k = i; break; }
    if (k < 0) for (int i = 0; i < 8; i++) if (!RDNA4C.surf[i].sid) { k = i; RDNA4C.surf[i].sid = sid; break; }
    if (k >= 0 && seq > RDNA4C.surf[k].seq) RDNA4C.surf[k].seq = seq;
    pthread_mutex_unlock(&RDNA4C.mu);
}
// C: immediately before scanout_present (the present block holds the scanout mutex).
static void rdna4s_crc_present(const rdna4crc_rec_t *r) {
    uint8_t scratch[16384]; if (r->rowbytes > sizeof scratch) return;
    rdna4crc_samp_t c; uint64_t t0 = rdna4_now(); rdna4crc_sample(&c, r->base, r->pitch, r->rowbytes, r->h, r->step, scratch); uint64_t dt = rdna4_now() - t0;
    pthread_mutex_lock(&RDNA4C.mu);
    int later = 0; for (int i = 0; i < 8; i++) if (RDNA4C.surf[i].sid == r->sid) { later = RDNA4C.surf[i].seq > r->seq; break; }
    int chg = rdna4crc_account_present(&RDNA4C.st, &r->a1, &c, dt, r->unst, later);
    pthread_mutex_unlock(&RDNA4C.mu);
    if (chg && atomic_fetch_add(&RDNA4C.logged_p, 1) < 20) {
        uint32_t d[8]; int nd = rdna4crc_diff(&r->a1, &c, d, 8);
        RDNA4LOG("scanout: CRC MISMATCH src-changed-before-present: seq %llu surface %u slot %d (%s), %d sampled rows differ (step %u), first y: %u %u %u %u", (unsigned long long)r->seq, r->sid, r->slot, later ? "a LATER command buffer for this surface was submitted" : "NO later command buffer: written outside our command buffers", nd, r->step, d[0], nd > 1 ? d[1] : 0, nd > 2 ? d[2] : 0, nd > 3 ? d[3] : 0);
    }
}
static void rdna4s_crc_free(rdna4crc_rec_t *r) { if (!r) return; if (r->ios) CFRelease(r->ios); free(r); }

// The command buffer that carried the copy into `slot` ended (fence result r) or never ran: present on VK_SUCCESS only. `rec` (nil unless the CRC diagnostic runs) is consumed.

// ---------------------------------------------------------------------------------------------------------------
// native #12 Stage 0b (an internal design note): per-process command-buffer / present log and the cross-queue RAW-inversion counter.
// OFF unless /private/tmp/rdna4m-cblog exists (polled every 1 s by its own timer; removed -> the file is closed). Output goes to
// /private/var/tmp/rdna4m-cblog.<pid>.txt (a plain file, written with write(2) line by line: no os_log, so nothing is lost to log throttling), capped at 50 MB.
// Observation only: nothing it logs changes what is submitted or presented. All times are CLOCK_UPTIME_RAW ns (rdna4_now); "K" lines pair it with CLOCK_REALTIME every second.
// IOSurface ids are IOSurfaceGetID, the same ids the DTrace transaction log prints. Line types (key=value, one per line):
//   H header (pid, up, wall)     K clock pair; K10 has submits, writebacks, accepted presents, present calls, skips and idle flag
//   C commit      S submit      F fence done      P present attempt (rc=0 means accepted)      X present skipped
//   I inversion case (first 20 per enable)       E end
// ---------------------------------------------------------------------------------------------------------------
#define RDNA4_CBLOG_FLAG "/private/tmp/rdna4m-cblog"
#define RDNA4_CBLOG_CAP (50ull << 20)
GARNET_INTERNAL RDNA4CBLogState RDNA4CB = { .fd = -1, .mu = PTHREAD_MUTEX_INITIALIZER };
GARNET_INTERNAL _Atomic uint64_t rdna4cb_ids;

GARNET_INTERNAL void rdna4cbl_line_locked(const char *fmt, ...) {
    if (RDNA4CB.fd < 0) return;
    char b[1024]; va_list ap; va_start(ap, fmt); int n = vsnprintf(b, sizeof b - 1, fmt, ap); va_end(ap);
    if (n <= 0) return;
    if (n > (int)sizeof b - 2) n = (int)sizeof b - 2;
    b[n++] = '\n';
    if (RDNA4CB.bytes + (uint64_t)n > RDNA4_CBLOG_CAP) {
        const char *m = "E reason=cap\n"; (void)!write(RDNA4CB.fd, m, strlen(m));
        RDNA4CB.capped = 1; atomic_store(&RDNA4CB.on, 0); close(RDNA4CB.fd); RDNA4CB.fd = -1;
        RDNA4LOG("cblog: 50 MB cap reached, logging stopped (remove " RDNA4_CBLOG_FLAG " and recreate it to start a new file)");
        return;
    }
    ssize_t w = write(RDNA4CB.fd, b, (size_t)n); if (w > 0) RDNA4CB.bytes += (uint64_t)w;
}
GARNET_INTERNAL void rdna4cbl_sids(char *o, size_t cap, const uint32_t *a, uint32_t k) {
    size_t l = 0; o[0] = 0;
    if (!k) { snprintf(o, cap, "-"); return; }
    for (uint32_t i = 0; i < k && l + 12 < cap; i++) l += (size_t)snprintf(o + l, cap - l, i ? ",%u" : "%u", a[i]);
}
static uint64_t rdna4cbl_wall(void) { return clock_gettime_nsec_np(CLOCK_REALTIME); }
static void rdna4cbl_poll(void) {   // 1 s timer
    struct stat sb; int want = stat(RDNA4_CBLOG_FLAG, &sb) == 0;
    pthread_mutex_lock(&RDNA4CB.mu);
    if (want && !atomic_load(&RDNA4CB.on) && !RDNA4CB.capped) {
        char path[96]; snprintf(path, sizeof path, "/private/var/tmp/rdna4m-cblog.%d.txt", (int)getpid());
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0) { static int warned; if (!warned) { warned = 1; RDNA4LOG("cblog: cannot open %s: errno %d", path, errno); } }
        else {
            RDNA4CB.fd = fd; RDNA4CB.bytes = 0; RDNA4CB.inv_logged = 0; RDNA4CB.tick = 0; rdna4cb_init(&RDNA4CB.trk); rdna4cba_init(&RDNA4CB.activity);
            rdna4cbl_line_locked("H pid=%d up=%llu wall=%llu cap=%llu fmt=1", (int)getpid(), (unsigned long long)rdna4_now(), (unsigned long long)rdna4cbl_wall(), (unsigned long long)RDNA4_CBLOG_CAP);
            atomic_store(&RDNA4CB.on, 1);
            RDNA4LOG("cblog: ENABLED (" RDNA4_CBLOG_FLAG " exists) -> %s", path);
        }
    } else if (!want && (atomic_load(&RDNA4CB.on) || RDNA4CB.capped)) {
        if (atomic_load(&RDNA4CB.on)) { rdna4cbl_line_locked("E reason=flag-removed"); atomic_store(&RDNA4CB.on, 0); close(RDNA4CB.fd); RDNA4CB.fd = -1; RDNA4LOG("cblog: DISABLED (flag removed), %llu bytes written", (unsigned long long)RDNA4CB.bytes); }
        RDNA4CB.capped = 0;
    } else if (atomic_load(&RDNA4CB.on)) {
        rdna4cbl_line_locked("K up=%llu wall=%llu", (unsigned long long)rdna4_now(), (unsigned long long)rdna4cbl_wall());
        if (++RDNA4CB.tick % 10 == 0) {
            uint64_t sub, inv; int idle;
            rdna4cb_take_interval(&RDNA4CB.trk, &sub, &inv);
            const rdna4cba_t activity = rdna4cba_take_interval(&RDNA4CB.activity, &idle);
            rdna4cbl_line_locked("K10 submits=%llu completed=%llu failed=%llu writebacks=%llu presents=%llu present_calls=%llu skips=%llu idle=%d inversions=%llu inv_total=%llu inv_cbs=%llu commits=%llu evicted=%llu no_commit=%llu bytes=%llu",
                (unsigned long long)sub, (unsigned long long)activity.completed, (unsigned long long)activity.failed, (unsigned long long)activity.writebacks, (unsigned long long)activity.presents, (unsigned long long)activity.present_calls, (unsigned long long)activity.skips, idle, (unsigned long long)inv,
                (unsigned long long)RDNA4CB.trk.inv_total, (unsigned long long)RDNA4CB.trk.inv_cbs, (unsigned long long)RDNA4CB.trk.commits, (unsigned long long)RDNA4CB.trk.evicted, (unsigned long long)RDNA4CB.trk.no_commit, (unsigned long long)RDNA4CB.bytes);
            RDNA4LOG("cblog (10 s): submits %llu completed %llu failed %llu writebacks %llu presents %llu present_calls %llu skips %llu idle %d; cross-queue RAW inversions %llu (total %llu in %llu submits; evicted %llu, submitted-without-commit %llu); file %llu bytes",
                (unsigned long long)sub, (unsigned long long)activity.completed, (unsigned long long)activity.failed, (unsigned long long)activity.writebacks, (unsigned long long)activity.presents, (unsigned long long)activity.present_calls, (unsigned long long)activity.skips, idle, (unsigned long long)inv, (unsigned long long)RDNA4CB.trk.inv_total, (unsigned long long)RDNA4CB.trk.inv_cbs, (unsigned long long)RDNA4CB.trk.evicted, (unsigned long long)RDNA4CB.trk.no_commit, (unsigned long long)RDNA4CB.bytes);
        }
    }
    pthread_mutex_unlock(&RDNA4CB.mu);
}
static dispatch_source_t rdna4cbl_timer;   // kept alive for the process (a local would be released by ARC and the timer would never fire)
GARNET_INTERNAL void rdna4cbl_start(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        dispatch_source_t t = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
        dispatch_source_set_timer(t, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), NSEC_PER_SEC, NSEC_PER_SEC / 10);
        dispatch_source_set_event_handler(t, ^{ rdna4cbl_poll(); });
        rdna4cbl_timer = t;
        dispatch_resume(t);
    });
}

static void rdna4cbl_present(uint64_t t0, uint64_t t1, uint32_t sid, uint64_t seq, int slot, int rc) {   // caller: the present queue, around scanout_present
    if (!rdna4cbl_on()) return;
    pthread_mutex_lock(&RDNA4CB.mu);
    rdna4cba_present(&RDNA4CB.activity, rc == 0);
    rdna4cbl_line_locked("P t0=%llu t1=%llu sid=%u seq=%llu slot=%d rc=%d", (unsigned long long)t0, (unsigned long long)t1, sid, (unsigned long long)seq, slot, rc);
    pthread_mutex_unlock(&RDNA4CB.mu);
}
GARNET_INTERNAL void rdna4cbl_skip(uint32_t sid, uint64_t seq, int slot, const char *why, int vr) {
    if (!rdna4cbl_on()) return;
    pthread_mutex_lock(&RDNA4CB.mu);
    rdna4cba_skip(&RDNA4CB.activity);
    rdna4cbl_line_locked("X t=%llu sid=%u seq=%llu slot=%d why=%s vk=%d", (unsigned long long)rdna4_now(), sid, (unsigned long long)seq, slot, why, vr);
    pthread_mutex_unlock(&RDNA4CB.mu);
}
typedef struct { uint64_t slept; } rdna4h_ctx;
static void rdna4h_wait(void *c, uint64_t ns) {   // runs on the present queue with RDNA4S.mu held: drop it for the sleep so the submit path (rdna4DispSeq) and the tick never block on the hold
    uint64_t t0 = rdna4_now();
    pthread_mutex_unlock(&RDNA4S.mu);
    struct timespec ts = { (time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull) }; nanosleep(&ts, NULL);
    pthread_mutex_lock(&RDNA4S.mu);
    ((rdna4h_ctx *)c)->slept = rdna4_now() - t0;
}
static void rdna4s_complete_run(int inst, int slot, VkResult r, uint64_t seq, uint32_t sid, rdna4crc_rec_t *rec, int cls, uint64_t tcommit, int tries);
static BOOL rdna4_capture_readback(VkBuffer slotBuffer, size_t row, size_t pitch, size_t rows,
                                   uint8_t *out, BOOL *unresolved) {
    const VkDeviceSize span = (rows - 1) * pitch + row;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void *mapped = NULL;
    VkBufferCreateInfo info = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = span,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    if (vkCreateBuffer(RDNA4R.dev, &info, NULL, &buffer) != VK_SUCCESS) return NO;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(RDNA4R.dev, buffer, &req);
    const int type = rdna4_find_mem(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    VkMemoryAllocateInfo alloc = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size, .memoryTypeIndex = (uint32_t)type };
    BOOL complete = NO;
    if (type >= 0 && vkAllocateMemory(RDNA4R.dev, &alloc, NULL, &memory) == VK_SUCCESS &&
        vkBindBufferMemory(RDNA4R.dev, buffer, memory, 0) == VK_SUCCESS &&
        vkMapMemory(RDNA4R.dev, memory, 0, span, 0, &mapped) == VK_SUCCESS && mapped) {
        complete = rdna4_oneshot(NULL, ^(VkCommandBuffer cmd) {
            VkMemoryBarrier read = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT };
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, 1, &read, 0, NULL, 0, NULL);
            VkBufferCopy copy = { .size = span };
            vkCmdCopyBuffer(cmd, slotBuffer, buffer, 1, &copy);
            VkMemoryBarrier host = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                0, 1, &host, 0, NULL, 0, NULL);
        }, unresolved);
        if (*unresolved) return NO;
        if (complete) for (size_t y = 0; y < rows; y++)
            memcpy(out + y * row, (const uint8_t *)mapped + y * pitch, row);
        vkUnmapMemory(RDNA4R.dev, memory);
    }
    vkDestroyBuffer(RDNA4R.dev, buffer, NULL);
    if (memory) vkFreeMemory(RDNA4R.dev, memory, NULL);
    return complete;
}
static void rdna4_capture_present(int inst, int slot, uint32_t sid, uint64_t seq, int rc, uint64_t presentId);
static void rdna4s_retry_run(int inst, uint64_t expected);
static void rdna4s_retry_schedule(int inst) {
    const uint64_t expected = inst ? RDNA4XI(inst).sm.retry_seq : RDNA4S.sm.retry_seq;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 16 * NSEC_PER_MSEC), RDNA4S.pq, ^{ rdna4s_retry_run(inst, expected); });
}
static void rdna4s_retry_run(int inst, uint64_t expected) {
    pthread_mutex_lock(&RDNA4S.mu);
    const int xinst = rdna4x_desc((uint32_t)inst) != NULL;
    rdna4df_t *const sm = xinst ? &RDNA4XI(inst).sm : &RDNA4S.sm;
    const uint32_t *const sids = xinst ? RDNA4XI(inst).sid : RDNA4S.sid;
    int slot = -1; uint32_t sid = 0; uint64_t seq = 0;
    if (!rdna4df_retry_begin(sm, expected, &slot, &sid, &seq)) { pthread_mutex_unlock(&RDNA4S.mu); return; }
    uint64_t out[3] = { 0, 0, 0 };
    const uint64_t start = rdna4_now();
    const int rc = xinst ? rdna4x_k_present(inst, sids[slot], out) : RDNA4S.fp(RDNA4R.dev, sids[slot], out);
    rdna4cbl_present(start, rdna4_now(), sid, seq, slot, rc);
    const int release = rdna4df_retry_result(sm, rc);
    if (rc == 0) {
        rdna4_capture_present(inst, slot, sid, seq, rc, out[0]);
        if (xinst) { RDNA4XI(inst).presents_seen++; RDNA4XI(inst).xs.recopy_pending = 0; }
        if (RDNA4S.m6.latch) (void)rdna4m6_final_count(&RDNA4S.m6, sid, RDNA4M6_PRESENT);
        RDNA4LOGR("scanout: retry presented slot %d seq %llu surface %u", slot, (unsigned long long)seq, sid);
    } else if (rc == -EBUSY || rc == -ENODEV) {
        if (release) {
            RDNA4LOG("scanout: FAIL CLOSED / OFF (retry refusals exhausted; rc %d)", rc);
            if (xinst) rdna4x_do_release_locked(inst); else rdna4s_do_release_locked();
        } else if (sm->retry_slot >= 0) rdna4s_retry_schedule(inst);
        else RDNA4LOGR("scanout: retry gave up after %u refusals (slot %d seq %llu surface %u)", RDNA4DF_RETRY_MAX_ATTEMPTS, slot, (unsigned long long)seq, sid);
    } else if (release) {
        RDNA4LOG("scanout: FAIL CLOSED / OFF (retry present rc %d)", rc);
        if (xinst) rdna4x_do_release_locked(inst); else rdna4s_do_release_locked();
    }
    pthread_mutex_unlock(&RDNA4S.mu);
}
static dispatch_queue_t rdna4s_present_queue(void) {
    static dispatch_once_t once; dispatch_once(&once, ^{ RDNA4S.pq = dispatch_queue_create("rdna4.present", DISPATCH_QUEUE_SERIAL); });
    return RDNA4S.pq;
}
GARNET_INTERNAL void rdna4s_complete(int inst, int slot, VkResult r, uint64_t seq, uint32_t sid, rdna4crc_rec_t *rec, int cls, uint64_t tcommit) {
    // All presents go through ONE serial queue (completions arrive from one serial queue PER Metal command queue, so across queues their order is arbitrary);
    // the sequence check under the mutex then makes the present order equal the submission order: an older frame is skipped, never shown after a newer one.
    dispatch_async(rdna4s_present_queue(), ^{ rdna4s_complete_run(inst, slot, r, seq, sid, rec, cls, tcommit, 0); });
}
// Runs ON the present queue. `tries` = how many times this frame was already queued again because the kernel had not yet seen its surface (bundle 12).
static void rdna4s_complete_run(int inst, int slot, VkResult r, uint64_t seq, uint32_t sid, rdna4crc_rec_t *rec, int cls, uint64_t tcommit, int tries) {
    {
        pthread_mutex_lock(&RDNA4S.mu);
        const int xinst = rdna4x_desc((uint32_t)inst) != NULL;                      // bundle 13/15: an HDMI frame (instance 1 = the monitor A, 2 = the monitor B); 0 = the DP
        rdna4df_t *const sm = xinst ? &RDNA4XI(inst).sm : &RDNA4S.sm;                     // the frame's own instance machine
        const uint32_t *const sids = xinst ? RDNA4XI(inst).sid : RDNA4S.sid;
        // bundle 12 (queue 290 S3): THE M6 VERDICT IS DECIDED FIRST, before rdna4df_complete_held releases the slot and moves last_seq. Bundle 11 asked after the release and, for an unknown ID, dropped the lock and slept
        // up to 8 x 3 ms with the slot already free (another thread could pick it) on the one present queue. Now: no sleep, no registry read here. A frame whose surface is another display's (or still unknown after the
        // last try) is skipped with its slot released as a failed chain frame would be (rdna4df_skip_content); an unknown one is queued again after RDNA4M6_RETRY_NS while the table is refreshed in the background.
        if (RDNA4S.m6.latch && r == VK_SUCCESS && slot >= 0 && slot < RDNA4DF_MAX_SLOTS && sm->inflight[slot]) {
            // bundle 13: PRESENT ONLY IF THE FINAL INSTANCE EQUALS THE SLOT'S INSTANCE. A table older than the kernel's last published generation (Status2) is refreshed first (async) and the frame retried, then dropped: a stale
            // cache can never send a DP frame to the monitor B. A mismatch is dropped and counted (inst_mismatch); a tentative instance-0 frame whose surface is the monitor B's also notes the surface for a re-copy (R3).
            const uint32_t xon = rdna4x_onmask_locked();
            int stale = 0; for (int xi = 1; xi <= 2; xi++) if (RDNA4XI(xi).sm.state == RDNA4DF_ACTIVE && rdna4m6_table_stale(&RDNA4S.m6, RDNA4XI(xi).gen_known ? RDNA4XI(xi).gen : 0u)) stale = 1;   // any ACTIVE HDMI instance has seen a newer table than the cache
            const int plan = rdna4x_complete_plan(&RDNA4S.m6, sid, inst, tries, stale, xon);
            const rdna4m6_ent_t *const fe = rdna4m6_find(&RDNA4S.m6, sid);                  // the final entry (if any): which HDMI instance a mismatch / re-copy concerns
            rdna4xi_t *const cx = xinst ? &RDNA4XI(inst) : (fe && rdna4x_desc(fe->inst) ? &RDNA4XI(fe->inst) : &RDNA4XI(2));   // the counters go to the frame's instance, else to the display the surface turned out to belong to
            if (plan == RDNA4X_C_RETRY) {
                if (stale) cx->xs.stale_retry++;
                rdna4s_m6_refresh_async();
                pthread_mutex_unlock(&RDNA4S.mu);
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)RDNA4M6_RETRY_NS), RDNA4S.pq, ^{ rdna4s_complete_run(inst, slot, r, seq, sid, rec, cls, tcommit, tries + 1); });
                return;                                    // the slot stays in flight, `rec` is carried by the queued completion
            }
            if (plan != RDNA4X_C_PRESENT) {
                if (plan == RDNA4X_C_MISMATCH || plan == RDNA4X_C_RECOPY) cx->xs.inst_mismatch++;
                if (plan == RDNA4X_C_RECOPY) rdna4x_recopy_note(&cx->xs, sid, seq);
                if (plan == RDNA4X_C_DROP && stale) cx->xs.stale_drop++;
                rdna4s_m6_skip_locked(slot, sid, plan, sm);
                rdna4cbl_skip(sid, seq, slot, plan == RDNA4X_C_MISMATCH || plan == RDNA4X_C_RECOPY ? "inst-mismatch" : "m6", (int)r);
                pthread_mutex_unlock(&RDNA4S.mu);
                rdna4s_crc_free(rec);
                return;
            }
        }
        uint64_t stale0 = sm->stale, sup0 = sm->superseded, inv0 = sm->chain_invalid;
        rdna4h_ctx hc = { 0 }; uint64_t hw = 0;
        int hmode = atomic_load_explicit(&RDNA4H.mode, memory_order_relaxed);
        int hon = rdna4df_hold_effective(hmode, cls);
        int hok = rdna4df_complete_held(sm, slot, r == VK_SUCCESS ? 0 : 1, seq, sid, hon, atomic_load_explicit(&RDNA4H.ms, memory_order_relaxed), tcommit, rdna4_now(), rdna4h_wait, &hc, &hw);
        if (hw) { RDNA4H.held++; RDNA4H.hold_ns += hc.slept; if (sm->superseded != sup0) RDNA4H.held_sup++; }
        if (hok) {
            uint64_t o[3] = { 0, 0, 0 };
            if (rec) rdna4s_crc_present(rec);
            uint64_t tp0_ = rdna4_now();
            const uint64_t tk0 = rdna4_now();
            int rc = xinst ? rdna4x_k_present(inst, sids[slot], o) : RDNA4S.fp(RDNA4R.dev, sids[slot], o);   // bundle 13/15: the instance's own present
            const uint64_t tk = rdna4_now() - tk0;
            if (tk > 250000000ull) RDNA4LOG("scanout STALL: the kernel present ioctl took %.1f ms (instance %d slot %d seq %llu rc %d)", (double)tk / 1e6, inst, slot, (unsigned long long)seq, rc);
            rdna4cbl_present(tp0_, rdna4_now(), sid, seq, slot, rc);   // Stage 0b (no-op unless /private/tmp/rdna4m-cblog exists)
            if (rc == 0) {
                rdna4_capture_present(inst, slot, sid, seq, rc, o[0]);
                rdna4df_count_present_cls(sm, cls);
                if (hw) RDNA4H.held_pres++;
                if (RDNA4S.m6.latch) (void)rdna4m6_final_count(&RDNA4S.m6, sid, RDNA4M6_PRESENT);
                rdna4df_retry_supersede(sm, seq);
            }
            if (xinst && rc == 0) { RDNA4XI(inst).presents_seen++; RDNA4XI(inst).xs.recopy_pending = 0; }   // a present of an HDMI display repaints the whole surface: a pending re-copy is moot
            const int retryable = rc == -EBUSY || rc == -ENODEV;
            if (!retryable && rdna4df_present_result(sm, rc)) {
                RDNA4LOG("scanout: FAIL CLOSED / OFF (scanout-error): scanout_present(slot %d) rc %d", slot, rc);
                if (xinst) rdna4x_do_release_locked(inst); else rdna4s_do_release_locked();   // a failed HDMI present fails THAT instance closed only
            } else if (rc == 0) RDNA4LOGR("scanout: present slot %d seq %llu id %llu target frame %llu vupdates %llu", slot, (unsigned long long)seq, (unsigned long long)o[0], (unsigned long long)o[1], (unsigned long long)o[2]);
            else if (retryable) {
                if (rc == -EBUSY) (void)rdna4df_present_result(sm, rc);
                rdna4df_retry_note(sm, slot, sid, seq, tcommit, cls, rc); rdna4s_retry_schedule(inst);
                RDNA4LOGR("scanout: present refused; retaining newest frame for bounded retry (slot %d seq %llu sid %u rc %d)", slot, (unsigned long long)seq, sid, rc);
            }
        } else if (rdna4cbl_skip(sid, seq, slot, sm->superseded != sup0 ? "superseded" : sm->chain_invalid != inv0 ? "chain-order" : sm->stale != stale0 ? "stale" : r != VK_SUCCESS ? "gpu-error" : "other", (int)r), sm->superseded != sup0) RDNA4LOGR("scanout: superseded frame skipped (slot %d seq %llu: a later command buffer for the same surface was submitted)", slot, (unsigned long long)seq);
        else if (sm->chain_invalid != inv0) RDNA4LOGR("scanout: frame NOT presented (slot %d seq %llu): submitted out of chain order, its base slot was not written yet", slot, (unsigned long long)seq);
        else if (sm->stale != stale0) RDNA4LOGR("scanout: stale frame skipped (slot %d seq %llu, last presented %llu)", slot, (unsigned long long)seq, (unsigned long long)sm->last_seq);
        else if (r != VK_SUCCESS) RDNA4LOGR("scanout: command buffer for slot %d ended with %d: not presented", slot, r);
        pthread_mutex_unlock(&RDNA4S.mu);
        rdna4s_crc_free(rec);
    }
}
// Device selectors for mtlprobe dispflip (root test path; the stats read is one status call).
GARNET_INTERNAL NSDictionary *rdna4s_stats(void) {
    pthread_mutex_lock(&RDNA4S.mu);
    struct rdna4n_scan_status st; memset(&st, 0, sizeof st); BOOL fresh = NO;
    if (RDNA4S.sm.state == RDNA4DF_ACTIVE) fresh = rdna4s_status_locked(&st);
    NSDictionary *d = @{ @"state": RDNA4S.sm.state == RDNA4DF_ACTIVE ? @"active" : RDNA4S.sm.state == RDNA4DF_OFF ? @"off" : (RDNA4S.bound ? @"unbound" : @"unbound-never-seen-a-candidate"),
        @"reason": @(rdna4s_reason_name(RDNA4S.sm.reason)), @"fresh": @(fresh), @"presents": @(RDNA4S.sm.presents), @"drops": @(RDNA4S.sm.drops), @"gpu_fail": @(RDNA4S.sm.gpu_fail), @"stale": @(RDNA4S.sm.stale), @"superseded": @(RDNA4S.sm.superseded), @"multi_disp": @(atomic_load(&RDNA4S.multi)),
        @"present_fail": @(RDNA4S.sm.present_fail), @"present_busy": @(RDNA4S.sm.present_busy), @"latched": @(st.latched), @"replaced": @(st.replaced), @"repeats": @(st.repeats), @"refused": @(st.refused),
        @"watchdog_restores": @(st.watchdog_restores), @"frame_count": @(st.frame_count), @"vupdates": @(st.vupdates), @"disp_surfaces": @(RDNA4S.ndisp), @"nonfinal_skipped": @(RDNA4S.sm.nonfinal[1] + RDNA4S.sm.nonfinal[2] + RDNA4S.sm.nonfinal[3] + RDNA4S.sm.nonfinal[4] + RDNA4S.sm.nonfinal[5] + RDNA4S.sm.nonfinal[6]), @"final_writes": @(RDNA4S.sm.cls[0]) };
    NSMutableDictionary *md = [d mutableCopy];   // bundle 13/15: the HDMI instances' counters (x_* = the monitor B as in bundle 13; a_* = the monitor A)
    for (int xi = 1; xi <= 2; xi++) {
        rdna4xi_t *const X = &RDNA4XI(xi); const char *pre = xi == 2 ? "x_" : "a_";
        #define XK(n) [NSString stringWithFormat:@"%s%s", pre, n]
        md[XK("enabled")] = @(X->enabled); md[XK("state")] = X->sm.state == RDNA4DF_ACTIVE ? @"active" : X->sm.state == RDNA4DF_OFF ? @"off" : @"unbound"; md[XK("reason")] = @(rdna4s_reason_name(X->sm.reason));
        md[XK("presents")] = @(X->sm.presents); md[XK("present_busy")] = @(X->sm.present_busy); md[XK("plan")] = @(X->plan_count); md[XK("inst_mismatch")] = @(X->xs.inst_mismatch); md[XK("inst_geom_mismatch")] = @(X->xs.inst_geom_mismatch); md[XK("recopy_noted")] = @(X->xs.recopy_noted); md[XK("recopy_done")] = @(X->xs.recopy_done);
        md[XK("keepalive_calls")] = @(X->xs.keepalive_calls); md[XK("stale_retry")] = @(X->xs.stale_retry); md[XK("gen")] = @(X->gen);
        #undef XK
    }
    md[@"x_plan_monb"] = @(RDNA4XI(2).plan_count); md[@"m6_gen"] = @(RDNA4S.m6.gen); md[@"m6_store_refused"] = @(RDNA4S.m6.store_refused);
    pthread_mutex_unlock(&RDNA4S.mu);
    return md;
}
GARNET_INTERNAL NSDictionary *rdna4s_release_request(void) {
    pthread_mutex_lock(&RDNA4S.mu);
    uint64_t o[2] = { 0, 0 }; int rc = -1;
    for (int xi = 1; xi <= 2; xi++) if (RDNA4XI(xi).sm.acquired) { RDNA4XI(xi).sm.state = RDNA4DF_OFF; rdna4x_do_release_locked(xi); }   // bundle 13/15: the HDMI instances' A go back first
    if (RDNA4S.sm.acquired && RDNA4S.fr) { if (!RDNA4S.sm.released) { RDNA4S.sm.released = 1; RDNA4S.sm.state = RDNA4DF_OFF; }
        if (RDNA4S.timer) { dispatch_source_cancel(RDNA4S.timer); RDNA4S.timer = nil; }
        rc = RDNA4S.fr(RDNA4R.dev, o); RDNA4LOG("scanout: release (requested) rc %d, verified %llu, plane MC after 0x%llx", rc, (unsigned long long)o[0], (unsigned long long)o[1]); }
    NSDictionary *d = @{ @"rc": @(rc), @"verified": @(o[0]), @"plane_mc": @(o[1]) };
    pthread_mutex_unlock(&RDNA4S.mu);
    return d;
}

#endif // !defined(GARNET_DISPLAY_LATE_ONLY) && !defined(GARNET_PRESENT_CAPTURE_ONLY)
#if defined(GARNET_DISPLAY_LATE_ONLY) || (!defined(GARNET_PRESENT_CAPTURE_ONLY))
// A display write can finish encoding while every scanout slot is still busy.
// Keep only the newest completed source per display and copy it after a slot becomes reusable.
@interface RDNA4LateFrame : NSObject {
@public
    RDNA4Texture *texture;
    IOSurfaceRef surface;
    uint64_t seq, commit;
    uint32_t sid;
    int inst, cls;
    unsigned tries;
}
@end
@implementation RDNA4LateFrame
- (void)dealloc { if (surface) { IOSurfaceDecrementUseCount(surface); CFRelease(surface); } }
@end

static RDNA4LateFrame *rdna4late_pending[3];   // present queue only
static void rdna4s_late_run(RDNA4LateFrame *frame) {
    if (rdna4late_pending[frame->inst] != frame) return;
    pthread_mutex_lock(&RDNA4S.mu);
    rdna4df_t *sm = frame->inst ? &RDNA4XI(frame->inst).sm : &RDNA4S.sm;
    const BOOL current = rdna4df_late_current(sm, frame->seq) &&
        !rdna4dc_commit_newer(RDNA4S.commits, frame->sid, frame->commit);
    pthread_mutex_unlock(&RDNA4S.mu);
    if (!current) { rdna4late_pending[frame->inst] = nil; return; }

    rdna4df_plan_t plan;
    // A deferred source contains the complete current surface; never chain it onto an older slot.
    int slot = rdna4s_plan(NULL, frame->inst, 1, &plan);
    if (slot < 0) {
        if (++frame->tries < RDNA4DF_RETRY_MAX_ATTEMPTS) {
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 16 * NSEC_PER_MSEC), rdna4s_present_queue(), ^{ rdna4s_late_run(frame); });
        } else {
            rdna4cbl_skip(frame->sid, frame->seq, -1, "late-no-slot", 0);
            rdna4late_pending[frame->inst] = nil;
        }
        return;
    }
    rdna4late_pending[frame->inst] = nil;
    pthread_mutex_lock(&RDNA4S.mu);
    const rdna4x_desc_t *xd = rdna4x_desc((uint32_t)frame->inst);
    const uint32_t w = xd ? xd->w : RDNA4S.pw, h = xd ? xd->h : RDNA4S.ph, pitch = xd ? xd->pitch_bytes : RDNA4S.pitch;
    const BOOL valid = rdna4df_late_current(sm, frame->seq) &&
        !rdna4dc_commit_newer(RDNA4S.commits, frame->sid, frame->commit) &&
        [frame->texture width] == w && [frame->texture height] == h &&
        [frame->texture bytesPerPixel] == 4 && [frame->texture rdna4IOSBytesPerRow] == pitch &&
        [frame->texture rdna4DispBuf] != VK_NULL_HANDLE;
    if (valid) {
        rdna4df_submit(sm, slot, frame->sid, frame->seq);
        sm->sub_commit[slot] = frame->commit;
        sm->deferred_copy[slot] = 1;
    } else rdna4df_skip_content(sm, slot);
    pthread_mutex_unlock(&RDNA4S.mu);
    if (!valid) return;

    NSError *error = nil; BOOL unresolved = NO;
    BOOL ok = rdna4_oneshot(&error, ^(VkCommandBuffer cmd) {
        // Capturing frame also retains the source/use-count if the GPU submission is quarantined.
        rdna4_full_barrier(cmd);
        rdna4s_record_copy(cmd, [frame->texture rdna4DispBuf], [frame->texture rdna4IOSOffset], &plan);
        rdna4_full_barrier(cmd);
    }, &unresolved);
    if (unresolved) {
        pthread_mutex_lock(&RDNA4S.mu);
        if (rdna4df_fail(sm, RDNA4DF_R_ERROR)) {
            if (frame->inst) rdna4x_do_release_locked(frame->inst); else rdna4s_do_release_locked();
        }
        pthread_mutex_unlock(&RDNA4S.mu);
        RDNA4LOG("scanout: deferred copy unresolved: %s", error.localizedDescription.UTF8String);
        return;   // The slot and source stay retained; never reuse memory the GPU may still write.
    }
    rdna4s_complete_run(frame->inst, slot, ok ? VK_SUCCESS : VK_ERROR_UNKNOWN, frame->seq, frame->sid, NULL, frame->cls, frame->commit, 0);
}

GARNET_INTERNAL void rdna4s_late_complete(RDNA4Texture *texture, int inst, uint64_t seq, uint32_t sid, int cls, uint64_t commit) {
    if (!texture || inst < 0 || inst > 2 || !seq || !sid) return;
    RDNA4LateFrame *frame = [RDNA4LateFrame new];
    frame->texture = texture; frame->inst = inst; frame->seq = seq; frame->sid = sid; frame->cls = cls; frame->commit = commit;
    IOSurfaceRef surface = [texture rdna4UCSurface];
    if (surface) { frame->surface = (IOSurfaceRef)CFRetain(surface); IOSurfaceIncrementUseCount(surface); }
    dispatch_async(rdna4s_present_queue(), ^{
        RDNA4LateFrame *old = rdna4late_pending[inst];
        if (old && old->seq > seq) return;
        rdna4late_pending[inst] = frame;
        rdna4s_late_run(frame);
    });
}
#endif // GARNET_DISPLAY_LATE_ONLY
#if !defined(GARNET_DISPLAY_LATE_ONLY) && !defined(GARNET_PRESENT_CAPTURE_ONLY)
// Opt-in snapshots of compositor inputs after GPU completion, before their use counts are released.
#define RDNA4_CAPTURE_FLAG "/private/tmp/rdna4m-surfaces"
static BOOL rdna4_capture_on(void) {
    if (!rdna4_is_ws() && !rdna4_allow()) return NO;
    static _Atomic uint64_t next;
    static _Atomic int enabled;
    uint64_t now = rdna4_now(), due = atomic_load(&next);
    if (now >= due && atomic_compare_exchange_strong(&next, &due, now + NSEC_PER_SEC)) {
        struct stat st;
        atomic_store(&enabled, lstat(RDNA4_CAPTURE_FLAG, &st) == 0 && S_ISREG(st.st_mode) &&
            (st.st_uid == 0 || st.st_uid == geteuid()));
    }
    return atomic_load(&enabled) != 0;
}
GARNET_INTERNAL void rdna4_capture_surfaces(NSArray *textures, uint64_t cbid) {
    if (!rdna4_capture_on()) return;
    const uint64_t now = rdna4_now();
    static NSLock *lock;
    static NSMutableDictionary *seen;
    static dispatch_queue_t writer;
    static uint64_t used;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        lock = [NSLock new]; seen = [NSMutableDictionary dictionary];
        writer = dispatch_queue_create("org.garnet.surface-capture", DISPATCH_QUEUE_SERIAL);
    });
    [lock lock];
    for (RDNA4Texture *view in textures) {
        RDNA4Texture *t = [view rootResource];
        IOSurfaceRef surface = [t rdna4IOSRef];
        if (!surface || [t rdna4IsDisp] || ![t rdna4IBase]) continue;
        size_t w = t.width, h = t.height, row = [t rdna4IOSBytesPerRow], bpp = [t bytesPerPixel];
        if (!w || !h || !bpp || w > SIZE_MAX / bpp || row < w * bpp ||
            h - 1 > (SIZE_MAX - w * bpp) / row) continue;
        size_t bytes = (h - 1) * row + w * bpp;
        if (bytes > 12u * 1024u * 1024u || used + bytes > 64u * 1024u * 1024u) continue;
        NSString *key = [NSString stringWithFormat:@"%u-%lu-%zux%zu-%lu", IOSurfaceGetID(surface),
            (unsigned long)t.pixelFormat, w, h, (unsigned long)t.iosurfacePlane];
        NSArray *prior = seen[key];
        if (!prior && seen.count >= 128) continue;
        unsigned ordinal = prior ? [prior[0] unsignedIntValue] : 0;
        if (ordinal >= 2 || (prior && now - [prior[1] unsignedLongLongValue] < 3 * NSEC_PER_SEC)) continue;
        char directory[1024];
        if (!rdna4_dump_dir_ready(directory, sizeof directory)) continue;
        uint32_t before = IOSurfaceGetSeed(surface);
        NSData *pixels = [NSData dataWithBytes:[t rdna4IBase] length:bytes];
        uint32_t after = IOSurfaceGetSeed(surface);
        if (!pixels) continue;
        NSDictionary *metadata = @{ @"surface": @(IOSurfaceGetID(surface)), @"pixel_format": @(t.pixelFormat),
            @"iosurface_format": @(IOSurfaceGetPixelFormat(surface)), @"width": @(w), @"height": @(h),
            @"bytes_per_row": @(row), @"bytes_per_pixel": @(bpp), @"plane": @(t.iosurfacePlane),
            @"bytes": @(bytes), @"command_buffer": @(cbid), @"time_ns": @(now),
            @"seed_before": @(before), @"seed_after": @(after), @"linear": @([t rdna4IOSLinear]) };
        NSData *json = [NSJSONSerialization dataWithJSONObject:metadata options:0 error:NULL];
        if (!json) continue;
        seen[key] = @[@(ordinal + 1), @(now)]; used += bytes;
        NSString *stem = [NSString stringWithFormat:@"%s/surface-%d-%@-%u", directory, getpid(), key, ordinal];
        dispatch_async(writer, ^{
            @autoreleasepool {
                BOOL okay = rdna4_dump_write([[stem stringByAppendingString:@".raw"] fileSystemRepresentation], pixels) &&
                    rdna4_dump_write([[stem stringByAppendingString:@".json"] fileSystemRepresentation], json);
                RDNA4LOG("surface capture: %s bytes=%lu status=%s seed=%u/%u", stem.UTF8String,
                    (unsigned long)pixels.length, okay ? "saved" : "failed", before, after);
            }
        });
    }
    [lock unlock];
}
#endif // !defined(GARNET_DISPLAY_LATE_ONLY) && !defined(GARNET_PRESENT_CAPTURE_ONLY)
#if defined(GARNET_PRESENT_CAPTURE_ONLY) || (!defined(GARNET_DISPLAY_LATE_ONLY))
#ifndef RDNA4_PRESENT_CAPTURE_FLAG
#define RDNA4_PRESENT_CAPTURE_FLAG "/private/tmp/rdna4m-present-capture"
#endif
#ifndef RDNA4_PRESENT_FULL_FLAG
#define RDNA4_PRESENT_FULL_FLAG "/private/tmp/rdna4m-present-full"
#endif
static BOOL rdna4_capture_scan_matches(const struct rdna4n_scan_status *status, int slot, uint64_t presentId) {
    return status->acquired && !status->flags && status->presents == presentId &&
        status->front_slot == (uint32_t)slot &&
        status->pending_slot == RDNA4N_SCAN_NO_SLOT && status->slot[slot].mc &&
        status->earliest_mc == status->slot[slot].mc && status->plane_mc == status->earliest_mc;
}
static BOOL rdna4_present_capture_on(void) {
    static _Atomic uint64_t next;
    static _Atomic int enabled;
    uint64_t now = rdna4_now(), due = atomic_load(&next);
    if (now >= due && atomic_compare_exchange_strong(&next, &due, now + NSEC_PER_SEC)) {
        struct stat st;
        atomic_store(&enabled, lstat(RDNA4_PRESENT_CAPTURE_FLAG, &st) == 0 && S_ISREG(st.st_mode) &&
            (st.st_uid == 0 || st.st_uid == geteuid()));
    }
    return atomic_load(&enabled) != 0;
}
static BOOL rdna4_capture_scan_status(struct rdna4n_scan_status *status) {
    pthread_mutex_lock(&RDNA4S.mu);
    const BOOL ready = RDNA4S.fs && RDNA4S.sm.state == RDNA4DF_ACTIVE && !RDNA4S.fs(RDNA4R.dev, status);
    pthread_mutex_unlock(&RDNA4S.mu);
    return ready;
}

static const uint8_t *rdna4_capture_source_plane(IOSurfaceRef source, uint32_t width,
                                                  uint32_t height, size_t pitch) {
    if (!source || IOSurfaceGetPixelFormat(source) != RDNA4S_FOURCC_BGRA) return NULL;
    const size_t planes = IOSurfaceGetPlaneCount(source);
    if (planes > 1) return NULL;
    const size_t planeWidth = planes ? IOSurfaceGetWidthOfPlane(source, 0) : IOSurfaceGetWidth(source);
    const size_t planeHeight = planes ? IOSurfaceGetHeightOfPlane(source, 0) : IOSurfaceGetHeight(source);
    const size_t element = planes ? IOSurfaceGetBytesPerElementOfPlane(source, 0) : IOSurfaceGetBytesPerElement(source);
    const size_t rowPitch = planes ? IOSurfaceGetBytesPerRowOfPlane(source, 0) : IOSurfaceGetBytesPerRow(source);
    if (planeWidth != width || planeHeight != height || element != 4 || rowPitch != pitch) return NULL;
    const uint8_t *allocation = IOSurfaceGetBaseAddress(source);
    const uint8_t *plane = planes ? IOSurfaceGetBaseAddressOfPlane(source, 0) : allocation;
    if (!allocation || (uintptr_t)plane < (uintptr_t)allocation) return NULL;
    const rdna4pl_in geometry = { .pbpe = element, .pbpr = pitch, .ph = height,
        .poff = (uintptr_t)plane - (uintptr_t)allocation, .alloc = IOSurfaceGetAllocSize(source) };
    rdna4pl_out bounds;
    return rdna4pl_bound(&geometry, &bounds) ? plane : NULL;
}

static void rdna4_capture_present(int inst, int slot, uint32_t sid, uint64_t seq, int rc, uint64_t presentId) {
    if (rc || inst || slot < 0 || slot >= RDNA4DF_MAX_SLOTS || !rdna4_is_ws() || !rdna4_present_capture_on()) return;
    static _Atomic uint64_t next;
    static _Atomic int writing;
    const uint64_t now = rdna4_now();
    if (now < atomic_load(&next) || atomic_exchange(&writing, 1)) return;
    struct stat flagStat;
    const BOOL full = lstat(RDNA4_PRESENT_FULL_FLAG, &flagStat) == 0 && S_ISREG(flagStat.st_mode) &&
        (flagStat.st_uid == 0 || flagStat.st_uid == geteuid());
    atomic_store(&next, now + (full ? 5 * NSEC_PER_SEC : NSEC_PER_SEC / 2));
    const uint32_t width = RDNA4S.pw, height = RDNA4S.ph, kernelSlot = RDNA4S.sid[slot];
    const VkBuffer slotBuffer = RDNA4S.buf[slot];
    const size_t pitch = RDNA4S.pitch, row = (size_t)width * 4;
    const size_t budget = (full ? 64u : 3u) * 1024u * 1024u;
    const size_t copies = full ? 1u : 3u;
    if (!slotBuffer || kernelSlot >= RDNA4N_SCAN_MAX_SLOTS || !row || pitch < row || row > budget / copies) {
        atomic_store(&writing, 0);
        return;
    }
    size_t rows = full ? height : MIN(height, 64u);
    const size_t maxRows = MIN(budget / (copies * row), 1 + (budget - row) / pitch);
    if (!rows || (full && rows > maxRows)) { atomic_store(&writing, 0); return; }
    rows = MIN(rows, maxRows);
    RDNA4S.sm.pin[slot]++;
    static dispatch_queue_t writer;
    static NSMutableArray *saved;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        writer = dispatch_queue_create("org.garnet.present-capture", DISPATCH_QUEUE_SERIAL);
        saved = [NSMutableArray array];
    });
    dispatch_async(writer, ^{
        @autoreleasepool {
            const uint64_t start = rdna4_now();
            char path[1024];
            const BOOL directoryReady = rdna4_dump_dir_ready(path, sizeof path);
            IOSurfaceRef source = directoryReady && sid ? IOSurfaceLookup(sid) : NULL;
            const uint8_t *base = rdna4_capture_source_plane(source, width, height, pitch);
            const BOOL sourceValid = base != NULL;
            const BOOL captureSource = sourceValid && !full;
            const size_t bytes = rows * row, slotOffset = captureSource ? bytes : 0;
            NSMutableData *data = directoryReady ? [NSMutableData dataWithLength:bytes * (captureSource ? 3 : 1)] : nil;
            uint8_t *out = data.mutableBytes;
            const uint32_t seedBefore = source ? IOSurfaceGetSeed(source) : 0;
            if (out && captureSource) for (size_t y = 0; y < rows; y++) memcpy(out + y * row, base + y * pitch, row);
            struct rdna4n_scan_status scanBefore = {0}, scanAfter = {0};
            BOOL scanReady = NO;
            const uint64_t pollStart = rdna4_now();
            if (out && presentId) for (unsigned attempt = 0; attempt < 100; attempt++) {
                if (!rdna4_capture_scan_status(&scanBefore)) break;
                if (rdna4_capture_scan_matches(&scanBefore, kernelSlot, presentId)) { scanReady = YES; break; }
                const BOOL awaitingFetch = scanBefore.pending_slot == RDNA4N_SCAN_NO_SLOT &&
                    scanBefore.front_slot == kernelSlot && scanBefore.slot[kernelSlot].mc &&
                    scanBefore.plane_mc == scanBefore.slot[kernelSlot].mc;
                if (!scanBefore.acquired || scanBefore.flags || scanBefore.presents != presentId ||
                    (scanBefore.pending_slot != kernelSlot && !awaitingFetch)) break;
                usleep(1000);
            }
            const uint64_t pollCost = rdna4_now() - pollStart;
            BOOL unresolved = NO;
            const BOOL copied = out && rdna4_capture_readback(slotBuffer, row, pitch, rows, out + slotOffset, &unresolved);
            const BOOL scanVerified = copied && scanReady && rdna4_capture_scan_status(&scanAfter) &&
                rdna4_capture_scan_matches(&scanAfter, kernelSlot, presentId) && scanBefore.earliest_mc == scanAfter.earliest_mc &&
                scanBefore.slot[kernelSlot].latched_frame &&
                scanBefore.slot[kernelSlot].latched_frame == scanAfter.slot[kernelSlot].latched_frame;
            if (copied && captureSource) for (size_t y = 0; y < rows; y++) memcpy(out + 2 * bytes + y * row, base + y * pitch, row);
            const uint32_t seedAfter = source ? IOSurfaceGetSeed(source) : 0;
            if (source) CFRelease(source);
            pthread_mutex_lock(&RDNA4S.mu);
            const uint64_t submission = RDNA4S.sm.submit_hi;
            if (!unresolved) RDNA4S.sm.pin[slot]--;
            pthread_mutex_unlock(&RDNA4S.mu);
            const uint64_t cost = rdna4_now() - start;
            const uint64_t copyCost = cost - pollCost;
            if (!full && copyCost > NSEC_PER_SEC / 200)
                atomic_store(&next, now + 100 * MIN(NSEC_PER_SEC / 10, copyCost));
            BOOL okay = NO;
            if (copied) {
                NSString *stem = [NSString stringWithFormat:@"%s/present-%d-%llu-%llu", path, getpid(),
                    (unsigned long long)seq, (unsigned long long)now];
                NSMutableDictionary *record = [@{ @"sequence": @(seq), @"surface": @(sid), @"slot": @(slot),
                    @"present_id": @(presentId), @"capture_mode": full ? @"full" : @"top",
                    @"time_ns": @(now), @"cost_ns": @(cost), @"latest_submission": @(submission),
                    @"width": @(width), @"height": @(height), @"captured_rows": @(rows), @"bytes_per_row": @(row),
                    @"pixel_format": @"BGRA8", @"source_valid": @(sourceValid), @"slot_offset": @(slotOffset),
                    @"source_before_offset": @0, @"source_after_offset": @(2 * bytes),
                    @"source_unchanged": @(captureSource && !memcmp(out, out + 2 * bytes, bytes)),
                    @"source_matches_slot": @(captureSource && !memcmp(out, out + slotOffset, bytes)),
                    @"full_frame": rows == height ? @YES : @NO, @"scanout_verified": @(scanVerified),
                    @"scanout_before_present": @(scanBefore.presents), @"scanout_after_present": @(scanAfter.presents),
                    @"scanout_before_mc": @(scanBefore.earliest_mc), @"scanout_after_mc": @(scanAfter.earliest_mc),
                    @"scanout_slot_mc": @(scanBefore.slot[kernelSlot].mc),
                    @"scanout_latched_frame": @(scanBefore.slot[kernelSlot].latched_frame),
                    @"seed_before": @(seedBefore), @"seed_after": @(seedAfter),
                    @"raw_file": [[stem lastPathComponent] stringByAppendingString:@".raw"] } mutableCopy];
                char boot[64] = {0};
                size_t bootSize = sizeof boot - 1;
                if (sysctlbyname("kern.bootsessionuuid", boot, &bootSize, NULL, 0) == 0)
                    record[@"boot_uuid"] = [NSString stringWithUTF8String:boot];
                if (full) record[@"slot_sha256"] = rdna4_sha256hex(data);
                NSData *json = [NSJSONSerialization dataWithJSONObject:record options:0 error:NULL];
                okay = json && rdna4_dump_write([[stem stringByAppendingString:@".raw"] fileSystemRepresentation], data) &&
                    rdna4_dump_write([[stem stringByAppendingString:@".json"] fileSystemRepresentation], json);
                if (okay) [saved addObject:stem];
                else { unlink([[stem stringByAppendingString:@".raw"] fileSystemRepresentation]); unlink([[stem stringByAppendingString:@".json"] fileSystemRepresentation]); }
                while (saved.count > (full ? 2u : 16u)) {
                    NSString *old = saved.firstObject;
                    unlink([[old stringByAppendingString:@".raw"] fileSystemRepresentation]);
                    unlink([[old stringByAppendingString:@".json"] fileSystemRepresentation]);
                    [saved removeObjectAtIndex:0];
                }
            }
            RDNA4LOG("scanout: %s capture seq %llu slot %d id %llu status=%s bytes=%lu cost=%.1f us verified=%d", full ? "full" : "top", (unsigned long long)seq,
                slot, (unsigned long long)presentId, okay ? "saved" : "failed", (unsigned long)data.length, cost / 1000.0, scanVerified);
            if (!unresolved) atomic_store(&writing, 0);
        }
    });
}
#endif // GARNET_PRESENT_CAPTURE_ONLY
