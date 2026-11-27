// garnet_heap_backing.h: physical backing blocks for RDNA4Heap (GarnetDevice.m).
// RDNA4HeapAlloc remains the logical offset/usedSize ledger. This header tracks the
// Vulkan allocations that actually support those logical ranges. It is pure C so
// the capacity, type, retirement and rollback rules can be tested without Vulkan.
#ifndef RDNA4_HEAP_BACKING_H
#define RDNA4_HEAP_BACKING_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define RDNA4HB_MAX_NORMAL (64ULL << 20)
#define RDNA4HB_TARGET_BLOCK (4ULL << 20)

enum {
    RDNA4HB_R_OK = 1,
    RDNA4HB_R_NEED_BLOCK,
    RDNA4HB_R_RETRY,
    RDNA4HB_R_NO_SPACE,
    RDNA4HB_R_INVALID,
};

typedef struct {
    uint64_t off, size, lease;
} rdna4hb_rng;

typedef struct {
    uint64_t id, size, handles;
    uint32_t memory_type;
    unsigned host_mapped : 1;
    unsigned dedicated : 1;
    unsigned exclusive : 1;
    unsigned owned_host : 1;
    unsigned placement : 1;
    void *mem, *map;
    rdna4hb_rng *rng;
    size_t nrng, crng;
} rdna4hb_block;

typedef struct {
    uint64_t id, block, off, size, charge, stamp;
    void *owner;   // dedicated leases: retained Objective-C handle owner, released by the heap
    unsigned pending : 1;
    unsigned dedicated : 1;
    unsigned retired : 1;
    unsigned finalizing : 1;
    unsigned placement : 1;
} rdna4hb_lease;

typedef struct {
    uint64_t size, backing, ctr, block_ctr, max_normal;
    rdna4hb_block *blk;
    size_t nblk, cblk;
    rdna4hb_lease *lease;
    size_t nlease, clease;
} rdna4hb_heap;

typedef struct {
    uint64_t block, lease, offset, block_size, range_size;
    unsigned dedicated : 1;
    unsigned exclusive : 1;
} rdna4hb_res;

typedef struct {
    uint64_t id, size;
    void *mem, *map;
    unsigned host_mapped : 1;
    unsigned owned_host : 1;
} rdna4hb_reclaim;

static inline uint64_t rdna4hb_alup(uint64_t v, uint64_t a) {
    if (!a) return UINT64_MAX;
    if (!(a & (a - 1))) return v > UINT64_MAX - (a - 1) ? UINT64_MAX : (v + a - 1) & ~(a - 1);
    uint64_t r = v % a;
    return r ? (v > UINT64_MAX - (a - r) ? UINT64_MAX : v + a - r) : v;
}

static inline void rdna4hb_init(rdna4hb_heap *h, uint64_t size, uint64_t max_normal) {
    memset(h, 0, sizeof *h);
    h->size = size;
    h->max_normal = max_normal ? max_normal : RDNA4HB_MAX_NORMAL;
}

static inline void rdna4hb_destroy(rdna4hb_heap *h) {
    for (size_t i = 0; i < h->nblk; i++) free(h->blk[i].rng);
    free(h->blk);
    free(h->lease);
    h->blk = NULL;
    h->lease = NULL;
    h->nblk = h->cblk = h->nlease = h->clease = 0;
    h->backing = 0;
}

static inline rdna4hb_block *rdna4hb_find_block(rdna4hb_heap *h, uint64_t id) {
    for (size_t i = 0; i < h->nblk; i++) if (h->blk[i].id == id) return &h->blk[i];
    return NULL;
}

static inline const rdna4hb_block *rdna4hb_find_block_const(const rdna4hb_heap *h, uint64_t id) {
    for (size_t i = 0; i < h->nblk; i++) if (h->blk[i].id == id) return &h->blk[i];
    return NULL;
}

static inline rdna4hb_lease *rdna4hb_find_lease(rdna4hb_heap *h, uint64_t id) {
    for (size_t i = 0; i < h->nlease; i++) if (h->lease[i].id == id) return &h->lease[i];
    return NULL;
}

static inline const rdna4hb_lease *rdna4hb_find_lease_const(const rdna4hb_heap *h, uint64_t id) {
    for (size_t i = 0; i < h->nlease; i++) if (h->lease[i].id == id) return &h->lease[i];
    return NULL;
}

static inline int rdna4hb_block_has_lease(const rdna4hb_heap *h, uint64_t block_id) {
    for (size_t i = 0; i < h->nlease; i++) if (h->lease[i].block == block_id) return 1;
    return 0;
}

static inline int rdna4hb_has_reclaimable(const rdna4hb_heap *h) {
    for (size_t i = 0; i < h->nblk; i++)
        if (!h->blk[i].placement && !h->blk[i].nrng && !h->blk[i].handles && !rdna4hb_block_has_lease(h, h->blk[i].id)) return 1;
    return 0;
}

// Returns 1 if block `b` has no active ranges (`nrng == 0`), is not a placement
// block, and every remaining handle on `b` (if any) belongs to a retired lease
// whose handle owner can be destroyed to reclaim the block under space pressure.
static inline int rdna4hb_block_evictable_retired(const rdna4hb_heap *h, const rdna4hb_block *b) {
    if (!h || !b || b->placement || b->nrng) return 0;
    if (!b->handles) return !rdna4hb_block_has_lease(h, b->id);
    uint64_t retired_owners = 0;
    for (size_t i = 0; i < h->nlease; i++) {
        const rdna4hb_lease *l = &h->lease[i];
        if (l->block != b->id) continue;
        if (!l->retired || !l->owner) return 0;
        retired_owners++;
    }
    return retired_owners == b->handles;
}

static inline int rdna4hb_has_evictable_retired(const rdna4hb_heap *h) {
    if (!h) return 0;
    for (size_t i = 0; i < h->nblk; i++)
        if (h->blk[i].handles && rdna4hb_block_evictable_retired(h, &h->blk[i])) return 1;
    return 0;
}

// Adds a Vulkan allocation. The caller owns the Vulkan calls; nothing is freed here.
static inline int rdna4hb_add_block(rdna4hb_heap *h, uint64_t size, uint32_t memory_type,
                                    int host_mapped, int dedicated, int exclusive,
                                    int owned_host, void *mem, void *map, uint64_t *id) {
    if (!h || !size || !mem || memory_type >= 32 || h->block_ctr == UINT64_MAX) return 0;
    if (h->backing > h->size || size > h->size - h->backing) return 0;
    if (h->nblk == h->cblk) {
        size_t cap = h->cblk ? h->cblk * 2 : 16;
        if (cap < h->cblk || cap > SIZE_MAX / sizeof *h->blk) return 0;
        rdna4hb_block *blocks = (rdna4hb_block *)realloc(h->blk, cap * sizeof *h->blk);
        if (!blocks) return 0;
        h->blk = blocks;
        h->cblk = cap;
    }
    uint64_t bid = ++h->block_ctr;
    h->blk[h->nblk++] = (rdna4hb_block){
        .id = bid, .size = size, .memory_type = memory_type,
        .host_mapped = host_mapped != 0, .dedicated = dedicated != 0,
        .exclusive = exclusive != 0, .owned_host = owned_host != 0,
        .mem = mem, .map = map,
    };
    h->backing += size;
    if (id) *id = bid;
    return 1;
}

static inline int rdna4hb_handle_open(rdna4hb_heap *h, uint64_t block_id) {
    rdna4hb_block *b = rdna4hb_find_block(h, block_id);
    if (!b || b->handles == UINT64_MAX) return 0;
    b->handles++;
    return 1;
}

static inline int rdna4hb_handle_close(rdna4hb_heap *h, uint64_t block_id) {
    rdna4hb_block *b = rdna4hb_find_block(h, block_id);
    if (!b || !b->handles) return 0;
    b->handles--;
    return 1;
}

static inline int rdna4hb_range_insert(rdna4hb_block *b, uint64_t off, uint64_t size, uint64_t lease) {
    if (b->nrng == b->crng) {
        size_t cap = b->crng ? b->crng * 2 : 8;
        if (cap < b->crng || cap > SIZE_MAX / sizeof *b->rng) return 0;
        rdna4hb_rng *ranges = (rdna4hb_rng *)realloc(b->rng, cap * sizeof *b->rng);
        if (!ranges) return 0;
        b->rng = ranges;
        b->crng = cap;
    }
    size_t i = 0;
    while (i < b->nrng && b->rng[i].off < off) i++;
    memmove(&b->rng[i + 1], &b->rng[i], (b->nrng - i) * sizeof *b->rng);
    b->rng[i] = (rdna4hb_rng){ off, size, lease };
    b->nrng++;
    return 1;
}

static inline int rdna4hb_range_remove(rdna4hb_block *b, uint64_t lease) {
    for (size_t i = 0; i < b->nrng; i++) if (b->rng[i].lease == lease) {
        b->rng[i] = b->rng[b->nrng - 1];
        b->nrng--;
        for (size_t j = 1; j < b->nrng; j++) {
            rdna4hb_rng tmp = b->rng[j];
            size_t k = j;
            while (k && b->rng[k - 1].off > tmp.off) { b->rng[k] = b->rng[k - 1]; k--; }
            b->rng[k] = tmp;
        }
        return 1;
    }
    return 0;
}

static inline int rdna4hb_candidate(const rdna4hb_block *b, uint64_t size, uint64_t align, uint64_t *off) {
    uint64_t prev = 0;
    for (size_t i = 0; i <= b->nrng; i++) {
        uint64_t end = i < b->nrng ? b->rng[i].off : b->size;
        uint64_t at = rdna4hb_alup(prev, align);
        if (at != UINT64_MAX && at <= end && end - at >= size) { *off = at; return 1; }
        if (i < b->nrng) {
            uint64_t next = b->rng[i].off + b->rng[i].size;
            if (next < prev || next > b->size) return 0;
            prev = next;
        }
    }
    return 0;
}

// Largest single range a non-dedicated request could receive without creating a
// Vulkan allocation. Pending ranges remain occupied until the caller reaps them.
// Unbacked heap capacity can still satisfy a new block, so the answer is the
// larger of an existing compatible block's best gap and the unbacked remainder.
static inline uint64_t rdna4hb_max_available(const rdna4hb_heap *h, uint64_t alignment,
                                             uint64_t granularity, int host_mapped) {
    if (!h || !alignment) return 0;
    uint64_t gran = granularity ? granularity : 1;
    uint64_t align = alignment > gran ? alignment : gran;
    uint64_t best = 0;
    for (size_t i = 0; i < h->nblk; i++) {
        const rdna4hb_block *b = &h->blk[i];
        if (b->placement || b->dedicated || b->host_mapped != (host_mapped != 0)) continue;
        if (b->exclusive && b->nrng) continue;
        uint64_t prev = 0;
        for (size_t r = 0; r <= b->nrng; r++) {
            uint64_t end = r < b->nrng ? b->rng[r].off : b->size;
            uint64_t at = rdna4hb_alup(prev, align);
            if (at != UINT64_MAX && at <= end) {
                uint64_t avail = end - at;
                avail -= avail % gran;
                if (avail > best) best = avail;
            }
            if (r < b->nrng) {
                uint64_t next = b->rng[r].off + b->rng[r].size;
                if (next < prev || next > b->size) break;
                prev = next;
            }
        }
    }
    uint64_t unbacked = h->backing < h->size ? h->size - h->backing : 0;
    uint64_t reclaimable = 0;
    for (size_t i = 0; i < h->nblk; i++) {
        if (rdna4hb_block_evictable_retired(h, &h->blk[i])) {
            if (h->blk[i].size > h->size - reclaimable) reclaimable = h->size;
            else reclaimable += h->blk[i].size;
        }
    }
    uint64_t pool = reclaimable > h->size - unbacked ? h->size : unbacked + reclaimable;
    pool -= pool % gran;
    return pool > best ? pool : best;
}

static inline int rdna4hb_lease_add(rdna4hb_heap *h, rdna4hb_block *b, uint64_t off, uint64_t size,
                                    int dedicated, void *owner, uint64_t *lease_id) {
    if (h->ctr == UINT64_MAX) return 0;
    if (h->nlease == h->clease) {
        size_t cap = h->clease ? h->clease * 2 : 16;
        if (cap < h->clease || cap > SIZE_MAX / sizeof *h->lease) return 0;
        rdna4hb_lease *leases = (rdna4hb_lease *)realloc(h->lease, cap * sizeof *h->lease);
        if (!leases) return 0;
        h->lease = leases;
        h->clease = cap;
    }
    uint64_t id = ++h->ctr;
    if (!rdna4hb_range_insert(b, off, size, id)) return 0;
    h->lease[h->nlease++] = (rdna4hb_lease){
        .id = id, .block = b->id, .off = off, .size = size,
        .owner = owner, .dedicated = dedicated != 0,
    };
    if (lease_id) *lease_id = id;
    return 1;
}

// Reserves a range in an existing block, or describes the block the caller must create.
// memory_type_bits is the resource's VkMemoryRequirements.memoryTypeBits.
static inline int rdna4hb_alloc(rdna4hb_heap *h, uint64_t size, uint64_t alignment, uint64_t granularity,
                                uint32_t memory_type_bits, int host_mapped, int host_import, int dedicated,
                                void *owner, rdna4hb_res *out) {
    if (!out) return RDNA4HB_R_INVALID;
    memset(out, 0, sizeof *out);
    if (!h || !size || !alignment || !memory_type_bits || !h->size) return RDNA4HB_R_INVALID;
    uint64_t gran = granularity ? granularity : 1;
    uint64_t need = rdna4hb_alup(size, gran);
    uint64_t align = alignment > gran ? alignment : gran;
    if (need == UINT64_MAX || need < size || !align) return RDNA4HB_R_INVALID;

    for (size_t i = 0; i < h->nblk; i++) {
        rdna4hb_block *b = &h->blk[i];
        if (b->placement) continue;
        if (dedicated) {
            if (!b->dedicated || b->nrng) continue;
        } else if (b->dedicated) continue;
        if (b->exclusive && !dedicated && b->nrng) continue;
        if (host_import ? !(b->host_mapped && b->owned_host)
                        : (b->host_mapped != (host_mapped != 0) || (host_mapped && b->owned_host))) continue;
        if (!(memory_type_bits & (1u << b->memory_type))) continue;
        if (!b->nrng && b->handles && b->size > need && b->size < RDNA4HB_TARGET_BLOCK &&
            (b->size - need) > (b->size >> 2) && rdna4hb_block_evictable_retired(h, b))
            continue;
        uint64_t off;
        if (need > b->size || !rdna4hb_candidate(b, need, align, &off)) continue;
        if (!rdna4hb_lease_add(h, b, off, need, b->dedicated, owner, &out->lease)) continue;
        out->block = b->id;
        out->offset = off;
        out->block_size = b->size;
        out->range_size = need;
        out->dedicated = b->dedicated;
        out->exclusive = b->exclusive;
        return RDNA4HB_R_OK;
    }

    if (dedicated || need > h->max_normal) {
        out->block_size = need;
        out->dedicated = dedicated != 0;
        out->exclusive = !dedicated;
    } else {
        uint64_t target = need > RDNA4HB_TARGET_BLOCK ? need : RDNA4HB_TARGET_BLOCK;
        if (target > h->max_normal) target = h->max_normal;
        if (target < need) target = need;
        out->block_size = target;
    }
    uint64_t remaining = h->backing < h->size ? h->size - h->backing : 0;
    if (remaining < need)
        return (rdna4hb_has_reclaimable(h) || rdna4hb_has_evictable_retired(h))
            ? RDNA4HB_R_RETRY : RDNA4HB_R_NO_SPACE;
    // A speculative slab may consume at most half the unbacked remainder;
    // otherwise allocate the exact extent so tight heaps never strand a
    // sub-slab tail or starve larger allocations / other memory classes.
    if (out->block_size > need &&
        (out->block_size > remaining || remaining - out->block_size < out->block_size))
        out->block_size = need;
    out->range_size = need;
    return RDNA4HB_R_NEED_BLOCK;
}

// Marks a lease's physical range for reuse after the GPU serial `stamp` completes.
// The logical heap accounting is deliberately independent and is released by RDNA4HeapAlloc.
static inline int rdna4hb_release(rdna4hb_heap *h, uint64_t lease_id, uint64_t stamp) {
    rdna4hb_lease *l = rdna4hb_find_lease(h, lease_id);
    if (!l || l->retired) return 0;
    l->pending = 1;
    if (!l->stamp || stamp < l->stamp) l->stamp = stamp;
    return 1;
}

// Progresses retirement to `completed`. Dedicated leases remain until their owner has
// destroyed the VkBuffer/VkImage that was dedicated to the block.
static inline void rdna4hb_reap(rdna4hb_heap *h, uint64_t completed) {
    if (!h) return;
    for (size_t i = 0; i < h->nlease;) {
        rdna4hb_lease *l = &h->lease[i];
        if (!l->pending || l->stamp > completed || l->retired) { i++; continue; }
        rdna4hb_block *b = rdna4hb_find_block(h, l->block);
        if (!b) {
            l->retired = 1;
            i++;
            continue;
        }
        rdna4hb_range_remove(b, l->id);
        l->retired = 1;
        if (!l->dedicated && !l->owner) {
            h->lease[i] = h->lease[--h->nlease];
            continue;
        }
        i++;
    }
}

// Finds one retired non-dedicated lease on an empty block whose handles can all be
// finalized so the block can be reclaimed under space pressure.
static inline int rdna4hb_evict_candidate(rdna4hb_heap *h, uint64_t *lease_id,
                                          uint64_t *block_id, void **owner) {
    if (!h) return 0;
    for (size_t i = 0; i < h->nlease; i++) {
        rdna4hb_lease *l = &h->lease[i];
        if (l->dedicated || !l->retired || l->finalizing || !l->owner) continue;
        const rdna4hb_block *b = rdna4hb_find_block_const(h, l->block);
        if (!b || !rdna4hb_block_evictable_retired(h, b)) continue;
        l->finalizing = 1;
        if (lease_id) *lease_id = l->id;
        if (block_id) *block_id = l->block;
        if (owner) *owner = l->owner;
        return 1;
    }
    return 0;
}

// Removes any retired lease (dedicated or non-dedicated) whose owner handle has been closed.
static inline void rdna4hb_finalized_retired(rdna4hb_heap *h, uint64_t lease_id) {
    rdna4hb_lease *l = rdna4hb_find_lease(h, lease_id);
    if (!l || !l->retired) return;
    size_t i = (size_t)(l - h->lease);
    h->lease[i] = h->lease[--h->nlease];
}

// A dedicated block can be destroyed while its Metal resource is still alive after
// -makeAliasable; the owner destroys its handle first. Returns 1 when a lease is ready.
static inline int rdna4hb_finalize_candidate(rdna4hb_heap *h, uint64_t *lease_id, uint64_t *block_id,
                                             void **owner) {
    if (!h) return 0;
    for (size_t i = 0; i < h->nlease; i++) {
        rdna4hb_lease *l = &h->lease[i];
        if (l->dedicated && l->retired && !l->finalizing && l->owner) {
            l->finalizing = 1;
            if (lease_id) *lease_id = l->id;
            if (block_id) *block_id = l->block;
            if (owner) *owner = l->owner;
            return 1;
        }
    }
    return 0;
}

static inline int rdna4hb_lease_owner(rdna4hb_heap *h, uint64_t lease_id, void *owner) {
    rdna4hb_lease *l = rdna4hb_find_lease(h, lease_id);
    if (!l) return 0;
    l->owner = owner;
    return 1;
}

// A dedicated resource can be made aliasable while its Metal wrapper is still alive.
// The lease therefore holds a retained opaque handle owner, not the wrapper itself.
// Clearing it is safe only after that owner has destroyed the handles and closed the
// lease's block handle, or after ownership has been transferred elsewhere.
static inline int rdna4hb_lease_owner_clear(rdna4hb_heap *h, uint64_t lease_id, void *owner) {
    rdna4hb_lease *l = rdna4hb_find_lease(h, lease_id);
    if (!l || l->owner != owner) return 0;
    l->owner = NULL;
    return 1;
}

// Removes a fully finalized dedicated lease. The caller must already have destroyed
// the resource handle and closed the block's handle reference.
static inline void rdna4hb_finalized(rdna4hb_heap *h, uint64_t lease_id) {
    rdna4hb_lease *l = rdna4hb_find_lease(h, lease_id);
    if (!l || !l->dedicated || !l->retired) return;
    rdna4hb_block *b = rdna4hb_find_block(h, l->block);
    if (b && !b->handles && !b->nrng) {
        // The caller may reclaim it in the same pass.
    }
    size_t i = (size_t)(l - h->lease);
    h->lease[i] = h->lease[--h->nlease];
}

// Removes the first empty block whose resource handles and leases are all gone.
// The caller must vkFreeMemory(out.mem) before unmapping an owned host range.
static inline int rdna4hb_take_reclaimable(rdna4hb_heap *h, rdna4hb_reclaim *out) {
    if (!h || !out) return 0;
    for (size_t i = 0; i < h->nblk; i++) {
        rdna4hb_block *b = &h->blk[i];
        if (b->placement || b->nrng || b->handles) continue;
        if (rdna4hb_block_has_lease(h, b->id)) continue;
        *out = (rdna4hb_reclaim){ b->id, b->size, b->mem, b->map, b->host_mapped, b->owned_host };
        free(b->rng);
        h->backing -= b->size;
        h->blk[i] = h->blk[--h->nblk];
        return 1;
    }
    return 0;
}

// Heap destruction: no resources retain the heap at this point, so every block can go.
static inline int rdna4hb_take_block(rdna4hb_heap *h, rdna4hb_reclaim *out) {
    if (!h || !out || !h->nblk) return 0;
    rdna4hb_block *b = &h->blk[0];
    *out = (rdna4hb_reclaim){ b->id, b->size, b->mem, b->map, b->host_mapped, b->owned_host };
    free(b->rng);
    h->backing -= b->size;
    h->blk[0] = h->blk[--h->nblk];
    return 1;
}

static inline uint64_t rdna4hb_live_bytes(const rdna4hb_heap *h) {
    uint64_t n = 0;
    for (size_t i = 0; i < h->nlease; i++) if (!h->lease[i].retired) n += h->lease[i].size;
    return n;
}

// ---- Placement heap helpers (MTLHeapTypePlacement) ----

static inline int rdna4hb_add_placement_block(rdna4hb_heap *h, uint64_t size, uint32_t memory_type,
                                              int host_mapped, int owned_host, void *mem, void *map,
                                              uint64_t *id) {
    uint64_t bid = 0;
    if (!rdna4hb_add_block(h, size, memory_type, host_mapped, 0, 0, owned_host, mem, map, &bid))
        return 0;
    rdna4hb_block *b = rdna4hb_find_block(h, bid);
    if (b) b->placement = 1;
    if (id) *id = bid;
    return 1;
}

static inline int rdna4hb_lease_add_placement(rdna4hb_heap *h, rdna4hb_block *b, uint64_t off,
                                              uint64_t extent, uint64_t charge, uint64_t *lease_id) {
    if (h->ctr == UINT64_MAX) return 0;
    if (h->nlease == h->clease) {
        size_t cap = h->clease ? h->clease * 2 : 16;
        if (cap < h->clease || cap > SIZE_MAX / sizeof *h->lease) return 0;
        rdna4hb_lease *leases = (rdna4hb_lease *)realloc(h->lease, cap * sizeof *h->lease);
        if (!leases) return 0;
        h->lease = leases;
        h->clease = cap;
    }
    uint64_t id = ++h->ctr;
    if (!rdna4hb_range_insert(b, off, extent, id)) return 0;
    h->lease[h->nlease++] = (rdna4hb_lease){
        .id = id, .block = b->id, .off = off, .size = extent, .charge = charge, .placement = 1,
    };
    if (lease_id) *lease_id = id;
    return 1;
}

// Validates explicit placement offset, alignment, granularity and bounds without mutating state.
static inline int rdna4hb_place_check(uint64_t heap_size, uint64_t offset, uint64_t size,
                                      uint64_t alignment, uint64_t granularity,
                                      int requires_dedicated, uint64_t *extent_out) {
    if (extent_out) *extent_out = 0;
    if (!heap_size || !size || !alignment || requires_dedicated) return RDNA4HB_R_INVALID;
    uint64_t gran = granularity ? granularity : 1;
    if (gran & (gran - 1)) return RDNA4HB_R_INVALID;
    if ((offset % alignment) != 0 || (offset % gran) != 0) return RDNA4HB_R_INVALID;
    uint64_t extent = rdna4hb_alup(size, gran);
    if (extent == UINT64_MAX || extent < size) return RDNA4HB_R_INVALID;
    if (offset > heap_size || extent > heap_size - offset) return RDNA4HB_R_NO_SPACE;
    if (extent_out) *extent_out = extent;
    return RDNA4HB_R_OK;
}

// Places a resource at an explicit offset in a placement heap; overlapping ranges are allowed.
static inline int rdna4hb_place(rdna4hb_heap *h, uint64_t offset, uint64_t size, uint64_t alignment,
                                uint64_t granularity, uint32_t memory_type_bits, int need_host_map,
                                int host_import, int requires_dedicated, rdna4hb_res *out) {
    if (!out) return RDNA4HB_R_INVALID;
    memset(out, 0, sizeof *out);
    if (!h || !memory_type_bits) return RDNA4HB_R_INVALID;
    uint64_t extent = 0;
    int chk = rdna4hb_place_check(h->size, offset, size, alignment, granularity, requires_dedicated, &extent);
    if (chk != RDNA4HB_R_OK) return chk;

    for (size_t i = 0; i < h->nblk; i++) {
        rdna4hb_block *b = &h->blk[i];
        if (!b->placement || b->dedicated || b->size < h->size) continue;
        if (need_host_map && !b->host_mapped) continue;
        if (host_import ? !b->owned_host : b->owned_host) continue;
        if (!(memory_type_bits & (1u << b->memory_type))) continue;
        if (!rdna4hb_lease_add_placement(h, b, offset, extent, size, &out->lease))
            return RDNA4HB_R_NO_SPACE;
        out->block = b->id;
        out->offset = offset;
        out->block_size = b->size;
        out->range_size = extent;
        return RDNA4HB_R_OK;
    }

    uint64_t remaining = h->backing < h->size ? h->size - h->backing : 0;
    if (remaining < h->size) return RDNA4HB_R_NO_SPACE;
    out->block_size = h->size;
    out->offset = offset;
    out->range_size = extent;
    return RDNA4HB_R_NEED_BLOCK;
}

// Union length of active (non-aliasable) placed ranges; bounded in [0, h->size] even under aliasing.
static inline uint64_t rdna4hb_placement_used(const rdna4hb_heap *h) {
    if (!h) return 0;
    uint64_t total = 0;
    for (size_t bi = 0; bi < h->nblk; bi++) {
        const rdna4hb_block *b = &h->blk[bi];
        if (!b->placement) continue;
        uint64_t cur_start = 0, cur_end = 0;
        int have = 0;
        for (size_t r = 0; r < b->nrng; r++) {
            const rdna4hb_lease *l = rdna4hb_find_lease_const(h, b->rng[r].lease);
            if (!l || l->pending || l->retired || !l->charge) continue;
            uint64_t s = l->off;
            uint64_t e = l->charge > h->size - s ? h->size : s + l->charge;
            if (!have) {
                cur_start = s; cur_end = e; have = 1;
            } else if (s <= cur_end) {
                if (e > cur_end) cur_end = e;
            } else {
                total += cur_end - cur_start;
                cur_start = s; cur_end = e;
            }
        }
        if (have) total += cur_end - cur_start;
    }
    return total > h->size ? h->size : total;
}

// Largest aligned contiguous gap not covered by active (non-aliasable) placement extents.
static inline uint64_t rdna4hb_placement_max_available(const rdna4hb_heap *h, uint64_t alignment,
                                                       uint64_t granularity) {
    if (!h || !h->size || !alignment || alignment > (UINT64_C(1) << 63)) return 0;
    uint64_t gran = granularity ? granularity : 1;
    if (gran & (gran - 1)) return 0;
    if (alignment & (alignment - 1)) {
        uint64_t p = 1;
        while (p < alignment) p <<= 1;
        alignment = p;
    }
    uint64_t align = alignment > gran ? alignment : gran;
    const rdna4hb_block *pb = NULL;
    for (size_t bi = 0; bi < h->nblk; bi++)
        if (h->blk[bi].placement) { pb = &h->blk[bi]; break; }
    if (!pb) {
        uint64_t avail = h->size;
        avail -= avail % gran;
        return avail;
    }
    uint64_t best = 0, prev = 0;
    for (size_t r = 0; r < pb->nrng; r++) {
        const rdna4hb_lease *l = rdna4hb_find_lease_const(h, pb->rng[r].lease);
        if (!l || l->pending || l->retired) continue;
        uint64_t s = pb->rng[r].off;
        uint64_t e = pb->rng[r].size > pb->size - s ? pb->size : s + pb->rng[r].size;
        if (s > prev) {
            uint64_t at = rdna4hb_alup(prev, align);
            if (at != UINT64_MAX && at <= s) {
                uint64_t avail = s - at;
                avail -= avail % gran;
                if (avail > best) best = avail;
            }
        }
        if (e > prev) prev = e;
    }
    if (pb->size > prev) {
        uint64_t at = rdna4hb_alup(prev, align);
        if (at != UINT64_MAX && at <= pb->size) {
            uint64_t avail = pb->size - at;
            avail -= avail % gran;
            if (avail > best) best = avail;
        }
    }
    return best;
}

#endif
