// garnet_hotswap.h - pure state machine for pipeline HOT-SWAP (native #12 hot-swap; no Metal, no Vulkan, no I/O; host test test-hotswap.c).
//
// A fallback pipeline object (spvcache miss in WindowServer) registers one rdna4hs_entry per object. A watcher thread feeds it file
// observations (stat() of <sha>.spv / <sha>.meta.json in the lookup directories) every tick; the entry answers BUILD exactly when the
// files are complete and STABLE (same size+mtime on two consecutive ticks and older than RDNA4HS_MIN_AGE_NS: a half-copied file never
// builds). The builder reports back with build_done(ok): ok -> SWAPPED (the caller publishes the real object with rdna4hs_install, one
// atomic compare-and-swap, once), failure -> backoff and up to RDNA4HS_MAX_ATTEMPTS tries, then GAVEUP (terminal).
//
// Swap safety bookkeeping (deferred destroy): users[0] counts holders of the FALLBACK generation (command buffers that encoded with
// it), users[1] holders of the real one. acquire() always hands out generation 1 once SWAPPED, 0 before; a holder keeps the generation
// it got. The fallback's Vulkan objects may be destroyed only when destroy_ok() says so: swapped AND users[0] == 0 AND not yet destroyed
// (it answers yes exactly once). In the bundle the same invariant is carried by ARC: every command buffer retains the pipeline object it
// encoded with (rdna4Retain), the fallback object keeps its VkPipeline/layout/module alive until its dealloc, and dealloc cannot run while a
// command buffer holds it.
#ifndef RDNA4_HOTSWAP_H
#define RDNA4_HOTSWAP_H
#include <stdint.h>
#include <string.h>
#include <stdatomic.h>

#define RDNA4HS_MAXKEY        2                      // render: vertex + fragment; compute: kernel
#define RDNA4HS_POLL_NS       20000000LL             // 20 ms watcher poll interval while a fallback pipeline is pending
#define RDNA4HS_MIN_AGE_NS    20000000LL             // newest file must be at least this old (atomic rename + 2 identical ticks)
#define RDNA4HS_META_GRACE_NS 5000000000LL           // a render pipeline proceeds without a .meta.json after the .spv has been stable this long
#define RDNA4HS_MAX_ATTEMPTS  4

typedef struct { int present; uint64_t size; int64_t mtime_ns; } rdna4hs_obs;
typedef struct { rdna4hs_obs spv, meta; } rdna4hs_key_obs;
typedef enum { RDNA4HS_WAIT = 0, RDNA4HS_BUILDING, RDNA4HS_SWAPPED, RDNA4HS_GAVEUP } rdna4hs_state;
typedef enum { RDNA4HS_NONE = 0, RDNA4HS_BUILD } rdna4hs_action;

typedef struct {
    int nkeys, meta_required;
    rdna4hs_state st;
    rdna4hs_key_obs last[RDNA4HS_MAXKEY]; int have_last;
    int attempts; int64_t next_try_ns;
    uint32_t users[2];                  // [0] fallback generation, [1] real generation
    int fb_retired, fb_destroyed;
} rdna4hs_entry;

static inline void rdna4hs_init(rdna4hs_entry *e, int nkeys, int meta_required) {
    memset(e, 0, sizeof *e);
    e->nkeys = nkeys < 1 ? 1 : nkeys > RDNA4HS_MAXKEY ? RDNA4HS_MAXKEY : nkeys; e->meta_required = meta_required;
}
static inline int rdna4hs_obs_eq(const rdna4hs_obs *a, const rdna4hs_obs *b) { return a->present == b->present && a->size == b->size && a->mtime_ns == b->mtime_ns; }
static inline int rdna4hs_spv_ok(const rdna4hs_obs *o) { return o->present && o->size >= 20 && (o->size & 3) == 0; }   // same sanity as the lookup

// One watcher tick. now_ns is wall-clock (same clock as mtime_ns). Returns BUILD at most once per attempt (state -> BUILDING).
static inline rdna4hs_action rdna4hs_tick(rdna4hs_entry *e, int64_t now_ns, const rdna4hs_key_obs *cur) {
    if (e->st != RDNA4HS_WAIT) return RDNA4HS_NONE;
    int same = e->have_last, all_spv = 1, all_meta = 1; int64_t newest = 0;
    for (int k = 0; k < e->nkeys; k++) {
        if (!rdna4hs_obs_eq(&e->last[k].spv, &cur[k].spv) || !rdna4hs_obs_eq(&e->last[k].meta, &cur[k].meta)) same = 0;
        if (!rdna4hs_spv_ok(&cur[k].spv)) all_spv = 0; else if (cur[k].spv.mtime_ns > newest) newest = cur[k].spv.mtime_ns;
        if (!cur[k].meta.present || !cur[k].meta.size) all_meta = 0; else if (cur[k].meta.mtime_ns > newest) newest = cur[k].meta.mtime_ns;
    }
    if (e->have_last && !same) { e->attempts = 0; e->next_try_ns = 0; }   // the files changed: a new content, a fresh try budget
    memcpy(e->last, cur, sizeof(rdna4hs_key_obs) * (size_t)e->nkeys); e->have_last = 1;
    if (!all_spv || !same) return RDNA4HS_NONE;
    if (now_ns - newest < RDNA4HS_MIN_AGE_NS) return RDNA4HS_NONE;
    if (!all_meta && (e->meta_required || now_ns - newest < RDNA4HS_META_GRACE_NS)) return RDNA4HS_NONE;
    if (now_ns < e->next_try_ns) return RDNA4HS_NONE;
    e->st = RDNA4HS_BUILDING; e->attempts++;
    return RDNA4HS_BUILD;
}
// The builder finished. Returns 1 when the entry is now SWAPPED (publish the real object), 0 otherwise.
static inline int rdna4hs_build_done(rdna4hs_entry *e, int ok, int64_t now_ns) {
    if (e->st != RDNA4HS_BUILDING) return 0;
    if (ok) { e->st = RDNA4HS_SWAPPED; e->fb_retired = 1; return 1; }
    if (e->attempts >= RDNA4HS_MAX_ATTEMPTS) { e->st = RDNA4HS_GAVEUP; return 0; }
    e->st = RDNA4HS_WAIT; e->next_try_ns = now_ns + (1000000000LL << e->attempts);
    return 0;
}
// A user (command buffer / encoder) picks a generation. After the swap new users always get 1; a user keeps the generation it was given.
static inline int rdna4hs_acquire(rdna4hs_entry *e) { int g = e->st == RDNA4HS_SWAPPED ? 1 : 0; e->users[g]++; return g; }
static inline void rdna4hs_release(rdna4hs_entry *e, int gen) { if (gen >= 0 && gen <= 1 && e->users[gen]) e->users[gen]--; }
// 1 exactly once: swapped, nobody holds the fallback generation any more.
static inline int rdna4hs_destroy_ok(rdna4hs_entry *e) {
    if (!e->fb_retired || e->fb_destroyed || e->users[0]) return 0;
    e->fb_destroyed = 1; return 1;
}

// Publication slot: the real object pointer, written once (0 -> v) by the watcher, read lock-free by the encoders.
static inline int rdna4hs_install(_Atomic uintptr_t *slot, uintptr_t v) { uintptr_t exp = 0; return atomic_compare_exchange_strong(slot, &exp, v); }
static inline uintptr_t rdna4hs_load(_Atomic uintptr_t *slot) { return atomic_load_explicit(slot, memory_order_acquire); }
#endif
