// test-heap-alloc.c: regression tests for MTLHeap pf 115 RGBA16Float
// tiled alignment parity, 3-7 MiB mediaanalysisd-shaped multi-texture heaps,
// retired-handle eviction & coalescing, and source pins on garnet_resources.m /
// garnet_device.m.
//
// Host build:
//   clang -std=c11 -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
//     -Isrc/garnet-metal src/garnet-metal/test-heap-alloc.c -o test-heap-alloc

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "garnet_heapalloc.h"
#include "garnet_heap_backing.h"

static int fails;
static int checks;

#define CHECK(name, cond, ...) do { \
    checks++; \
    int ok_ = (cond); \
    if (!ok_) { \
        fprintf(stderr, "FAIL %s: ", name); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
        fails++; \
    } \
} while (0)

#define KIB (UINT64_C(1) << 10)
#define MIB (UINT64_C(1) << 20)

static char *read_text_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        char alt[512];
        snprintf(alt, sizeof(alt), "src/garnet-metal/%s", path);
        f = fopen(alt, "rb");
    }
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = (char *)malloc((size_t)sz + 1u);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

typedef struct {
    int destroyed;
    int handle_open;
} mock_backing;

static int sim_evict_and_collect(rdna4hb_heap *h, uint64_t completed,
                                 uint64_t *freed_blocks_out) {
    int evicted = 0;
    rdna4hb_reap(h, completed);
    for (;;) {
        uint64_t lid = 0, bid = 0;
        void *owner = NULL;
        if (!rdna4hb_finalize_candidate(h, &lid, &bid, &owner)) break;
        mock_backing *mb = (mock_backing *)owner;
        mb->destroyed++;
        if (mb->handle_open) {
            mb->handle_open = 0;
            rdna4hb_handle_close(h, bid);
        }
        rdna4hb_finalized(h, lid);
        evicted++;
    }
    for (;;) {
        uint64_t lid = 0, bid = 0;
        void *owner = NULL;
        if (!rdna4hb_evict_candidate(h, &lid, &bid, &owner)) break;
        mock_backing *mb = (mock_backing *)owner;
        mb->destroyed++;
        if (mb->handle_open) {
            mb->handle_open = 0;
            rdna4hb_handle_close(h, bid);
        }
        rdna4hb_finalized_retired(h, lid);
        evicted++;
    }
    rdna4hb_reclaim gone;
    while (rdna4hb_take_reclaimable(h, &gone)) {
        if (freed_blocks_out) (*freed_blocks_out)++;
    }
    return evicted;
}

static int sim_alloc_with_owner(RDNA4HeapAlloc *ha, rdna4hb_heap *hb,
                                uint64_t mr_size, uint64_t mr_align,
                                uint64_t gran, uint32_t mem_bits,
                                int dedicated, int requires_dedicated,
                                uint64_t completed, mock_backing *owner,
                                uint64_t *logical_id_out, uint64_t *lease_out,
                                uint64_t *block_out, uint64_t *freed_blocks_out,
                                uint64_t *alloc_blocks_out) {
    uint64_t g = gran ? gran : 1;
    uint64_t ext = rdna4ha_alup(mr_size, g);
    uint64_t al = mr_align > g ? mr_align : g;
    uint64_t loff = 0, lid = 0;
    if (!rdna4ha_alloc_ext(ha, mr_size, ext, al, &loff, &lid)) return 0;

    int try_dedicated = dedicated;
    for (int pass = 0; pass < 2; pass++) {
        rdna4hb_reap(hb, completed);
        for (int attempt = 0; attempt < 5; attempt++) {
            rdna4hb_res res = {0};
            int act = rdna4hb_alloc(hb, mr_size, mr_align, g, mem_bits,
                                    0, 0, try_dedicated, NULL, &res);
            if (act == RDNA4HB_R_OK) {
                if (!rdna4hb_handle_open(hb, res.block)) {
                    rdna4ha_free(ha, lid);
                    return 0;
                }
                owner->handle_open = 1;
                if (!rdna4hb_lease_owner(hb, res.lease, owner)) {
                    rdna4hb_handle_close(hb, res.block);
                    owner->handle_open = 0;
                    rdna4ha_free(ha, lid);
                    return 0;
                }
                if (logical_id_out) *logical_id_out = lid;
                if (lease_out) *lease_out = res.lease;
                if (block_out) *block_out = res.block;
                return 1;
            }
            if (act == RDNA4HB_R_RETRY) {
                sim_evict_and_collect(hb, completed, freed_blocks_out);
                continue;
            }
            if (act == RDNA4HB_R_NEED_BLOCK) {
                uint64_t bid = 0;
                uint32_t mtype = 0;
                while (mtype < 32 && !(mem_bits & (1u << mtype))) mtype++;
                uintptr_t tok = (uintptr_t)(0x10000 + (hb->block_ctr + 1) * 0x100);
                if (!rdna4hb_add_block(hb, res.block_size, mtype, 0,
                                       try_dedicated, res.exclusive, 0,
                                       (void *)tok, NULL, &bid))
                    break;
                if (alloc_blocks_out) (*alloc_blocks_out)++;
                continue;
            }
            break;
        }
        if (try_dedicated && !requires_dedicated) {
            try_dedicated = 0;
            continue;
        }
        break;
    }
    rdna4ha_free(ha, lid);
    return 0;
}

static void test_pf115_query_extent_and_alignment_parity(void) {
    uint64_t sz = 0, al = 0;

    // RADV real-world pf 115 (RGBA16Float) 299x299 tiled image:
    // bufferImageGranularity = 1, mr.size = 778240 (11.875 * 64 KiB), mr.align = 65536.
    // Query size must round up to 786432 (12 * 64 KiB) so multi-texture heap packing
    // never runs out of room from inter-texture alignment gaps.
    CHECK("pf 115 299x299 RADV gran=1 rounds query size to 64 KiB alignment",
          rdna4ha_query_extent(778240ULL, 65536ULL, 1ULL, 0, 0, &sz, &al) &&
              sz == 786432ULL && al == 65536ULL && (sz % al) == 0,
          "got size=%llu align=%llu", (unsigned long long)sz, (unsigned long long)al);

    // Pack 4 and 8 pf 115 textures into heaps sized from heapTextureSizeAndAlignWithDescriptor:.
    // Before rounding query size up to align, 4 * 778240 = 3112960 failed on texture #4
    // (needed 3 * 786432 + 778240 = 3137536).
    for (int count = 4; count <= 8; count += 4) {
        RDNA4HeapAlloc ha;
        rdna4ha_init(&ha, (uint64_t)count * sz);
        int all_fit = 1;
        for (int i = 0; i < count; i++) {
            uint64_t off = 0, id = 0;
            if (!rdna4ha_alloc_ext(&ha, 778240ULL, 778240ULL, 65536ULL, &off, &id) ||
                (off % 65536ULL) != 0) {
                all_fit = 0;
                break;
            }
        }
        CHECK("pf 115 multi-texture heap sized from query fits all textures",
              all_fit && ha.used == (uint64_t)count * 778240ULL,
              "count=%d all_fit=%d used=%llu/%llu",
              count, all_fit, (unsigned long long)ha.used, (unsigned long long)ha.size);
        rdna4ha_destroy(&ha);
    }

    // Granularity > mr.align (test-heap-execution.m parity).
    CHECK("gran=64 KiB dominates align=16 KiB",
          rdna4ha_query_extent(131073ULL, 16384ULL, 65536ULL, 0, 0, &sz, &al) &&
              sz == 196608ULL && al == 65536ULL,
          "got size=%llu align=%llu", (unsigned long long)sz, (unsigned long long)al);

    // Large host-import buffer (>64 MiB Shared/Managed).
    CHECK("host import aligns extent and alignment to hostAlign=256 KiB",
          rdna4ha_query_extent((64 * MIB) + 1, 4096ULL, 65536ULL, 1, 256 * KIB, &sz, &al) &&
              sz == (64 * MIB) + (256 * KIB) && al == 256 * KIB,
          "got size=%llu align=%llu", (unsigned long long)sz, (unsigned long long)al);

    // Invalid inputs and overflow must fail closed.
    CHECK("invalid and overflowing query extents refuse",
          !rdna4ha_query_extent(0, 4096, 1, 0, 0, &sz, &al) &&
              !rdna4ha_query_extent(4096, 0, 1, 0, 0, &sz, &al) &&
              !rdna4ha_query_extent(4096, 3, 1, 0, 0, &sz, &al) &&
              !rdna4ha_query_extent(4096, 4096, 3, 0, 0, &sz, &al) &&
              !rdna4ha_query_extent(4096, 4096, 1, 1, 0, &sz, &al) &&
              !rdna4ha_query_extent(UINT64_MAX - 10, 65536, 1, 0, 0, &sz, &al),
          "invalid cases rejected");
}

static void test_mediaanalysisd_shattered_2mib_and_3_to_7mib_heaps(void) {
    // 1. Exact replay of mediaanalysisd 2 MiB heap: small buffers total 589,824 B,
    //    become aliasable & GPU-retired while ObjC wrappers stay alive (handles > 0),
    //    then a 1,966,080 B request arrives. Must evict retired blocks and succeed.
    {
        RDNA4HeapAlloc ha;
        rdna4hb_heap hb;
        rdna4ha_init(&ha, 2 * MIB);
        rdna4hb_init(&hb, 2 * MIB, RDNA4HB_MAX_NORMAL);

        const uint64_t wave1[] = { 131072, 131072, 131072, 131072, 65536 }; // 589,824 B
        mock_backing owners1[5] = {{0}};
        uint64_t lids[5] = {0}, leases[5] = {0};
        uint64_t freed = 0, allocs = 0;

        int w1_ok = 1;
        for (int i = 0; i < 5; i++) {
            w1_ok &= sim_alloc_with_owner(&ha, &hb, wave1[i], 256, 1, 1, 0, 0, 10,
                                          &owners1[i], &lids[i], &leases[i], NULL,
                                          &freed, &allocs);
        }
        CHECK("2 MiB wave1 allocates 589,824 B across exact blocks",
              w1_ok && hb.backing == 589824ULL,
              "ok=%d backing=%llu", w1_ok, (unsigned long long)hb.backing);

        // makeAliasable while ObjC objects remain alive; GPU serial 10 already complete.
        for (int i = 0; i < 5; i++) {
            rdna4ha_free(&ha, lids[i]);
            rdna4hb_release(&hb, leases[i], 10);
        }
        rdna4hb_reap(&hb, 10);
        CHECK("2 MiB retired blocks report full 2 MiB maxAvailable",
              rdna4hb_max_available(&hb, 256, 1, 0) == 2 * MIB &&
                  rdna4hb_has_evictable_retired(&hb) == 1,
              "maxAvail=%llu evictable=%d",
              (unsigned long long)rdna4hb_max_available(&hb, 256, 1, 0),
              rdna4hb_has_evictable_retired(&hb));

        // Now request 1,966,080 B — previously failed with "backing 589824 of 2097152".
        mock_backing big_owner = {0};
        uint64_t big_lid = 0, big_lease = 0;
        int big_ok = sim_alloc_with_owner(&ha, &hb, 1966080ULL, 256, 1, 1, 0, 0, 10,
                                          &big_owner, &big_lid, &big_lease, NULL,
                                          &freed, &allocs);
        int all_destroyed = 1;
        for (int i = 0; i < 5; i++) all_destroyed &= (owners1[i].destroyed == 1 && !owners1[i].handle_open);
        CHECK("2 MiB 1,966,080 B allocation evicts retired blocks and succeeds",
              big_ok && freed == 5 && all_destroyed && hb.backing == 1966080ULL && hb.backing <= hb.size,
              "ok=%d freed=%llu all_destroyed=%d backing=%llu/%llu",
              big_ok, (unsigned long long)freed, all_destroyed,
              (unsigned long long)hb.backing, (unsigned long long)hb.size);

        rdna4ha_free(&ha, big_lid);
        rdna4hb_release(&hb, big_lease, 11);
        sim_evict_and_collect(&hb, 11, &freed);
        CHECK("2 MiB cleanup reclaims everything", hb.backing == 0 && hb.nlease == 0 && hb.nblk == 0,
              "backing=%llu nlease=%zu nblk=%zu", (unsigned long long)hb.backing, hb.nlease, hb.nblk);
        rdna4ha_destroy(&ha);
        rdna4hb_destroy(&hb);
    }

    // 2. mediaanalysisd 3.28 MiB (3,440,640 B), 6.15 MiB (6,447,104 B), and
    //    6.75 MiB (7,077,888 B) pf 115 multi-texture heaps across 3 aliasing waves.
    const uint64_t heap_sizes[3] = { 3440640ULL, 6447104ULL, 7077888ULL };
    for (int hi = 0; hi < 3; hi++) {
        uint64_t hsz = heap_sizes[hi];
        RDNA4HeapAlloc ha;
        rdna4hb_heap hb;
        rdna4ha_init(&ha, hsz);
        rdna4hb_init(&hb, hsz, RDNA4HB_MAX_NORMAL);

        // Batch 1: many small pf 115 textures filling nearly the entire 3-7 MiB heap.
        const uint64_t small_sizes[] = {
            46080, 98304, 196608, 245760, 262144, 393216, 442368, 552960, 573440, 622592
        };
        mock_backing w1_owners[32] = {{0}};
        uint64_t w1_lids[32] = {0}, w1_leases[32] = {0};
        int w1_n = 0;
        uint64_t freed = 0, allocs = 0;
        for (int pass = 0; pass < 2 && w1_n < 32; pass++) {
            for (int i = 0; i < 10 && w1_n < 32; i++) {
                uint64_t req = small_sizes[i];
                if (ha.used + req > hsz) continue;
                int ok = sim_alloc_with_owner(&ha, &hb, req, 4096, 1, 1, 0, 0, 1,
                                              &w1_owners[w1_n], &w1_lids[w1_n], &w1_leases[w1_n],
                                              NULL, &freed, &allocs);
                if (ok) w1_n++;
            }
        }
        CHECK("mediaanalysisd 3-7 MiB wave1 populates heap", w1_n >= 5 && hb.backing <= hsz,
              "hsz=%llu w1_n=%d backing=%llu", (unsigned long long)hsz, w1_n, (unsigned long long)hb.backing);

        // Mark all wave1 textures aliasable and complete serial 1 (objects still alive).
        for (int i = 0; i < w1_n; i++) {
            rdna4ha_free(&ha, w1_lids[i]);
            rdna4hb_release(&hb, w1_leases[i], 1);
        }
        rdna4hb_reap(&hb, 1);

        // Batch 2: large pf 115 textures (2,064,384 B + 1,130,496 B + 16,128 B = 3,211,008 B <= 3.28 MiB).
        const uint64_t wave2[] = { 2064384ULL, 1130496ULL, 16128ULL };
        mock_backing w2_owners[3] = {{0}};
        uint64_t w2_lids[3] = {0}, w2_leases[3] = {0};
        int w2_ok = 1;
        for (int i = 0; i < 3; i++) {
            w2_ok &= sim_alloc_with_owner(&ha, &hb, wave2[i], 65536, 1, 1, 0, 0, 1,
                                          &w2_owners[i], &w2_lids[i], &w2_leases[i],
                                          NULL, &freed, &allocs);
        }
        CHECK("mediaanalysisd 3-7 MiB wave2 large pf 115 textures succeed without NIL",
              w2_ok && hb.backing <= hsz && freed > 0,
              "hsz=%llu w2_ok=%d backing=%llu freed=%llu",
              (unsigned long long)hsz, w2_ok, (unsigned long long)hb.backing, (unsigned long long)freed);

        // Batch 3: keep w2[0] (2,064,384 B) LIVE while retiring w2[1] + w2[2],
        // then allocate a new 737,280 B + 393,216 B pair (total 1,130,496 B).
        rdna4ha_free(&ha, w2_lids[1]);
        rdna4hb_release(&hb, w2_leases[1], 2);
        rdna4ha_free(&ha, w2_lids[2]);
        rdna4hb_release(&hb, w2_leases[2], 2);
        rdna4hb_reap(&hb, 2);

        mock_backing w3_owners[2] = {{0}};
        uint64_t w3_lids[2] = {0}, w3_leases[2] = {0};
        int w3_ok = sim_alloc_with_owner(&ha, &hb, 737280ULL, 65536, 1, 1, 0, 0, 2,
                                         &w3_owners[0], &w3_lids[0], &w3_leases[0],
                                         NULL, &freed, &allocs) &&
                    sim_alloc_with_owner(&ha, &hb, 393216ULL, 65536, 1, 1, 0, 0, 2,
                                         &w3_owners[1], &w3_lids[1], &w3_leases[1],
                                         NULL, &freed, &allocs);
        CHECK("mediaanalysisd 3-7 MiB wave3 partial live + retired reuse succeeds",
              w3_ok && w2_owners[0].destroyed == 0 && hb.backing <= hsz,
              "hsz=%llu w3_ok=%d live_intact=%d backing=%llu",
              (unsigned long long)hsz, w3_ok, w2_owners[0].destroyed == 0, (unsigned long long)hb.backing);

        rdna4ha_free(&ha, w2_lids[0]); rdna4hb_release(&hb, w2_leases[0], 3);
        rdna4ha_free(&ha, w3_lids[0]); rdna4hb_release(&hb, w3_leases[0], 3);
        rdna4ha_free(&ha, w3_lids[1]); rdna4hb_release(&hb, w3_leases[1], 3);
        sim_evict_and_collect(&hb, 3, &freed);
        CHECK("mediaanalysisd 3-7 MiB full drain clean", hb.backing == 0 && hb.nlease == 0,
              "hsz=%llu backing=%llu nlease=%zu", (unsigned long long)hsz, (unsigned long long)hb.backing, hb.nlease);
        rdna4ha_destroy(&ha);
        rdna4hb_destroy(&hb);
    }
}

static void test_inflight_safety_and_same_block_reuse_and_dedicated_fallback(void) {
    // 1. In-flight retired leases (stamp > completed) MUST NOT be evicted.
    {
        RDNA4HeapAlloc ha;
        rdna4hb_heap hb;
        rdna4ha_init(&ha, 786432ULL);
        rdna4hb_init(&hb, 786432ULL, RDNA4HB_MAX_NORMAL);

        mock_backing o1 = {0}, o2 = {0};
        uint64_t lid1 = 0, lease1 = 0, freed = 0, allocs = 0;
        CHECK("768 KiB initial pf 115 alloc",
              sim_alloc_with_owner(&ha, &hb, 778240ULL, 65536, 1, 1, 0, 0, 5,
                                   &o1, &lid1, &lease1, NULL, &freed, &allocs),
              "backing=%llu", (unsigned long long)hb.backing);
        rdna4ha_free(&ha, lid1);
        rdna4hb_release(&hb, lease1, 6); // serial 6 still in flight (completed=5)
        rdna4hb_reap(&hb, 5);
        CHECK("in-flight block is NOT evictable and refuses reuse",
              !rdna4hb_has_evictable_retired(&hb) &&
                  rdna4hb_max_available(&hb, 65536, 1, 0) < 778240ULL &&
                  !sim_alloc_with_owner(&ha, &hb, 778240ULL, 65536, 1, 1, 0, 0, 5,
                                        &o2, NULL, NULL, NULL, &freed, &allocs) &&
                  o1.destroyed == 0,
              "evictable=%d destroyed=%d", rdna4hb_has_evictable_retired(&hb), o1.destroyed);

        // Once serial 6 completes, reusing the SAME block succeeds WITHOUT freeing it!
        uint64_t b2 = 0, lid2 = 0, lease2 = 0;
        CHECK("completed serial reuses existing block without unnecessary churn",
              sim_alloc_with_owner(&ha, &hb, 778240ULL, 65536, 1, 1, 0, 0, 6,
                                   &o2, &lid2, &lease2, &b2, &freed, &allocs) &&
                  allocs == 1 && freed == 0 && b2 == 1 && o1.destroyed == 0,
              "allocs=%llu freed=%llu b2=%llu", (unsigned long long)allocs,
              (unsigned long long)freed, (unsigned long long)b2);
        rdna4ha_free(&ha, lid2);
        rdna4hb_release(&hb, lease2, 7);
        sim_evict_and_collect(&hb, 7, &freed);
        CHECK("both owners destroyed exactly once on full reclaim",
              o1.destroyed == 1 && o2.destroyed == 1 && hb.backing == 0,
              "o1=%d o2=%d backing=%llu", o1.destroyed, o2.destroyed, (unsigned long long)hb.backing);
        rdna4ha_destroy(&ha);
        rdna4hb_destroy(&hb);
    }

    // 2. prefersDedicated && !requiresDedicated falls back to existing non-dedicated slab
    //    when remaining unbacked capacity is zero but the slab has free space.
    {
        RDNA4HeapAlloc ha;
        rdna4hb_heap hb;
        rdna4ha_init(&ha, 16 * MIB);
        rdna4hb_init(&hb, 16 * MIB, RDNA4HB_MAX_NORMAL);

        mock_backing o_slab = {0}, o_fill[3] = {{0}}, o_pref = {0};
        uint64_t l_slab = 0, s_slab = 0, freed = 0, allocs = 0;
        // First small alloc in 16 MiB heap creates a 4 MiB shared slab (3.75 MiB free inside).
        CHECK("initial small alloc creates 4 MiB slab",
              sim_alloc_with_owner(&ha, &hb, 256 * KIB, 65536, 1, 1, 0, 0, 0,
                                   &o_slab, &l_slab, &s_slab, NULL, &freed, &allocs) &&
                  hb.backing == 4 * MIB,
              "backing=%llu", (unsigned long long)hb.backing);
        // Consume the remaining 12 MiB with 3 dedicated 4 MiB blocks so remaining == 0.
        uint64_t l_fill[3] = {0}, s_fill[3] = {0};
        int fill_ok = 1;
        for (int i = 0; i < 3; i++) {
            fill_ok &= sim_alloc_with_owner(&ha, &hb, 4 * MIB, 65536, 1, 1, 1, 1, 0,
                                            &o_fill[i], &l_fill[i], &s_fill[i], NULL,
                                            &freed, &allocs);
        }
        CHECK("dedicated blocks fill remaining 12 MiB", fill_ok && hb.backing == 16 * MIB,
              "fill_ok=%d backing=%llu", fill_ok, (unsigned long long)hb.backing);

        // Now a 1 MiB image with prefersDedicated=1, requiresDedicated=0 arrives:
        // dedicated fails (remaining==0), fallback to non-dedicated packs into the 4 MiB slab!
        uint64_t l_pref = 0, s_pref = 0, b_pref = 0;
        CHECK("prefersDedicated falls back to non-dedicated slab when remaining==0",
              sim_alloc_with_owner(&ha, &hb, 1 * MIB, 65536, 1, 1, 1, 0, 0,
                                   &o_pref, &l_pref, &s_pref, &b_pref, &freed, &allocs) &&
                  b_pref == 1 && hb.backing == 16 * MIB,
              "b_pref=%llu backing=%llu", (unsigned long long)b_pref, (unsigned long long)hb.backing);

        // Meanwhile a truly requiresDedicated=1 image must still fail cleanly.
        mock_backing o_req = {0};
        CHECK("requiresDedicated=1 still refuses when remaining==0",
              !sim_alloc_with_owner(&ha, &hb, 1 * MIB, 65536, 1, 1, 1, 1, 0,
                                    &o_req, NULL, NULL, NULL, &freed, &allocs),
              "requiresDedicated refused");

        rdna4ha_free(&ha, l_slab); rdna4hb_release(&hb, s_slab, 1);
        rdna4ha_free(&ha, l_pref); rdna4hb_release(&hb, s_pref, 1);
        for (int i = 0; i < 3; i++) {
            rdna4ha_free(&ha, l_fill[i]); rdna4hb_release(&hb, s_fill[i], 1);
        }
        sim_evict_and_collect(&hb, 1, &freed);
        CHECK("dedicated fallback cleanup clean", hb.backing == 0 && hb.nlease == 0,
              "backing=%llu nlease=%zu", (unsigned long long)hb.backing, hb.nlease);
        rdna4ha_destroy(&ha);
        rdna4hb_destroy(&hb);
    }
}

static void test_source_pins(void) {
    char *res = read_text_file("garnet_resources.m");
    char *dev = read_text_file("garnet_device.m");
    CHECK("garnet_resources.m and garnet_device.m readable", res != NULL && dev != NULL,
          "run from src/garnet-metal");
    if (res && dev) {
        CHECK("garnet_device.m delegates query extent to rdna4ha_query_extent",
              strstr(dev, "rdna4ha_query_extent(mr.size, mr.alignment, RDNA4R.lim.bufferImageGranularity,") != NULL,
              "rdna4ha_query_extent call missing in garnet_device.m");
        CHECK("garnet_device.m queries vkGetImageMemoryRequirements2 via rdna4_heap_image_plan_make",
              strstr(dev, "rdna4_heap_image_plan_make(d, YES, &plan, &error)") != NULL &&
                  strstr(dev, "vkCreateImage(RDNA4R.dev, &plan.create, NULL, &image)") != NULL &&
                  strstr(dev, "rdna4_requirements2(VK_NULL_HANDLE, image, &mr, NULL, NULL, YES)") != NULL,
              "image size/align parity missing in garnet_device.m");
        CHECK("garnet_resources.m shares rdna4_heap_image_plan_make in RDNA4Texture",
              strstr(res, "rdna4_heap_image_plan_make(d, heap != nil, &plan, err)") != NULL,
              "rdna4_heap_image_plan_make missing in garnet_resources.m");
        CHECK("garnet_resources.m wires retired-block eviction and one-shot handle close",
              strstr(res, "rdna4EvictRetiredBlocksLocked") != NULL &&
                  strstr(res, "rdna4hb_evict_candidate") != NULL &&
                  strstr(res, "rdna4hb_finalized_retired") != NULL &&
                  strstr(res, "rdna4ConsumeHandleOpen") != NULL,
              "eviction helpers missing in garnet_resources.m");
        CHECK("garnet_resources.m falls back from prefersDedicated when !requiresDedicated",
              strstr(res, "if (!acquired && dedicated && !requiresDedicated)") != NULL,
              "prefersDedicated fallback missing in garnet_resources.m");
    }
    free(res);
    free(dev);
}

int main(void) {
    test_pf115_query_extent_and_alignment_parity();
    test_mediaanalysisd_shattered_2mib_and_3_to_7mib_heaps();
    test_inflight_safety_and_same_block_reuse_and_dedicated_fallback();
    test_source_pins();
    printf(fails ? "FAIL test-heap-alloc (%d of %d failed)\n"
                 : "PASS test-heap-alloc (%d checks)\n",
           fails ? fails : checks, checks);
    return fails ? 1 : 0;
}
