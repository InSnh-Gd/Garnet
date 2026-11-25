// test-heap-backing.c: physical block/range policy for RDNA4Heap.
// Host build: cc -O1 -Wall -Wextra -Werror -o /tmp/test-heap-backing test-heap-backing.c && /tmp/test-heap-backing
#include <stdio.h>
#include "garnet_heap_backing.h"

static int fails;
#define CHECK(name, cond, ...) do { \
    int ok_ = (cond); \
    printf("%s %s: ", ok_ ? "ok  " : "FAIL", name); \
    printf(__VA_ARGS__); \
    printf("\n"); \
    if (!ok_) fails++; \
} while (0)
#define KIB (1ULL << 10)
#define MIB (1ULL << 20)

static int add(rdna4hb_heap *h, uint64_t size, uint32_t type, int host, int dedicated,
               int exclusive, uintptr_t token, uint64_t *id) {
    return rdna4hb_add_block(h, size, type, host, dedicated, exclusive, 0,
                             (void *)token, host ? (void *)(token + 1) : NULL, id);
}

int main(void) {
    rdna4hb_heap h;
    rdna4hb_res r;
    rdna4hb_reclaim gone;
    uint64_t block, lease, lease2;

    // First fit, granularity, alignment, and retirement gating.
    rdna4hb_init(&h, 64 * MIB, RDNA4HB_MAX_NORMAL);
    int k = rdna4hb_alloc(&h, 1000, 256, 64, 1, 0, 0, 0, NULL, &r);
    CHECK("empty heap asks for a bounded block", k == RDNA4HB_R_NEED_BLOCK && r.block_size == 4 * MIB && h.backing == 0,
          "kind %d block %llu backing %llu", k, (unsigned long long)r.block_size, (unsigned long long)h.backing);
    CHECK("add bounded block", add(&h, r.block_size, 0, 0, 0, 0, 0x1000, &block) && h.backing == 4 * MIB,
          "block %llu backing %llu", (unsigned long long)block, (unsigned long long)h.backing);
    k = rdna4hb_alloc(&h, 1000, 256, 64, 1, 0, 0, 0, NULL, &r);
    lease = r.lease;
    CHECK("first allocation at zero", k == RDNA4HB_R_OK && r.block == block && r.offset == 0 && rdna4hb_handle_open(&h, block),
          "kind %d block %llu off %llu lease %llu", k, (unsigned long long)r.block, (unsigned long long)r.offset, (unsigned long long)lease);
    CHECK("same block first-fit", rdna4hb_alloc(&h, 1000, 256, 64, 1, 0, 0, 0, NULL, &r) == RDNA4HB_R_OK && r.block == block && r.offset == 1024,
          "off %llu", (unsigned long long)r.offset);
    lease2 = r.lease;
    CHECK("release is stamped", rdna4hb_release(&h, lease, 7), "serial 7");
    rdna4hb_reap(&h, 6);
    CHECK("in-flight range is not reused", rdna4hb_alloc(&h, 1000, 256, 64, 1, 0, 0, 0, NULL, &r) == RDNA4HB_R_OK && r.offset == 2048,
          "off %llu (want 2048)", (unsigned long long)r.offset);
    rdna4hb_release(&h, r.lease, 8);
    rdna4hb_reap(&h, 7);
    CHECK("completed serial reuses the lowest hole", rdna4hb_alloc(&h, 1000, 256, 64, 1, 0, 0, 0, NULL, &r) == RDNA4HB_R_OK && r.offset == 0,
          "off %llu (want 0)", (unsigned long long)r.offset);
    rdna4hb_release(&h, r.lease, 9);
    rdna4hb_reap(&h, 9);
    CHECK("release is idempotent", !rdna4hb_release(&h, lease, 10), "second release ignored");
    rdna4hb_release(&h, lease2, 9);
    rdna4hb_reap(&h, 9);

    // maxAvailable is a successful-allocation promise: a fully occupied block
    // contributes no range until its retirement serial clears.
    rdna4hb_destroy(&h);
    rdna4hb_init(&h, 4 * MIB, RDNA4HB_MAX_NORMAL);
    CHECK("empty heap advertises only unbacked capacity",
          rdna4hb_max_available(&h, 64 * KIB, 64 * KIB, 0) == 4 * MIB,
          "%llu", (unsigned long long)rdna4hb_max_available(&h, 64 * KIB, 64 * KIB, 0));
    add(&h, 4 * MIB, 0, 0, 0, 0, 0x1800, &block);
    CHECK("full backing block allocation",
          rdna4hb_alloc(&h, 4 * MIB, 64 * KIB, 64 * KIB, 1, 0, 0, 0, NULL, &r) == RDNA4HB_R_OK,
          "block %llu", (unsigned long long)block);
    lease = r.lease;
    rdna4hb_release(&h, lease, 7);
    rdna4hb_reap(&h, 6);
    CHECK("in-flight full-block range advertises zero", rdna4hb_max_available(&h, 256, 64 * KIB, 0) == 0,
          "%llu", (unsigned long long)rdna4hb_max_available(&h, 256, 64 * KIB, 0));
    rdna4hb_reap(&h, 7);
    CHECK("completed full-block retirement is reusable capacity",
          rdna4hb_max_available(&h, 256, 64 * KIB, 0) == 4 * MIB,
          "%llu", (unsigned long long)rdna4hb_max_available(&h, 256, 64 * KIB, 0));

    // Memory type and host-map class are part of the block identity.
    rdna4hb_destroy(&h);
    rdna4hb_init(&h, 64 * MIB, RDNA4HB_MAX_NORMAL);
    k = rdna4hb_alloc(&h, 4096, 4096, 4096, 1, 0, 0, 0, NULL, &r);
    CHECK("device-local request needs its own class", k == RDNA4HB_R_NEED_BLOCK && add(&h, r.block_size, 0, 0, 0, 0, 0x2000, &block), "type 0");
    CHECK("wrong memory-type bits miss", rdna4hb_alloc(&h, 4096, 4096, 4096, 2, 0, 0, 0, NULL, &r) == RDNA4HB_R_NEED_BLOCK,
          "needs type 1");
    CHECK("host-map request misses a private block", rdna4hb_alloc(&h, 4096, 4096, 4096, 1, 1, 0, 0, NULL, &r) == RDNA4HB_R_NEED_BLOCK,
          "needs host-visible block");
    CHECK("matching bits and class hit", rdna4hb_alloc(&h, 4096, 4096, 4096, 1, 0, 0, 0, NULL, &r) == RDNA4HB_R_OK && r.block == block,
          "block %llu", (unsigned long long)r.block);

    rdna4hb_destroy(&h);
    rdna4hb_init(&h, MIB, RDNA4HB_MAX_NORMAL);
    k = rdna4hb_alloc(&h, 1000, 256, 16, 1, 0, 0, 0, NULL, &r);
    CHECK("small heap reserves only the requested partial slab",
          k == RDNA4HB_R_NEED_BLOCK && r.block_size == 1008 &&
              add(&h, r.block_size, 0, 0, 0, 0, 0x2800, &block),
          "block size %llu", (unsigned long long)r.block_size);
    CHECK("small buffer binds before mixed memory class",
          rdna4hb_alloc(&h, 1000, 256, 16, 1, 0, 0, 0, NULL, &r) == RDNA4HB_R_OK,
          "backing %llu", (unsigned long long)h.backing);
    k = rdna4hb_alloc(&h, 1024, 256, 16, 2, 0, 0, 1, NULL, &r);
    CHECK("small heap retains space for a dedicated image",
          k == RDNA4HB_R_NEED_BLOCK && r.dedicated && r.block_size == 1024 &&
              add(&h, r.block_size, 1, 0, 1, 0, 0x2900, &block) &&
              rdna4hb_alloc(&h, 1024, 256, 16, 2, 0, 0, 1, NULL, &r) == RDNA4HB_R_OK,
          "mixed backing %llu of %llu", (unsigned long long)h.backing, (unsigned long long)h.size);

    // Dedicated requirements are exact, standalone, and survive until finalization.
    rdna4hb_destroy(&h);
    rdna4hb_init(&h, 64 * MIB, RDNA4HB_MAX_NORMAL);
    k = rdna4hb_alloc(&h, 3 * MIB, 64 * KIB, 4 * KIB, 1, 0, 0, 1, NULL, &r);
    CHECK("dedicated asks for exact standalone block", k == RDNA4HB_R_NEED_BLOCK && r.dedicated && r.block_size == 3 * MIB,
          "kind %d size %llu dedicated %u", k, (unsigned long long)r.block_size, r.dedicated);
    CHECK("add dedicated block", add(&h, r.block_size, 0, 0, 1, 0, 0x3000, &block), "block %llu", (unsigned long long)block);
    k = rdna4hb_alloc(&h, 3 * MIB, 64 * KIB, 4 * KIB, 1, 0, 0, 1, (void *)0xabc, &r);
    lease = r.lease;
    CHECK("dedicated range binds at zero", k == RDNA4HB_R_OK && r.offset == 0 && r.dedicated, "kind %d off %llu", k, (unsigned long long)r.offset);
    CHECK("owner detach rejects a different lifecycle token", !rdna4hb_lease_owner_clear(&h, lease, (void *)0xdef),
          "lease %llu keeps its owner", (unsigned long long)lease);
    CHECK("a second dedicated request cannot share it", rdna4hb_alloc(&h, 1, 1, 1, 1, 0, 0, 1, NULL, &r) == RDNA4HB_R_NEED_BLOCK,
          "new exact block required");
    CHECK("dedicated handle opens", rdna4hb_handle_open(&h, block), "block %llu", (unsigned long long)block);
    rdna4hb_release(&h, lease, 12);
    rdna4hb_reap(&h, 11);
    CHECK("dedicated waits for retirement", !rdna4hb_finalize_candidate(&h, NULL, NULL, NULL), "no candidate");
    rdna4hb_reap(&h, 12);
    void *owner = NULL;
    CHECK("dedicated becomes finalizable", rdna4hb_finalize_candidate(&h, &lease, &block, &owner) && owner == (void *)0xabc,
          "owner %p", owner);
    rdna4hb_handle_close(&h, block);
    rdna4hb_finalized(&h, lease);
    CHECK("finalized dedicated block is reclaimable", rdna4hb_take_reclaimable(&h, &gone) && gone.size == 3 * MIB && h.backing == 0,
          "size %llu backing %llu", (unsigned long long)gone.size, (unsigned long long)h.backing);

    // Handle lifetime only gates block destruction; physical ranges can be reused.
    rdna4hb_destroy(&h);
    rdna4hb_init(&h, 1 * MIB, RDNA4HB_MAX_NORMAL);
    rdna4hb_alloc(&h, 64 * KIB, 256, 256, 1, 0, 0, 0, NULL, &r);
    add(&h, r.block_size, 0, 0, 0, 0, 0x4000, &block);
    rdna4hb_alloc(&h, 64 * KIB, 256, 256, 1, 0, 0, 0, NULL, &r);
    lease = r.lease;
    rdna4hb_handle_open(&h, block);
    rdna4hb_release(&h, lease, 0);
    rdna4hb_reap(&h, 0);
    CHECK("open handle keeps an empty block alive", !rdna4hb_take_reclaimable(&h, &gone) && h.backing == r.block_size,
          "backing %llu", (unsigned long long)h.backing);
    CHECK("empty block range is reusable before handle release", rdna4hb_alloc(&h, 64 * KIB, 256, 256, 1, 0, 0, 0, NULL, &r) == RDNA4HB_R_OK && r.offset == 0,
          "off %llu", (unsigned long long)r.offset);
    rdna4hb_release(&h, r.lease, 0);
    rdna4hb_reap(&h, 0);
    rdna4hb_handle_close(&h, block);
    CHECK("closed handle releases the block", rdna4hb_take_reclaimable(&h, &gone), "block %llu", (unsigned long long)gone.id);

    // Large non-dedicated resources use one exact exclusive block and cannot overcommit.
    rdna4hb_destroy(&h);
    rdna4hb_init(&h, 128 * MIB, RDNA4HB_MAX_NORMAL);
    k = rdna4hb_alloc(&h, 65 * MIB, 4096, 4096, 1, 0, 0, 0, NULL, &r);
    CHECK(">64 MiB is exact exclusive", k == RDNA4HB_R_NEED_BLOCK && r.exclusive && !r.dedicated && r.block_size == 65 * MIB,
          "kind %d exact %llu exclusive %u", k, (unsigned long long)r.block_size, r.exclusive);
    add(&h, r.block_size, 0, 0, 0, 1, 0x5000, &block);
    CHECK("exact large allocation", rdna4hb_alloc(&h, 65 * MIB, 4096, 4096, 1, 0, 0, 0, NULL, &r) == RDNA4HB_R_OK && r.offset == 0,
          "off %llu", (unsigned long long)r.offset);
    CHECK("second large allocation cannot exceed the heap", rdna4hb_alloc(&h, 64 * MIB, 4096, 4096, 1, 0, 0, 0, NULL, &r) == RDNA4HB_R_NO_SPACE,
          "backing %llu of %llu", (unsigned long long)h.backing, (unsigned long long)h.size);
    CHECK("backing never exceeds heap size", h.backing <= h.size, "%llu <= %llu", (unsigned long long)h.backing, (unsigned long long)h.size);

    // A clean block can be evicted before a different memory type is created.
    rdna4hb_destroy(&h);
    rdna4hb_init(&h, 8 * MIB, RDNA4HB_MAX_NORMAL);
    rdna4hb_alloc(&h, 64 * KIB, 256, 256, 1, 0, 0, 0, NULL, &r);
    add(&h, r.block_size, 0, 0, 0, 0, 0x6000, &block);
    rdna4hb_alloc(&h, 64 * KIB, 256, 256, 1, 0, 0, 0, NULL, &r);
    lease = r.lease;
    rdna4hb_handle_open(&h, block);
    rdna4hb_release(&h, lease, 0);
    rdna4hb_reap(&h, 0);
    rdna4hb_handle_close(&h, block);
    CHECK("clean block is reclaimable for another type", rdna4hb_take_reclaimable(&h, &gone) && gone.id == block && h.backing == 0,
          "reclaimed %llu", (unsigned long long)gone.id);

    // Granularity rounds both size and offset (conservative for buffer/image adjacency).
    rdna4hb_destroy(&h);
    rdna4hb_init(&h, 1 * MIB, RDNA4HB_MAX_NORMAL);
    add(&h, 1 * MIB, 0, 0, 0, 0, 0x7000, &block);
    CHECK("granularity aligns the first range", rdna4hb_alloc(&h, 1, 1, 64 * KIB, 1, 0, 0, 0, NULL, &r) == RDNA4HB_R_OK && r.offset == 0,
          "off %llu", (unsigned long long)r.offset);
    lease = r.lease;
    CHECK("granularity rounds the range size", rdna4hb_alloc(&h, 1, 1, 64 * KIB, 1, 0, 0, 0, NULL, &r) == RDNA4HB_R_OK && r.offset == 64 * KIB,
          "off %llu", (unsigned long long)r.offset);
    rdna4hb_release(&h, lease, 0);
    rdna4hb_release(&h, r.lease, 0);
    rdna4hb_reap(&h, 0);

    // Invalid input does not mutate accounting.
    rdna4hb_destroy(&h);
    rdna4hb_init(&h, 1 * MIB, RDNA4HB_MAX_NORMAL);
    CHECK("invalid requests", rdna4hb_alloc(&h, 0, 1, 1, 1, 0, 0, 0, NULL, &r) == RDNA4HB_R_INVALID &&
          rdna4hb_alloc(&h, 1, 0, 1, 1, 0, 0, 0, NULL, &r) == RDNA4HB_R_INVALID &&
          rdna4hb_alloc(&h, 1, 1, 1, 0, 0, 0, 0, NULL, &r) == RDNA4HB_R_INVALID && !h.nblk && !h.nlease,
          "zero size/alignment/type bits refuse");
    CHECK("block sum limits", !rdna4hb_add_block(&h, 1 * MIB + 1, 0, 0, 0, 0, 0, (void *)1, NULL, NULL) && !h.nblk,
          "refused over-capacity block");

    // Random ownership/retirement property: no live range overlaps and backing stays bounded.
    rdna4hb_destroy(&h);
    rdna4hb_init(&h, 16 * MIB, RDNA4HB_MAX_NORMAL);
    add(&h, 16 * MIB, 0, 0, 0, 0, 0x8000, &block);
    enum { NL = 64 };
    typedef struct { uint64_t lease, off, size; } live_range;
    live_range live[NL];
    int nl = 0, overlaps = 0, stale = 0;
    uint64_t serial = 0;
    srand(31);
    for (int i = 0; i < 20000; i++) {
        if (nl < NL && (rand() & 1)) {
            uint64_t size = 1 + (uint64_t)(rand() % 65536), align = 1ULL << (rand() % 13);
            if (rdna4hb_alloc(&h, size, align, 256, 1, 0, 0, 0, NULL, &r) == RDNA4HB_R_OK) {
                uint64_t need = (size + 255) & ~UINT64_C(255);
                if (r.offset % align) stale++;
                for (int j = 0; j < nl; j++)
                    if (r.offset < live[j].off + live[j].size && live[j].off < r.offset + need) overlaps++;
                live[nl++] = (live_range){ r.lease, r.offset, need };
            }
        } else if (nl) {
            int j = rand() % nl;
            rdna4hb_release(&h, live[j].lease, ++serial);
            live[j] = live[--nl];
        }
        if ((i & 7) == 0) rdna4hb_reap(&h, serial > 2 ? serial - 2 : 0);
        if (h.backing > h.size) stale++;
    }
    rdna4hb_reap(&h, serial);
    CHECK("random ranges never overlap", overlaps == 0 && stale == 0, "%d overlaps, %d invariant failures", overlaps, stale);
    for (int j = 0; j < nl; j++) rdna4hb_release(&h, live[j].lease, serial + 1);
    rdna4hb_reap(&h, serial + 1);
    CHECK("all random leases retire", h.nlease == 0, "leases %zu", h.nlease);
    rdna4hb_destroy(&h);

    printf(fails ? "FAIL test-heap-backing (%d)\n" : "PASS test-heap-backing\n", fails);
    return fails ? 1 : 0;
}
