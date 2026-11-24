// Pure-C staged/committed snapshot state machine, diffing, accounting, and
// ref-tracking helpers for Metal 3.2 MTLResidencySet.
// Shared by rdna4_residency_set_impl.inc and host C unit tests (test-io-residency.c).
#ifndef RDNA4_RESIDENCY_SET_H
#define RDNA4_RESIDENCY_SET_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define RDNA4RS_MAX_INITIAL_CAPACITY (1u << 20)

typedef struct {
    uintptr_t key;
    uint64_t size;
} rdna4rs_entry_t;

typedef struct {
    uintptr_t *added;
    size_t added_count;
    uintptr_t *removed;
    size_t removed_count;
} rdna4rs_diff_t;

// Immutable refcounted snapshot for host simulation & lifetime verification.
typedef struct {
    rdna4rs_entry_t *entries;
    size_t count;
    uint64_t total_size;
    uint64_t generation;
    uint32_t refs;
} rdna4rs_snapshot_t;

typedef struct {
    rdna4rs_entry_t *committed;
    size_t committed_count;
    size_t committed_cap;
    uint64_t committed_size;
    uint64_t generation;

    rdna4rs_entry_t *pending_adds;
    size_t pending_add_count;
    size_t pending_add_cap;

    uintptr_t *pending_removes;
    size_t pending_remove_count;
    size_t pending_remove_cap;

    int pending_clear_all;
    int resident_requested;
} rdna4rs_set_t;

static inline int rdna4rs_validate_capacity(uint64_t initial_capacity) {
    return initial_capacity <= RDNA4RS_MAX_INITIAL_CAPACITY;
}

static inline int rdna4rs_init(rdna4rs_set_t *s, uint64_t initial_capacity) {
    if (!s || !rdna4rs_validate_capacity(initial_capacity)) return 0;
    memset(s, 0, sizeof *s);
    if (initial_capacity > 0) {
        size_t cap = (size_t)initial_capacity;
        s->committed = (rdna4rs_entry_t *)malloc(cap * sizeof(*s->committed));
        if (!s->committed) return 0;
        s->committed_cap = cap;
    }
    return 1;
}

static inline void rdna4rs_destroy(rdna4rs_set_t *s) {
    if (!s) return;
    free(s->committed);
    free(s->pending_adds);
    free(s->pending_removes);
    memset(s, 0, sizeof *s);
}

static inline void rdna4rs_diff_destroy(rdna4rs_diff_t *d) {
    if (!d) return;
    free(d->added);
    free(d->removed);
    memset(d, 0, sizeof *d);
}

static inline ptrdiff_t rdna4rs_find_entry(const rdna4rs_entry_t *arr, size_t n, uintptr_t key) {
    for (size_t i = 0; i < n; i++)
        if (arr[i].key == key) return (ptrdiff_t)i;
    return -1;
}

static inline ptrdiff_t rdna4rs_find_key(const uintptr_t *arr, size_t n, uintptr_t key) {
    for (size_t i = 0; i < n; i++)
        if (arr[i] == key) return (ptrdiff_t)i;
    return -1;
}

// Queries always reflect the last committed state (never uncommitted staged mutations).
static inline int rdna4rs_contains(const rdna4rs_set_t *s, uintptr_t key) {
    if (!s || !key) return 0;
    return rdna4rs_find_entry(s->committed, s->committed_count, key) >= 0;
}

static inline size_t rdna4rs_count(const rdna4rs_set_t *s) {
    return s ? s->committed_count : 0;
}

static inline uint64_t rdna4rs_allocated_size(const rdna4rs_set_t *s) {
    return s ? s->committed_size : 0;
}

static inline uint64_t rdna4rs_generation(const rdna4rs_set_t *s) {
    return s ? s->generation : 0;
}

static inline int rdna4rs_add(rdna4rs_set_t *s, uintptr_t key, uint64_t size) {
    if (!s || !key) return 0;
    // Cancel any staged removal of the same key.
    ptrdiff_t ridx = rdna4rs_find_key(s->pending_removes, s->pending_remove_count, key);
    if (ridx >= 0) {
        size_t idx = (size_t)ridx;
        s->pending_removes[idx] = s->pending_removes[--s->pending_remove_count];
    }
    ptrdiff_t aidx = rdna4rs_find_entry(s->pending_adds, s->pending_add_count, key);
    if (aidx >= 0) {
        s->pending_adds[aidx].size = size;
        return 1;
    }
    if (s->pending_add_count == s->pending_add_cap) {
        size_t ncap = s->pending_add_cap ? s->pending_add_cap * 2u : 16u;
        if (ncap < s->pending_add_cap || ncap > SIZE_MAX / sizeof(*s->pending_adds)) return 0;
        rdna4rs_entry_t *na = (rdna4rs_entry_t *)realloc(s->pending_adds, ncap * sizeof(*na));
        if (!na) return 0;
        s->pending_adds = na;
        s->pending_add_cap = ncap;
    }
    s->pending_adds[s->pending_add_count++] = (rdna4rs_entry_t){ .key = key, .size = size };
    return 1;
}

// Stages removal of key. Returns 1 on success, 0 on invalid arg or OOM.
// Sets *removed_pending_add = 1 if key was in pending_adds (caller may release staged ref).
static inline int rdna4rs_remove(rdna4rs_set_t *s, uintptr_t key, int *removed_pending_add) {
    if (removed_pending_add) *removed_pending_add = 0;
    if (!s || !key) return 0;
    ptrdiff_t aidx = rdna4rs_find_entry(s->pending_adds, s->pending_add_count, key);
    if (aidx >= 0) {
        size_t idx = (size_t)aidx;
        size_t tail = s->pending_add_count - idx - 1u;
        if (tail) memmove(&s->pending_adds[idx], &s->pending_adds[idx + 1u],
                          tail * sizeof(*s->pending_adds));
        s->pending_add_count--;
        if (removed_pending_add) *removed_pending_add = 1;
    }
    if (s->pending_clear_all) return 1;
    if (rdna4rs_find_key(s->pending_removes, s->pending_remove_count, key) >= 0) return 1;
    if (s->pending_remove_count == s->pending_remove_cap) {
        size_t ncap = s->pending_remove_cap ? s->pending_remove_cap * 2u : 16u;
        if (ncap < s->pending_remove_cap || ncap > SIZE_MAX / sizeof(*s->pending_removes)) return 0;
        uintptr_t *nr = (uintptr_t *)realloc(s->pending_removes, ncap * sizeof(*nr));
        if (!nr) return 0;
        s->pending_removes = nr;
        s->pending_remove_cap = ncap;
    }
    s->pending_removes[s->pending_remove_count++] = key;
    return 1;
}

static inline void rdna4rs_remove_all(rdna4rs_set_t *s) {
    if (!s) return;
    s->pending_clear_all = 1;
    s->pending_add_count = 0;
    s->pending_remove_count = 0;
}

static inline void rdna4rs_request_residency(rdna4rs_set_t *s) {
    if (s) s->resident_requested = 1;
}

static inline void rdna4rs_end_residency(rdna4rs_set_t *s) {
    if (s) s->resident_requested = 0;
}

static inline uint64_t rdna4rs_sat_add(uint64_t a, uint64_t b) {
    return b > UINT64_MAX - a ? UINT64_MAX : a + b;
}

// Atomically commits staged additions/removals into committed state and outputs the
// exact added/removed delta against the previous committed set (diff may be NULL).
// On OOM returns 0 leaving both committed and staged state untouched.
static inline int rdna4rs_commit(rdna4rs_set_t *s, rdna4rs_diff_t *diff_out) {
    if (diff_out) memset(diff_out, 0, sizeof *diff_out);
    if (!s) return 0;

    size_t base_n = s->pending_clear_all ? 0u : s->committed_count;
    size_t max_next = 0;
    if (__builtin_add_overflow(base_n, s->pending_add_count, &max_next)) return 0;
    rdna4rs_entry_t *next = NULL;
    if (max_next > 0) {
        if (max_next > SIZE_MAX / sizeof(*next)) return 0;
        next = (rdna4rs_entry_t *)malloc(max_next * sizeof(*next));
        if (!next) return 0;
    }
    uintptr_t *added = NULL;
    uintptr_t *removed = NULL;
    if (diff_out && s->pending_add_count > 0) {
        added = (uintptr_t *)malloc(s->pending_add_count * sizeof(*added));
        if (!added) { free(next); return 0; }
    }
    if (diff_out && s->committed_count > 0) {
        removed = (uintptr_t *)malloc(s->committed_count * sizeof(*removed));
        if (!removed) { free(next); free(added); return 0; }
    }

    size_t next_n = 0, add_n = 0, rem_n = 0;
    uint64_t total_size = 0;

    // 1. Keep surviving committed entries in deterministic order; record removed ones.
    for (size_t i = 0; i < s->committed_count; i++) {
        uintptr_t k = s->committed[i].key;
        int drop = s->pending_clear_all
            ? (rdna4rs_find_entry(s->pending_adds, s->pending_add_count, k) < 0)
            : (rdna4rs_find_key(s->pending_removes, s->pending_remove_count, k) >= 0);
        if (drop) {
            if (removed) removed[rem_n++] = k;
            continue;
        }
        // If re-added with updated size, refresh size while keeping existing membership.
        uint64_t sz = s->committed[i].size;
        ptrdiff_t aidx = rdna4rs_find_entry(s->pending_adds, s->pending_add_count, k);
        if (aidx >= 0) sz = s->pending_adds[aidx].size;
        next[next_n++] = (rdna4rs_entry_t){ .key = k, .size = sz };
        total_size = rdna4rs_sat_add(total_size, sz);
    }

    // 2. Append genuinely new entries from pending_adds.
    for (size_t i = 0; i < s->pending_add_count; i++) {
        uintptr_t k = s->pending_adds[i].key;
        if (rdna4rs_find_entry(next, next_n, k) >= 0) continue;
        next[next_n++] = s->pending_adds[i];
        total_size = rdna4rs_sat_add(total_size, s->pending_adds[i].size);
        if (added) added[add_n++] = k;
    }

    free(s->committed);
    s->committed = next;
    s->committed_count = next_n;
    s->committed_cap = max_next;
    s->committed_size = total_size;
    s->generation++;

    s->pending_add_count = 0;
    s->pending_remove_count = 0;
    s->pending_clear_all = 0;

    if (diff_out) {
        if (!add_n) { free(added); added = NULL; }
        if (!rem_n) { free(removed); removed = NULL; }
        diff_out->added = added;
        diff_out->added_count = add_n;
        diff_out->removed = removed;
        diff_out->removed_count = rem_n;
    }
    return 1;
}

// Pure-C immutable snapshot constructor & refcounting (used by host lifetime tests).
static inline rdna4rs_snapshot_t *rdna4rs_snapshot_create(const rdna4rs_set_t *s) {
    if (!s) return NULL;
    rdna4rs_snapshot_t *snap = (rdna4rs_snapshot_t *)calloc(1, sizeof *snap);
    if (!snap) return NULL;
    if (s->committed_count > 0) {
        snap->entries = (rdna4rs_entry_t *)malloc(s->committed_count * sizeof(*snap->entries));
        if (!snap->entries) { free(snap); return NULL; }
        memcpy(snap->entries, s->committed, s->committed_count * sizeof(*snap->entries));
    }
    snap->count = s->committed_count;
    snap->total_size = s->committed_size;
    snap->generation = s->generation;
    snap->refs = 1u;
    return snap;
}

static inline rdna4rs_snapshot_t *rdna4rs_snapshot_retain(rdna4rs_snapshot_t *snap) {
    if (snap) snap->refs++;
    return snap;
}

static inline int rdna4rs_snapshot_release(rdna4rs_snapshot_t *snap,
                                           void (*on_release)(uintptr_t key, void *ctx),
                                           void *ctx) {
    if (!snap || !snap->refs) return 0;
    if (--snap->refs > 0) return 0;
    if (on_release) {
        for (size_t i = 0; i < snap->count; i++) on_release(snap->entries[i].key, ctx);
    }
    free(snap->entries);
    free(snap);
    return 1;
}

#endif // RDNA4_RESIDENCY_SET_H
