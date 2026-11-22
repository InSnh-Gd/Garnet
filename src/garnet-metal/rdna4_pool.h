// rdna4_pool.h: P1 "memory pooling" decisions as plain C (no Vulkan, no Foundation; host test: test-pool.c). GarnetDevice.m does the Vulkan calls and holds ONE mutex
// around every call into this header. Time is a parameter (ns) so the tests control it.
//
//  (a) FENCE.  `submitted` is a monotonic serial taken when a command buffer is handed to vkQueueSubmit; `completed` is the highest S such that every command buffer
//      with serial <= S has finished (min(pending) - 1, or `submitted` when nothing is pending). Anything freed is stamped with `submitted` at free time and may be
//      reused only when completed >= stamp. A serial that is never marked done (a hung GPU) pins `completed`: nothing newer is ever reused (safe, not live).
//  (b) SLABS.  Allocations <= RDNA4P_SUB_MAX with alignment <= RDNA4P_SLAB_SIZE are sub-allocated from 4 MiB slabs, one set of slabs per key (the caller's memory type
//      and map class). Free ranges are kept sorted and coalesced. A slab that has been empty for >= RDNA4P_IDLE_NS is released by rdna4p_trim.
//  (c) RECYCLE.  Whole allocations of RDNA4P_SUB_MAX < size <= RDNA4P_REC_MAX are kept, after the fence has cleared, in a cache keyed by (key, size), total <= RDNA4P_REC_CAP
//      (oldest evicted first); an entry unused for > RDNA4P_IDLE_NS is released by rdna4p_trim.
//  The pool never calls Vulkan: memory it gives up goes to a release queue (rdna4p_pop_release) that the caller drains and vkFreeMemory's outside its mutex.
//  (d) A fence-gated FIFO of opaque 64-bit handles (descriptor pools): rdna4p_h_push / rdna4p_h_pop.
// Not reused: rdna4_heapalloc.h (RDNA4Heap's budget allocator) stores USED blocks of one heap and has neither a fence nor coalescing of free ranges, so it does not fit.
#ifndef RDNA4_POOL_H
#define RDNA4_POOL_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define RDNA4P_SLAB_SIZE       (4ULL << 20)
#define RDNA4P_GTT_SLAB_SIZE   (1ULL << 20)
#define RDNA4P_MIN_SLAB_SIZE   (512ULL << 10)
#define RDNA4P_SUB_MAX         (256ULL << 10)
#define RDNA4P_REC_MAX         (64ULL << 20)
#define RDNA4P_REC_CAP         (256ULL << 20)
#define RDNA4P_IDLE_NS         2000000000ULL
#define RDNA4P_SLAB_BACKOFF_NS 5000000000ULL
#define RDNA4P_MAX_KEYS        64u
#define RDNA4P_DEFER_MAX       (512ULL << 20)   // whole allocations waiting for the fence beyond this are freed at once (the kernel's idle wait protects them, as today)

typedef struct { uint64_t off, size; } rdna4p_rng;
typedef struct {
    uint64_t id, size, nlive, emptySince; uint32_t key; int empty;
    void *mem, *map; rdna4p_rng *fr; size_t nfr, cfr;
} rdna4p_slab;
typedef struct { uint32_t key; uint64_t size, lastUse; void *mem, *map; } rdna4p_rec;
typedef struct { int whole; uint64_t stamp, slab, off, size; uint32_t key; void *mem, *map; } rdna4p_def;
typedef struct { void *mem, *map; uint64_t size; uint32_t key; int slab; } rdna4p_rel;   // slab = 1: a whole slab, 0: a recycled whole allocation

typedef struct {
    int on;                                                                  // the P1 switch, latched once by the caller: when 0 every entry point below is a no-op / "not pooled"
    uint64_t submitted, completed; uint64_t *pend; size_t npend, cpend;     // (a)
    rdna4p_slab **sl; size_t nsl, csl; uint64_t slabCtr;                       // (b)
    rdna4p_rec *rc; size_t nrc, crc; uint64_t recBytes;                        // (c)
    rdna4p_def *df; size_t hd, nd, cd; uint64_t deferWhole;                    // deferred frees: FIFO, stamps non-decreasing
    rdna4p_rel *rl; size_t nrl, crl;                                           // release queue
    uint64_t *hv, *hs; size_t hh, hn, hc;                                    // (d) handle FIFO
    uint64_t slabSize[RDNA4P_MAX_KEYS], slabBackoffUntil[RDNA4P_MAX_KEYS];   // adaptive slab sizing + failure backoff (GTT contiguous RAM fragmentation)
    // statistics
    uint64_t subAlloc, subFree, subHit, slabNew, slabRel, recHit, recMiss, recRel, dpReuse, dpCreate, avoided, directAlloc;
} rdna4p_pool;

static inline void rdna4p_init(rdna4p_pool *p, int on) { memset(p, 0, sizeof *p); p->on = on; }
static inline void rdna4p_destroy(rdna4p_pool *p) {
    for (size_t i = 0; i < p->nsl; i++) { free(p->sl[i]->fr); free(p->sl[i]); }
    free(p->sl); free(p->rc); free(p->df); free(p->rl); free(p->pend); free(p->hv); free(p->hs); { int on = p->on; memset(p, 0, sizeof *p); p->on = on; }
}
#define RDNA4P_GROW(arr, n, cap, T) do { if ((n) == (cap)) { (cap) = (cap) ? (cap) * 2 : 16; (arr) = (T *)realloc((arr), (cap) * sizeof(T)); } } while (0)

// ---- (a) fence ------------------------------------------------------------------------------------------------------------------
static inline void rdna4p_fence_recompute(rdna4p_pool *p) { p->completed = p->npend ? p->pend[0] - 1 : p->submitted; }
static inline uint64_t rdna4p_submit(rdna4p_pool *p) {   // call under the same lock that orders the vkQueueSubmit calls; returns the serial
    if (!p->on || p->submitted == UINT64_MAX) return 0;
    if (p->npend == p->cpend) {
        if (p->cpend > SIZE_MAX / 2 / sizeof *p->pend) return 0;
        size_t cap = p->cpend ? p->cpend * 2 : 16;
        uint64_t *pending = (uint64_t *)realloc(p->pend, cap * sizeof *pending);
        if (!pending) return 0;
        p->pend = pending;
        p->cpend = cap;
    }
    const uint64_t s = p->submitted + 1;
    p->pend[p->npend++] = s;
    p->submitted = s;
    rdna4p_fence_recompute(p);
    return s;
}
// Withdraws the newest submit only if it was never published to Vulkan. The caller
// holds the same lock as rdna4p_submit and uses this to roll back a token acquired
// speculatively by a paired timeline.
static inline int rdna4p_cancel_last(rdna4p_pool *p, uint64_t serial) {
    if (!p->on || !serial || p->submitted != serial || !p->npend ||
        p->pend[p->npend - 1] != serial) return 0;
    p->npend--;
    p->submitted = serial - 1;
    rdna4p_fence_recompute(p);
    return 1;
}
static inline void rdna4p_done(rdna4p_pool *p, uint64_t serial) {   // idempotent
    if (!p->on) return;
    for (size_t i = 0; i < p->npend; i++) if (p->pend[i] == serial) { memmove(&p->pend[i], &p->pend[i + 1], (p->npend - i - 1) * sizeof *p->pend); p->npend--; break; }
    rdna4p_fence_recompute(p);
}

// ---- (b) slabs ------------------------------------------------------------------------------------------------------------------
static inline uint64_t rdna4p_alup(uint64_t v, uint64_t a) { if (a <= 1) return v; if (!(a & (a - 1))) return (v + a - 1) & ~(a - 1); return ((v + a - 1) / a) * a; }
static inline rdna4p_slab *rdna4p_find_slab(rdna4p_pool *p, uint64_t id) { for (size_t i = 0; i < p->nsl; i++) if (p->sl[i]->id == id) return p->sl[i]; return NULL; }
static inline uint64_t rdna4p_add_slab(rdna4p_pool *p, uint32_t key, uint64_t size, void *mem, void *map, uint64_t now) {
    rdna4p_slab *s = (rdna4p_slab *)calloc(1, sizeof *s);
    s->id = ++p->slabCtr; s->key = key; s->size = size; s->mem = mem; s->map = map; s->fr = (rdna4p_rng *)malloc(4 * sizeof *s->fr); s->cfr = 4; s->nfr = 1; s->fr[0] = (rdna4p_rng){ 0, size };
    s->empty = 1; s->emptySince = now;
    RDNA4P_GROW(p->sl, p->nsl, p->csl, rdna4p_slab *); p->sl[p->nsl++] = s; p->slabNew++; return s->id;
}
// First fit inside one slab. Returns 1 and the offset, or 0.
static inline int rdna4p_slab_take(rdna4p_slab *s, uint64_t size, uint64_t align, uint64_t *off) {
    for (size_t i = 0; i < s->nfr; i++) {
        uint64_t b = s->fr[i].off, e = b + s->fr[i].size, c = rdna4p_alup(b, align);
        if (c + size > e) continue;
        int front = c > b, back = c + size < e;
        if (front && back) {
            if (s->nfr == s->cfr) { s->cfr *= 2; s->fr = (rdna4p_rng *)realloc(s->fr, s->cfr * sizeof *s->fr); }
            memmove(&s->fr[i + 2], &s->fr[i + 1], (s->nfr - i - 1) * sizeof *s->fr); s->nfr++;
            s->fr[i] = (rdna4p_rng){ b, c - b }; s->fr[i + 1] = (rdna4p_rng){ c + size, e - (c + size) };
        } else if (front) s->fr[i].size = c - b;
        else if (back) s->fr[i] = (rdna4p_rng){ c + size, e - (c + size) };
        else { memmove(&s->fr[i], &s->fr[i + 1], (s->nfr - i - 1) * sizeof *s->fr); s->nfr--; }
        s->nlive++; s->empty = 0; *off = c; return 1;
    }
    return 0;
}
// Returns the range to the slab's sorted free list and merges it with its neighbours.
static inline void rdna4p_slab_give(rdna4p_slab *s, uint64_t off, uint64_t size, uint64_t now) {
    size_t i = 0; while (i < s->nfr && s->fr[i].off < off) i++;
    int mprev = i > 0 && s->fr[i - 1].off + s->fr[i - 1].size == off, mnext = i < s->nfr && off + size == s->fr[i].off;
    if (mprev && mnext) { s->fr[i - 1].size += size + s->fr[i].size; memmove(&s->fr[i], &s->fr[i + 1], (s->nfr - i - 1) * sizeof *s->fr); s->nfr--; }
    else if (mprev) s->fr[i - 1].size += size;
    else if (mnext) { s->fr[i].off = off; s->fr[i].size += size; }
    else {
        if (s->nfr == s->cfr) { s->cfr *= 2; s->fr = (rdna4p_rng *)realloc(s->fr, s->cfr * sizeof *s->fr); }
        memmove(&s->fr[i + 1], &s->fr[i], (s->nfr - i) * sizeof *s->fr); s->fr[i] = (rdna4p_rng){ off, size }; s->nfr++;
    }
    if (s->nlive) s->nlive--;
    if (!s->nlive && s->nfr == 1 && s->fr[0].off == 0 && s->fr[0].size == s->size) { s->empty = 1; s->emptySince = now; }
}

// ---- release queue and the recycle cache ------------------------------------------------------------------------------------------
static inline void rdna4p_release(rdna4p_pool *p, void *mem, void *map, uint64_t size, uint32_t key, int slab) {
    RDNA4P_GROW(p->rl, p->nrl, p->crl, rdna4p_rel); p->rl[p->nrl++] = (rdna4p_rel){ mem, map, size, key, slab };
}
// Pops one memory object the caller must now vkFreeMemory. Returns 1 and fills *o, or 0 when the queue is empty.
static inline int rdna4p_pop_release(rdna4p_pool *p, rdna4p_rel *o) {
    if (!p->nrl) return 0;
    *o = p->rl[0]; memmove(&p->rl[0], &p->rl[1], (p->nrl - 1) * sizeof *p->rl); p->nrl--; return 1;
}
static inline void rdna4p_rec_drop(rdna4p_pool *p, size_t i) {
    p->recBytes -= p->rc[i].size; rdna4p_release(p, p->rc[i].mem, p->rc[i].map, p->rc[i].size, p->rc[i].key, 0); p->recRel++;
    memmove(&p->rc[i], &p->rc[i + 1], (p->nrc - i - 1) * sizeof *p->rc); p->nrc--;
}
static inline void rdna4p_rec_put(rdna4p_pool *p, uint32_t key, uint64_t size, void *mem, void *map, uint64_t now) {
    if (size > RDNA4P_REC_CAP) { rdna4p_release(p, mem, map, size, key, 0); p->recRel++; return; }
    while (p->nrc && p->recBytes + size > RDNA4P_REC_CAP) { size_t o = 0; for (size_t i = 1; i < p->nrc; i++) if (p->rc[i].lastUse < p->rc[o].lastUse) o = i; rdna4p_rec_drop(p, o); }
    RDNA4P_GROW(p->rc, p->nrc, p->crc, rdna4p_rec); p->rc[p->nrc++] = (rdna4p_rec){ key, size, now, mem, map }; p->recBytes += size;
}

// ---- deferred frees ------------------------------------------------------------------------------------------------------------
static inline void rdna4p_apply(rdna4p_pool *p, const rdna4p_def *d, uint64_t now) {
    if (d->whole) { p->deferWhole -= d->size; rdna4p_rec_put(p, d->key, d->size, d->mem, d->map, now); return; }
    rdna4p_slab *s = rdna4p_find_slab(p, d->slab); if (s) rdna4p_slab_give(s, d->off, d->size, now);
}
// Applies every deferred free whose stamp the fence has cleared (the queue is in stamp order, so the head decides).
static inline void rdna4p_reap(rdna4p_pool *p, uint64_t now) {
    while (p->hd < p->nd && p->df[p->hd].stamp <= p->completed) rdna4p_apply(p, &p->df[p->hd++], now);
    if (p->hd == p->nd) p->hd = p->nd = 0;
}
static inline size_t rdna4p_waiting(const rdna4p_pool *p) { return p->nd - p->hd; }
static inline void rdna4p_defer(rdna4p_pool *p, rdna4p_def d) {
    if (p->hd && p->nd == p->cd) { memmove(p->df, &p->df[p->hd], (p->nd - p->hd) * sizeof *p->df); p->nd -= p->hd; p->hd = 0; }
    RDNA4P_GROW(p->df, p->nd, p->cd, rdna4p_def); p->df[p->nd++] = d;
}

// ---- allocation ----------------------------------------------------------------------------------------------------------------
enum { RDNA4P_R_SUB = 1, RDNA4P_R_REC, RDNA4P_R_NEED_SLAB, RDNA4P_R_NEED_WHOLE, RDNA4P_R_DIRECT };
typedef struct { int kind; uint64_t slab, off; void *mem, *map; } rdna4p_res;
// SUB: *off in slab r->slab (mem/map are the slab's: bind at off, contents = map + off). REC: a recycled whole allocation. NEED_SLAB: the caller allocates a slab
// (RDNA4P_SLAB_SIZE, key), calls rdna4p_add_slab and asks again. NEED_WHOLE: the caller allocates exactly `size`; give it back with rdna4p_free_whole.
// DIRECT: too large / over-aligned: the caller allocates and frees it itself, as before.
static inline int rdna4p_alloc(rdna4p_pool *p, uint32_t key, uint64_t size, uint64_t align, uint64_t now, rdna4p_res *r) {
    memset(r, 0, sizeof *r); if (align < 1) align = 1;
    if (!p->on) { r->kind = RDNA4P_R_DIRECT; return r->kind; }
    rdna4p_reap(p, now);
    if (!size) { r->kind = RDNA4P_R_DIRECT; p->directAlloc++; return r->kind; }
    if (size <= RDNA4P_SUB_MAX && align <= RDNA4P_SLAB_SIZE) {
        for (size_t i = 0; i < p->nsl; i++) {
            rdna4p_slab *s = p->sl[i]; uint64_t off;
            if (s->key == key && rdna4p_slab_take(s, size, align, &off)) { r->kind = RDNA4P_R_SUB; r->slab = s->id; r->off = off; r->mem = s->mem; r->map = s->map; p->subAlloc++; p->subHit++; p->avoided++; return r->kind; }
        }
        r->kind = RDNA4P_R_NEED_SLAB; return r->kind;
    }
    if (size > RDNA4P_SUB_MAX && size <= RDNA4P_REC_MAX) {
        for (size_t i = 0; i < p->nrc; i++) if (p->rc[i].key == key && p->rc[i].size == size) {
            r->kind = RDNA4P_R_REC; r->mem = p->rc[i].mem; r->map = p->rc[i].map; p->recBytes -= size;
            memmove(&p->rc[i], &p->rc[i + 1], (p->nrc - i - 1) * sizeof *p->rc); p->nrc--; p->recHit++; p->avoided++; return r->kind;
        }
        p->recMiss++; r->kind = RDNA4P_R_NEED_WHOLE; return r->kind;
    }
    r->kind = RDNA4P_R_DIRECT; p->directAlloc++; return r->kind;
}
// Accounts for a sub-allocation that followed an rdna4p_add_slab (the first allocation of a new slab is a miss, not an avoided kernel allocation).
static inline void rdna4p_note_new_slab_use(rdna4p_pool *p) { if (p->avoided) p->avoided--; if (p->subHit) p->subHit--; }
// Target slab size for a key: GTT starts at 1 MiB (contiguous XNU RAM fragments over uptime), device-local starts at 4 MiB.
// Returns 0 when backed off after failure or when the current slab size cannot fit `need`.
static inline uint64_t rdna4p_slab_target(const rdna4p_pool *p, uint32_t key, int deviceLocal, uint64_t need, uint64_t now) {
    if (key < RDNA4P_MAX_KEYS && p->slabBackoffUntil[key] && now < p->slabBackoffUntil[key]) return 0;
    uint64_t sz = (key < RDNA4P_MAX_KEYS && p->slabSize[key]) ? p->slabSize[key]
                : (deviceLocal ? RDNA4P_SLAB_SIZE : RDNA4P_GTT_SLAB_SIZE);
    return sz >= need ? sz : 0;
}
static inline uint64_t rdna4p_slab_stepdown(uint64_t cur, uint64_t need) {
    uint64_t next = cur >= RDNA4P_SLAB_SIZE ? RDNA4P_GTT_SLAB_SIZE : (cur >> 1);
    return (next >= RDNA4P_MIN_SLAB_SIZE && next >= need) ? next : 0;
}
static inline void rdna4p_slab_note_ok(rdna4p_pool *p, uint32_t key, uint64_t size) {
    if (key < RDNA4P_MAX_KEYS) { p->slabSize[key] = size; p->slabBackoffUntil[key] = 0; }
}
static inline void rdna4p_slab_note_fail(rdna4p_pool *p, uint32_t key, uint64_t now) {
    if (key < RDNA4P_MAX_KEYS) { p->slabSize[key] = RDNA4P_MIN_SLAB_SIZE; p->slabBackoffUntil[key] = now + RDNA4P_SLAB_BACKOFF_NS; }
}

// ---- frees ---------------------------------------------------------------------------------------------------------------------
static inline void rdna4p_free_sub(rdna4p_pool *p, uint64_t slab, uint64_t off, uint64_t size, uint64_t now) {
    if (!p->on) return;
    p->subFree++;
    rdna4p_def d = { 0, p->submitted, slab, off, size, 0, NULL, NULL };
    rdna4p_reap(p, now);
    if (d.stamp <= p->completed && p->hd == p->nd) rdna4p_apply(p, &d, now);   // nothing in flight and nothing queued ahead: reusable now
    else rdna4p_defer(p, d);
}
// Returns 1: the allocation now belongs to the pool (cached after the fence clears). 0: the caller must vkFreeMemory it now.
static inline int rdna4p_free_whole(rdna4p_pool *p, uint32_t key, uint64_t size, void *mem, void *map, uint64_t now) {
    if (!p->on) return 0;
    rdna4p_reap(p, now);
    if (size > RDNA4P_REC_MAX) return 0;
    if (p->deferWhole + size > RDNA4P_DEFER_MAX) return 0;
    rdna4p_def d = { 1, p->submitted, 0, 0, size, key, mem, map };
    if (d.stamp <= p->completed && p->hd == p->nd) rdna4p_rec_put(p, key, size, mem, map, now);
    else { p->deferWhole += size; rdna4p_defer(p, d); }
    return 1;
}

// ---- trimming ------------------------------------------------------------------------------------------------------------------
// Moves recycle entries unused for > RDNA4P_IDLE_NS and slabs empty for >= RDNA4P_IDLE_NS to the release queue. flushAll (out of memory): every cleared cached entry and empty slab.
static inline void rdna4p_trim_x(rdna4p_pool *p, uint64_t now, int flushAll) {
    rdna4p_reap(p, now);
    for (size_t i = 0; i < p->nrc;) { if (flushAll || (now > p->rc[i].lastUse && now - p->rc[i].lastUse > RDNA4P_IDLE_NS)) rdna4p_rec_drop(p, i); else i++; }
    for (size_t i = 0; i < p->nsl;) {
        rdna4p_slab *s = p->sl[i];
        if (s->empty && (flushAll || (now >= s->emptySince && now - s->emptySince >= RDNA4P_IDLE_NS))) {
            rdna4p_release(p, s->mem, s->map, s->size, s->key, 1); p->slabRel++; free(s->fr); free(s);
            memmove(&p->sl[i], &p->sl[i + 1], (p->nsl - i - 1) * sizeof *p->sl); p->nsl--;
        } else i++;
    }
}
static inline void rdna4p_trim(rdna4p_pool *p, uint64_t now) { rdna4p_trim_x(p, now, 0); }
static inline void rdna4p_flush(rdna4p_pool *p, uint64_t now) { rdna4p_trim_x(p, now, 1); }

// ---- (d) fence-gated handle FIFO (descriptor pools) ----------------------------------------------------------------------------------
// push: 0 when the list already holds `cap` handles (the caller destroys it). pop: the oldest handle whose stamp is cleared, or 0.
static inline int rdna4p_h_push(rdna4p_pool *p, uint64_t h, size_t cap) {
    if (!p->on || p->hn - p->hh >= cap) return 0;
    if (p->hh && p->hn == p->hc) { memmove(p->hv, &p->hv[p->hh], (p->hn - p->hh) * sizeof *p->hv); memmove(p->hs, &p->hs[p->hh], (p->hn - p->hh) * sizeof *p->hs); p->hn -= p->hh; p->hh = 0; }
    if (p->hn == p->hc) { p->hc = p->hc ? p->hc * 2 : 16; p->hv = (uint64_t *)realloc(p->hv, p->hc * sizeof *p->hv); p->hs = (uint64_t *)realloc(p->hs, p->hc * sizeof *p->hs); }
    p->hv[p->hn] = h; p->hs[p->hn] = p->submitted; p->hn++; return 1;
}
static inline uint64_t rdna4p_h_pop(rdna4p_pool *p) {
    if (p->on && p->hh < p->hn && p->hs[p->hh] <= p->completed) { uint64_t h = p->hv[p->hh++]; if (p->hh == p->hn) p->hh = p->hn = 0; p->dpReuse++; p->avoided++; return h; }
    return 0;
}
static inline size_t rdna4p_h_count(const rdna4p_pool *p) { return p->hn - p->hh; }

// ---- statistics ----------------------------------------------------------------------------------------------------------------
static inline int rdna4p_fmt(const rdna4p_pool *p, char *out, size_t cap) {
    char sk[160]; size_t o = 0; sk[0] = 0; uint32_t seen[16]; size_t ns = 0;
    for (size_t i = 0; i < p->nsl && ns < 16; i++) {
        uint32_t k = p->sl[i]->key; size_t j = 0; for (; j < ns; j++) if (seen[j] == k) break;
        if (j < ns) continue;
        seen[ns++] = k; unsigned n = 0; for (size_t q = 0; q < p->nsl; q++) n += p->sl[q]->key == k;
        int w = snprintf(sk + o, sizeof sk - o, "%s%u:%u", o ? "," : "", (unsigned)k, n); if (w < 0 || (size_t)w >= sizeof sk - o) break; o += (size_t)w;
    }
    return snprintf(out, cap, "T1 pool ON slabs(key:n)=[%s] new=%llu released=%llu live_sub=%llu recycle hit/miss=%llu/%llu (cached %zu, %llu KiB) descpool reuse/create=%llu/%llu (free %zu) deferred_waiting=%zu kernel_allocs_avoided=%llu fence submitted/completed=%llu/%llu",
        sk, (unsigned long long)p->slabNew, (unsigned long long)p->slabRel, (unsigned long long)(p->subAlloc - p->subFree), (unsigned long long)p->recHit, (unsigned long long)p->recMiss,
        p->nrc, (unsigned long long)(p->recBytes >> 10), (unsigned long long)p->dpReuse, (unsigned long long)p->dpCreate, rdna4p_h_count(p), rdna4p_waiting(p), (unsigned long long)p->avoided,
        (unsigned long long)p->submitted, (unsigned long long)p->completed);
}
#endif
