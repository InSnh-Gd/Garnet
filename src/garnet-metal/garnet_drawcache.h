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

#define RDNA4DC_MAXW     36    // longest cacheable signature (words); keeps RDNA4RenderEncoder slim (~6.3 KiB vs 8.5 KiB)
#define RDNA4DC_PC_CAP   8u    // pipelineForTopology cache slots
#define RDNA4DC_LRU_CAP  16u   // per-stage same-segment descriptor-set reuse slots (owned by RDNA4CommandBuffer)
#define RDNA4DC_LRU_MAXW 32u   // max signature words kept in the segment LRU (covers layout+dsl+15 bindings)

// Per-encoder dirty bits for skipping redundant texture/VB/descriptor work across draws.
#define GARNET_DRAW_DIRTY_PIPE   (1u << 0)
#define GARNET_DRAW_DIRTY_VSTAGE (1u << 1)
#define GARNET_DRAW_DIRTY_FSTAGE (1u << 2)
#define GARNET_DRAW_DIRTY_VB     (1u << 3)
#define GARNET_DRAW_DIRTY_PASS   (1u << 4)
#define GARNET_DRAW_DIRTY_VTEX   (1u << 5)
#define GARNET_DRAW_DIRTY_FTEX   (1u << 6)
#define GARNET_DRAW_DIRTY_ALL    (GARNET_DRAW_DIRTY_PIPE | GARNET_DRAW_DIRTY_VSTAGE | \
                                  GARNET_DRAW_DIRTY_FSTAGE | GARNET_DRAW_DIRTY_VB | \
                                  GARNET_DRAW_DIRTY_PASS | GARNET_DRAW_DIRTY_VTEX | GARNET_DRAW_DIRTY_FTEX)

static inline int rdna4dc_stage_can_skip(int valid, uint32_t draw_dirty, uint32_t stage_bit, int need_arg_prep) {
    return valid && !need_arg_prep && (draw_dirty & (stage_bit | GARNET_DRAW_DIRTY_PASS)) == 0u;
}
static inline int rdna4dc_stage_needs_tex(uint32_t draw_dirty, uint32_t tex_bit,
                                          uint32_t stage_bit, int has_embedded_tex) {
    if (draw_dirty & (tex_bit | GARNET_DRAW_DIRTY_PASS)) return 1;
    return has_embedded_tex && (draw_dirty & stage_bit) != 0u;
}
static inline int rdna4dc_clean_draw_ready(int clean_fast_ok, uint32_t draw_dirty, uint32_t dyn_dirty,
                                           int topo_same, int in_pass, int has_err, int has_arg_writes,
                                           int has_arg_res, int seg_same) {
    return clean_fast_ok && draw_dirty == 0u && dyn_dirty == 0u && topo_same &&
           in_pass && !has_err && !has_arg_writes && !has_arg_res && seg_same;
}

typedef struct { uint32_t n; int over; uint64_t hash; uint64_t w[RDNA4DC_MAXW]; } rdna4dc_sig;
static inline void rdna4dc_sig_begin(rdna4dc_sig *s, uint64_t layout, uint64_t dsl) {
    s->n = 2u; s->over = 0; s->w[0] = layout; s->w[1] = dsl;
    s->hash = (layout * 0x9e3779b97f4a7c15ULL) ^ dsl;
}
static inline void rdna4dc_sig_push(rdna4dc_sig *s, uint64_t v) {
    if (s->n >= RDNA4DC_MAXW) { s->over = 1; return; }
    s->w[s->n++] = v;
    s->hash = (s->hash ^ v) * 0x100000001b3ULL;
}

typedef struct { int valid; uint32_t n; uint64_t hash; uint64_t w[RDNA4DC_MAXW]; uint64_t set; } rdna4dc_stage;
static inline void rdna4dc_stage_reset(rdna4dc_stage *t) { t->valid = 0; t->n = 0; t->hash = 0; t->set = 0; }
// Inline word copy/compare: avoids PLT calls to _platform_memmove/memcmp on short 4..16 word signatures.
static inline void rdna4dc_copy_words(uint64_t *dst, const uint64_t *src, uint32_t n) {
    uint32_t i = 0;
    for (; i + 4u <= n; i += 4u) {
        dst[i] = src[i]; dst[i + 1u] = src[i + 1u]; dst[i + 2u] = src[i + 2u]; dst[i + 3u] = src[i + 3u];
    }
    if (i + 2u <= n) { dst[i] = src[i]; dst[i + 1u] = src[i + 1u]; i += 2u; }
    if (i < n) dst[i] = src[i];
}
static inline int rdna4dc_eq_words(const uint64_t *a, const uint64_t *b, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}
// YES when the stage holds a set built from exactly this signature (then *set is that set).
static inline int rdna4dc_stage_match(const rdna4dc_stage *t, const rdna4dc_sig *s, uint64_t *set) {
    if (s->over || !t->valid || t->hash != s->hash || t->n != s->n || !rdna4dc_eq_words(t->w, s->w, s->n)) return 0;
    *set = t->set; return 1;
}
static inline void rdna4dc_stage_store(rdna4dc_stage *t, const rdna4dc_sig *s, uint64_t set) {
    if (s->over) { rdna4dc_stage_reset(t); return; }
    t->valid = 1; t->n = s->n; t->hash = s->hash; rdna4dc_copy_words(t->w, s->w, s->n); t->set = set;
}

typedef struct { uint32_t topo, cull, front; const void *pso; uint64_t pipe; } rdna4dc_pce;
typedef struct { uint32_t n; uint64_t hash; uint64_t set; uint64_t w[RDNA4DC_LRU_MAXW]; } rdna4dc_lru_ent;

// Command-buffer-scoped cache shared across all render encoders in the same command buffer/segment.
// WindowServer averages <2 draws per render encoder (~40 encoders/CB); hoisting pc[] and lru[][] out of
// RDNA4RenderEncoder cuts per-encoder calloc/bzero and enables reuse across encoder boundaries.
typedef struct {
    int pcValid;
    uint32_t pcNext;
    rdna4dc_pce pc[RDNA4DC_PC_CAP];
    uint8_t lruN[2], lruNext[2];
    rdna4dc_lru_ent lru[2][RDNA4DC_LRU_CAP];
} rdna4dc_cbcache;
static inline void rdna4dc_cb_init(rdna4dc_cbcache *c) {
    c->pcValid = 0; c->pcNext = 0;
    c->lruN[0] = c->lruN[1] = c->lruNext[0] = c->lruNext[1] = 0;
}
static inline void rdna4dc_cb_reset_segment(rdna4dc_cbcache *c) {
    c->lruN[0] = c->lruN[1] = c->lruNext[0] = c->lruNext[1] = 0;
}
static inline int rdna4dc_cb_pc_get(const rdna4dc_cbcache *c, uint32_t topo, uint32_t cull, uint32_t front, const void *pso, uint64_t *pipe) {
    for (int i = 0; i < c->pcValid; i++) {
        const rdna4dc_pce *p = &c->pc[i];
        if (p->pso == pso && p->topo == topo && p->cull == cull && p->front == front) { *pipe = p->pipe; return 1; }
    }
    return 0;
}
static inline void rdna4dc_cb_pc_put(rdna4dc_cbcache *c, uint32_t topo, uint32_t cull, uint32_t front, const void *pso, uint64_t pipe) {
    for (int i = 0; i < c->pcValid; i++) {
        rdna4dc_pce *p = &c->pc[i];
        if (p->pso == pso && p->topo == topo && p->cull == cull && p->front == front) {
            if (pipe) p->pipe = pipe;
            else c->pc[i] = c->pc[--c->pcValid];
            return;
        }
    }
    if (!pipe) return;
    uint32_t slot;
    if ((uint32_t)c->pcValid < RDNA4DC_PC_CAP) slot = (uint32_t)(c->pcValid++);
    else { slot = c->pcNext; c->pcNext = (slot + 1u) & (RDNA4DC_PC_CAP - 1u); }
    c->pc[slot] = (rdna4dc_pce){ topo, cull, front, pso, pipe };
}
// YES when a previously filled immutable set in the current segment matches `s`; caller must still vkCmdBindDescriptorSets.
static inline int rdna4dc_cb_lru_find(const rdna4dc_cbcache *c, int idx, const rdna4dc_sig *s, uint64_t *set) {
    if (s->over || s->n < 2u || s->n > RDNA4DC_LRU_MAXW || (unsigned)idx >= 2u) return 0;
    const uint8_t n = c->lruN[idx];
    const uint32_t sn = s->n;
    const uint64_t sh = s->hash;
    const rdna4dc_lru_ent *arr = c->lru[idx];
    for (uint8_t i = 0; i < n; i++) {
        const rdna4dc_lru_ent *ent = &arr[i];
        if (ent->hash == sh && ent->n == sn && ent->w[0] == s->w[0] && ent->w[1] == s->w[1] &&
            (sn == 2u || rdna4dc_eq_words(ent->w + 2, s->w + 2, sn - 2u))) {
            *set = ent->set; return 1;
        }
    }
    return 0;
}
static inline void rdna4dc_cb_lru_put(rdna4dc_cbcache *c, int idx, const rdna4dc_sig *s, uint64_t set) {
    if (s->over || s->n < 2u || s->n > RDNA4DC_LRU_MAXW || !set || (unsigned)idx >= 2u) return;
    uint8_t slot;
    if (c->lruN[idx] < RDNA4DC_LRU_CAP) slot = c->lruN[idx]++;
    else { slot = c->lruNext[idx]; c->lruNext[idx] = (uint8_t)((slot + 1u) & (RDNA4DC_LRU_CAP - 1u)); }
    rdna4dc_lru_ent *ent = &c->lru[idx][slot];
    ent->n = s->n; ent->hash = s->hash; ent->set = set; rdna4dc_copy_words(ent->w, s->w, s->n);
}

typedef struct {
    uint64_t layout;                 // pipeline layout the cached sets were bound with
    rdna4dc_stage st[2];             // 0 = vertex set (index 0), 1 = fragment set (index 1)
    uint64_t lastPipe;               // last VkPipeline bound in the current render pass (0 = none)
    int pcValid;                     // fallback single-encoder pc[] valid count when no CB cache is attached
    uint32_t pcNext;
    rdna4dc_pce pc[RDNA4DC_PC_CAP];
    uint64_t draws, setsReused, setsAllocated, pipesSkipped, pipesBound, pcHits;
} rdna4dc_enc;
static inline void rdna4dc_enc_init(rdna4dc_enc *e) {
    e->layout = 0; e->lastPipe = 0; e->pcValid = 0; e->pcNext = 0;
    rdna4dc_stage_reset(&e->st[0]); rdna4dc_stage_reset(&e->st[1]);
    e->draws = e->setsReused = e->setsAllocated = e->pipesSkipped = e->pipesBound = e->pcHits = 0;
}
// New render pass / barrier inside the same segment: forget what is currently bound.
static inline void rdna4dc_enc_reset_bound(rdna4dc_enc *e) { e->lastPipe = 0; e->layout = 0; rdna4dc_stage_reset(&e->st[0]); rdna4dc_stage_reset(&e->st[1]); }
// A draw binds a pipeline with this layout: a different layout forgets the currently bound sets.
static inline void rdna4dc_enc_layout(rdna4dc_enc *e, uint64_t layout) { if (e->layout != layout) { e->layout = layout; rdna4dc_stage_reset(&e->st[0]); rdna4dc_stage_reset(&e->st[1]); } }
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
