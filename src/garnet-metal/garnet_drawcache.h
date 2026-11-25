// garnet_drawcache.h: P5b "per-draw redundancy" decisions as plain C (no Vulkan, no Foundation; host test: test-drawcache.c). GarnetDevice.m does the Vulkan calls.
//  (A) DESCRIPTOR-SET SIGNATURE.  A signature is the exact list of 64-bit words a stage's descriptor set is built from (set layout handle first, then per slot the
//      object / handle / offset the slot holds). Two draws with equal signatures would fill byte-identical sets, so the second may re-use the first's VkDescriptorSet.
//      The encoder keeps the last signature + set per stage; ANY difference (an offset-only change, another texture, another layout) means a new set.
//  (B) LAST-STATE CACHES per render encoder: last bound VkPipeline, and the last (topology, cull, front, pso) -> VkPipeline answer.
//  (C) RENDER-PASS CACHE (device-wide): key = every field of every attachment description + attachment count + fetch shape; handles are never removed.
#ifndef RDNA4_DRAWCACHE_H
#define RDNA4_DRAWCACHE_H
#include <stdint.h>
#include <string.h>

#define RDNA4DC_MAXW     256   // longest cacheable signature (words); a longer one is never cached (always a fresh set)
#define RDNA4DC_PC_CAP   8u    // per-encoder pipelineForTopology cache slots
#define RDNA4DC_LRU_CAP  8u    // per-stage same-segment descriptor-set reuse slots
#define RDNA4DC_LRU_MAXW 32u   // max signature words kept in the segment LRU (covers layout+dsl+15 bindings)

typedef struct { uint32_t n; int over; uint64_t w[RDNA4DC_MAXW]; } rdna4dc_sig;
static inline void rdna4dc_sig_begin(rdna4dc_sig *s, uint64_t layout, uint64_t dsl) { s->n = 0; s->over = 0; s->w[s->n++] = layout; s->w[s->n++] = dsl; }
static inline void rdna4dc_sig_push(rdna4dc_sig *s, uint64_t v) { if (s->n >= RDNA4DC_MAXW) { s->over = 1; return; } s->w[s->n++] = v; }

typedef struct { int valid; uint32_t n; uint64_t w[RDNA4DC_MAXW]; uint64_t set; } rdna4dc_stage;
static inline void rdna4dc_stage_reset(rdna4dc_stage *t) { t->valid = 0; t->n = 0; t->set = 0; }
// YES when the stage holds a set built from exactly this signature (then *set is that set).
static inline int rdna4dc_stage_match(const rdna4dc_stage *t, const rdna4dc_sig *s, uint64_t *set) {
    if (s->over || !t->valid || t->n != s->n || memcmp(t->w, s->w, (size_t)s->n * sizeof s->w[0]) != 0) return 0;
    *set = t->set; return 1;
}
static inline void rdna4dc_stage_store(rdna4dc_stage *t, const rdna4dc_sig *s, uint64_t set) {
    if (s->over) { rdna4dc_stage_reset(t); return; }
    t->valid = 1; t->n = s->n; memcpy(t->w, s->w, (size_t)s->n * sizeof s->w[0]); t->set = set;
}

typedef struct { uint32_t topo, cull, front; const void *pso; uint64_t pipe; } rdna4dc_pce;
typedef struct { uint32_t n; uint64_t set; uint64_t w[RDNA4DC_LRU_MAXW]; } rdna4dc_lru_ent;

typedef struct {
    uint64_t layout;                 // pipeline layout the cached sets were bound with
    rdna4dc_stage st[2];             // 0 = vertex set (index 0), 1 = fragment set (index 1)
    uint64_t lastPipe;               // last VkPipeline bound in the current render pass (0 = none)
    int pcValid;                     // count of valid entries in pc[] (0..RDNA4DC_PC_CAP)
    uint32_t pcNext;                 // round-robin eviction cursor for pc[]
    rdna4dc_pce pc[RDNA4DC_PC_CAP];  // recent (topology, cull, front, pso) -> VkPipeline answers
    uint8_t lruN[2], lruNext[2];     // populated entries + eviction cursor per stage in the current segment
    rdna4dc_lru_ent lru[2][RDNA4DC_LRU_CAP];
    uint64_t draws, setsReused, setsAllocated, pipesSkipped, pipesBound, pcHits;
} rdna4dc_enc;
static inline void rdna4dc_enc_init(rdna4dc_enc *e) {
    e->layout = 0; e->lastPipe = 0; e->pcValid = 0; e->pcNext = 0;
    rdna4dc_stage_reset(&e->st[0]); rdna4dc_stage_reset(&e->st[1]);
    e->lruN[0] = e->lruN[1] = e->lruNext[0] = e->lruNext[1] = 0;
    e->draws = e->setsReused = e->setsAllocated = e->pipesSkipped = e->pipesBound = e->pcHits = 0;
}
// New render pass / barrier inside the same segment: forget what is currently bound (segment-owned LRU sets stay valid).
static inline void rdna4dc_enc_reset_bound(rdna4dc_enc *e) { e->lastPipe = 0; e->layout = 0; rdna4dc_stage_reset(&e->st[0]); rdna4dc_stage_reset(&e->st[1]); }
// Segment boundary: descriptor pools of the previous segment may retire, so drop segment-scoped LRU sets too.
static inline void rdna4dc_enc_reset_segment(rdna4dc_enc *e) {
    rdna4dc_enc_reset_bound(e);
    e->lruN[0] = e->lruN[1] = e->lruNext[0] = e->lruNext[1] = 0;
}
// A draw binds a pipeline with this layout: a different layout forgets the currently bound sets (LRU keeps layout in w[0]).
static inline void rdna4dc_enc_layout(rdna4dc_enc *e, uint64_t layout) { if (e->layout != layout) { e->layout = layout; rdna4dc_stage_reset(&e->st[0]); rdna4dc_stage_reset(&e->st[1]); } }
// YES when a previously filled immutable set in the current segment matches `s`; caller must still vkCmdBindDescriptorSets.
static inline int rdna4dc_enc_lru_find(const rdna4dc_enc *e, int idx, const rdna4dc_sig *s, uint64_t *set) {
    if (s->over || s->n < 2u || s->n > RDNA4DC_LRU_MAXW || (unsigned)idx >= 2u) return 0;
    const uint8_t n = e->lruN[idx];
    const rdna4dc_lru_ent *arr = e->lru[idx];
    for (uint8_t i = 0; i < n; i++) {
        const rdna4dc_lru_ent *ent = &arr[i];
        if (ent->n == s->n && ent->w[0] == s->w[0] && ent->w[1] == s->w[1] &&
            (s->n == 2u || memcmp(ent->w + 2, s->w + 2, (size_t)(s->n - 2u) * sizeof s->w[0]) == 0)) {
            *set = ent->set; return 1;
        }
    }
    return 0;
}
static inline void rdna4dc_enc_lru_put(rdna4dc_enc *e, int idx, const rdna4dc_sig *s, uint64_t set) {
    if (s->over || s->n < 2u || s->n > RDNA4DC_LRU_MAXW || !set || (unsigned)idx >= 2u) return;
    uint8_t slot;
    if (e->lruN[idx] < RDNA4DC_LRU_CAP) slot = e->lruN[idx]++;
    else { slot = e->lruNext[idx]; e->lruNext[idx] = (uint8_t)((slot + 1u) & (RDNA4DC_LRU_CAP - 1u)); }
    rdna4dc_lru_ent *ent = &e->lru[idx][slot];
    ent->n = s->n; ent->set = set; memcpy(ent->w, s->w, (size_t)s->n * sizeof s->w[0]);
}
// YES when vkCmdBindPipeline may be skipped (same pipeline as the last one bound in this pass).
static inline int rdna4dc_enc_pipe_same(rdna4dc_enc *e, uint64_t pipe) { if (e->lastPipe == pipe && pipe) { e->pipesSkipped++; return 1; } e->lastPipe = pipe; e->pipesBound++; return 0; }
static inline int rdna4dc_enc_pc_get(rdna4dc_enc *e, uint32_t topo, uint32_t cull, uint32_t front, const void *pso, uint64_t *pipe) {
    for (int i = 0; i < e->pcValid; i++) {
        const rdna4dc_pce *c = &e->pc[i];
        if (c->pso == pso && c->topo == topo && c->cull == cull && c->front == front) { *pipe = c->pipe; e->pcHits++; return 1; }
    }
    return 0;
}
static inline void rdna4dc_enc_pc_put(rdna4dc_enc *e, uint32_t topo, uint32_t cull, uint32_t front, const void *pso, uint64_t pipe) {
    for (int i = 0; i < e->pcValid; i++) {
        rdna4dc_pce *c = &e->pc[i];
        if (c->pso == pso && c->topo == topo && c->cull == cull && c->front == front) {
            if (pipe) c->pipe = pipe;
            else e->pc[i] = e->pc[--e->pcValid];
            return;
        }
    }
    if (!pipe) return;
    uint32_t slot;
    if ((uint32_t)e->pcValid < RDNA4DC_PC_CAP) slot = (uint32_t)(e->pcValid++);
    else { slot = e->pcNext; e->pcNext = (slot + 1u) & (RDNA4DC_PC_CAP - 1u); }
    e->pc[slot] = (rdna4dc_pce){ topo, cull, front, pso, pipe };
}

// ---- (C) render-pass cache ----
#define RDNA4DC_RP_CAP 64
typedef struct { uint32_t format, samples, loadOp, storeOp, stencilLoadOp, stencilStoreOp, initialLayout, finalLayout; } rdna4dc_att;
typedef struct { uint32_t na, fetch; rdna4dc_att a[8]; } rdna4dc_rpkey;
static inline int rdna4dc_key_eq(const rdna4dc_rpkey *x, const rdna4dc_rpkey *y) {
    if (x->na != y->na || x->fetch != y->fetch) return 0;
    for (uint32_t i = 0; i < x->na && i < 8; i++) {
        const rdna4dc_att *p = &x->a[i], *q = &y->a[i];
        if (p->format != q->format || p->samples != q->samples || p->loadOp != q->loadOp || p->storeOp != q->storeOp || p->stencilLoadOp != q->stencilLoadOp ||
            p->stencilStoreOp != q->stencilStoreOp || p->initialLayout != q->initialLayout || p->finalLayout != q->finalLayout) return 0;
    }
    return 1;
}
typedef struct { uint32_t n; rdna4dc_rpkey k[RDNA4DC_RP_CAP]; uint64_t h[RDNA4DC_RP_CAP]; uint64_t hits, created, uncached; } rdna4dc_rpcache;
static inline uint64_t rdna4dc_rp_find(rdna4dc_rpcache *c, const rdna4dc_rpkey *k) {
    for (uint32_t i = 0; i < c->n; i++) if (rdna4dc_key_eq(&c->k[i], k)) { c->hits++; return c->h[i]; }
    return 0;
}
// YES when the handle was entered (the caller must then never destroy it); NO when the cache is full (the caller keeps ownership as before).
static inline int rdna4dc_rp_add(rdna4dc_rpcache *c, const rdna4dc_rpkey *k, uint64_t h) {
    if (c->n >= RDNA4DC_RP_CAP || !h) { c->uncached++; return 0; }
    c->k[c->n] = *k; c->h[c->n] = h; c->n++; c->created++; return 1;
}
#endif
