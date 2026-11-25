// garnet_heapalloc.h: the offset allocator of RDNA4Heap (GarnetDevice.m) as plain C, so it can be tested without Vulkan (test-heapalloc.c, run on the host Mac against
// numbers measured on an Apple-silicon Mac by `mtlprobe heap`). First fit by offset; a block is (offset, requested size); the next block may start at the first
// `align`-aligned offset at or after the previous block's end; usedSize is the sum of the requested sizes (not rounded).
#ifndef RDNA4_HEAPALLOC_H
#define RDNA4_HEAPALLOC_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
// size is the bytes charged to usedSize; extent is the physical footprint the
// next range must clear (bufferImageGranularity may exceed a small Vulkan size).
typedef struct { uint64_t off, size, id, extent; } RDNA4HeapBlk;
typedef struct { uint64_t size, used, ctr; RDNA4HeapBlk *blk; size_t nblk, cap; } RDNA4HeapAlloc;
static inline uint64_t rdna4ha_alup(uint64_t v, uint64_t a) {
    if (!a || (a & (a - 1)) || v > UINT64_MAX - (a - 1)) return UINT64_MAX;
    return (v + a - 1) & ~(a - 1);
}
static inline void rdna4ha_init(RDNA4HeapAlloc *h, uint64_t size) { memset(h, 0, sizeof *h); h->size = size; }
static inline void rdna4ha_destroy(RDNA4HeapAlloc *h) { free(h->blk); h->blk = NULL; h->nblk = h->cap = 0; }
// returns 1 and fills *off / *id, or 0 when no gap fits. charge is what Metal's
// usedSize reports; extent is the reserved placement span (extent >= charge).
static inline int rdna4ha_alloc_ext(RDNA4HeapAlloc *h, uint64_t charge, uint64_t extent, uint64_t al, uint64_t *off, uint64_t *id) {
    if (!charge || !extent || extent < charge || !al || (al & (al - 1)) || extent > h->size || h->ctr == UINT64_MAX) return 0;
    uint64_t prevEnd = 0, at = UINT64_MAX; size_t pos = 0;
    for (size_t i = 0; i <= h->nblk; i++) {
        uint64_t gapEnd = i < h->nblk ? h->blk[i].off : h->size, cand = rdna4ha_alup(prevEnd, al);
        if (cand <= gapEnd && gapEnd - cand >= extent) { at = cand; pos = i; break; }
        if (i < h->nblk) prevEnd = h->blk[i].off + h->blk[i].extent;
    }
    if (at == UINT64_MAX) return 0;
    if (h->nblk == h->cap) {
        if (h->cap > SIZE_MAX / 2 / sizeof *h->blk) return 0;
        size_t capacity = h->cap ? h->cap * 2 : 64;
        RDNA4HeapBlk *blocks = (RDNA4HeapBlk *)realloc(h->blk, capacity * sizeof *h->blk);
        if (!blocks) return 0;
        h->blk = blocks; h->cap = capacity;
    }
    memmove(&h->blk[pos + 1], &h->blk[pos], (h->nblk - pos) * sizeof *h->blk);
    h->blk[pos] = (RDNA4HeapBlk){ at, charge, ++h->ctr, extent }; h->nblk++; h->used += charge; *off = at; *id = h->ctr; return 1;
}
static inline int rdna4ha_alloc(RDNA4HeapAlloc *h, uint64_t sz, uint64_t al, uint64_t *off, uint64_t *id) {
    return rdna4ha_alloc_ext(h, sz, sz, al, off, id);
}
// frees by block id (a second free of the same id is a no-op)
static inline void rdna4ha_free(RDNA4HeapAlloc *h, uint64_t id) {
    for (size_t i = 0; i < h->nblk; i++) if (h->blk[i].id == id) { h->used -= h->blk[i].size; memmove(&h->blk[i], &h->blk[i + 1], (h->nblk - i - 1) * sizeof *h->blk); h->nblk--; return; }
}
static inline uint64_t rdna4ha_maxavail(const RDNA4HeapAlloc *h, uint64_t al) {
    if (al < 1) al = 1;
    if (al > (UINT64_C(1) << 63)) return 0;
    if (al & (al - 1)) { uint64_t p = 1; while (p < al) p <<= 1; al = p; }
    uint64_t best = 0, prevEnd = 0;
    for (size_t i = 0; i <= h->nblk; i++) {
        uint64_t gapEnd = i < h->nblk ? h->blk[i].off : h->size, cand = rdna4ha_alup(prevEnd, al);
        if (cand < gapEnd && gapEnd - cand > best) best = gapEnd - cand;
        if (i < h->nblk) prevEnd = h->blk[i].off + h->blk[i].extent;
    }
    return best;
}
#endif
