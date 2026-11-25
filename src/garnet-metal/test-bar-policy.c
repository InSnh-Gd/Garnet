// test-bar-policy.c — Host ASan/UBSan tests for Phase A4 dual-BAR policy
// (garnet_bar_policy.h) and linear VkImage reuse cache (rdna4li_cache in garnet_impcache.h).
#include <stdio.h>
#include <stdint.h>
#include "garnet_bar_policy.h"
#include "garnet_impcache.h"

static int g_checks = 0, g_fails = 0;
#define CHECK(name, cond) do { \
    g_checks++; \
    if (!(cond)) { g_fails++; printf("FAIL: %s\n", name); } \
} while (0)

int main(void) {
    // 1. Dual-BAR policy
    garnet_bar_policy_t p;

    p = garnet_bar_choose_policy(0, 256, 0, 0);
    CHECK("256 MiB Small-BAR: nosam ON, cap 512 MiB",
          p.full_bar == 0 && p.enable_nosam == 1 && p.vram_surf_cap_bytes == GARNET_VRAM_SURF_CAP_SMALL_BAR);

    p = garnet_bar_choose_policy(0, 0, 0, 0);
    CHECK("IORegistry probe failure (0, 0) falls back to Small-BAR safely",
          p.full_bar == 0 && p.enable_nosam == 1 && p.vram_surf_cap_bytes == GARNET_VRAM_SURF_CAP_SMALL_BAR);

    p = garnet_bar_choose_policy(1, 256, 0, 0);
    CHECK("fullMode=1 but bar0MB < 4096 still stays Small-BAR",
          p.full_bar == 0 && p.enable_nosam == 1 && p.vram_surf_cap_bytes == GARNET_VRAM_SURF_CAP_SMALL_BAR);

    p = garnet_bar_choose_policy(1, 8192, 0, 0);
    CHECK("8 GiB Full-BAR: nosam OFF, cap 1536 MiB",
          p.full_bar == 1 && p.enable_nosam == 0 && p.vram_surf_cap_bytes == GARNET_VRAM_SURF_CAP_FULL_BAR);

    p = garnet_bar_choose_policy(1, 16384, 0, 0);
    CHECK("16 GiB Full-BAR: nosam OFF, cap 1536 MiB",
          p.full_bar == 1 && p.enable_nosam == 0 && p.vram_surf_cap_bytes == GARNET_VRAM_SURF_CAP_FULL_BAR);

    p = garnet_bar_choose_policy(1, 16384, 1, 0);
    CHECK("Full-BAR with /private/tmp/garnet-nosam force file enables nosam",
          p.full_bar == 1 && p.enable_nosam == 1 && p.vram_surf_cap_bytes == GARNET_VRAM_SURF_CAP_FULL_BAR);

    p = garnet_bar_choose_policy(0, 256, 0, 1);
    CHECK("Small-BAR with /private/tmp/garnet-nosam-off disables nosam",
          p.full_bar == 0 && p.enable_nosam == 0 && p.vram_surf_cap_bytes == GARNET_VRAM_SURF_CAP_SMALL_BAR);

    p = garnet_bar_choose_policy(0, 256, 1, 1);
    CHECK("force nosam takes precedence over nosam-off",
          p.enable_nosam == 1);

    // 2. Linear VkImage reuse cache (eliminates redundant rdna4_ios_linear_init)
    rdna4li_cache lc;
    rdna4li_init(&lc, 1, 64);
    rdna4li_key k1 = { .id = 41, .base = 0x100000, .size = 33554432, .poff = 0,
                       .w = 3840, .h = 2160, .bpr = 15360, .vk_fmt = 44, .usage = 0x17, .flags = 0 };
    void *img = NULL; int inited = 0;
    CHECK("cold miss returns 0", rdna4li_acquire(&lc, &k1, 1000, 1, &img, &inited) == 0);
    rdna4li_add(&lc, &k1, (void *)0xdead1, 0);
    CHECK("uninitialized image starts inited=0", lc.n == 1 && lc.created == 1);
    rdna4li_mark_inited(&lc, (void *)0xdead1);

    // Second wrapper hits the same VkImage AND sees inited=1 (skips 66 MB rdna4_ios_linear_init!)
    CHECK("warm hit reuses VkImage and reports inited=1",
          rdna4li_acquire(&lc, &k1, 2000, 2, &img, &inited) == 1 && img == (void *)0xdead1 && inited == 1);
    CHECK("unref 1st wrapper keeps entry live (refs=1)",
          rdna4li_unref(&lc, (void *)0xdead1, 3000, 3) == 1 && lc.nrl == 0);
    CHECK("unref 2nd wrapper caches unused entry (refs=0, not destroyed yet)",
          rdna4li_unref(&lc, (void *)0xdead1, 4000, 4) == 1 && lc.nrl == 0 && lc.n == 1);

    // Reacquire from cached unused pool
    CHECK("reacquire from unused pool hits with inited=1",
          rdna4li_acquire(&lc, &k1, 5000, 5, &img, &inited) == 1 && img == (void *)0xdead1 && inited == 1);
    rdna4li_unref(&lc, (void *)0xdead1, 6000, 6);

    // Same id with different base/size marks old entry stale and queues release on fence
    rdna4li_key k1_moved = k1;
    k1_moved.base = 0x200000;
    CHECK("moved backing address misses and retires old unused entry",
          rdna4li_acquire(&lc, &k1_moved, 7000, 7, &img, &inited) == 0 && lc.nrl == 1 && lc.mismatches == 1);
    void *relImg = NULL;
    CHECK("fence-gated pop refuses before stamp 7", rdna4li_pop(&lc, 6, &relImg) == 0);
    CHECK("fence-gated pop returns retired VkImage at stamp 7",
          rdna4li_pop(&lc, 7, &relImg) == 1 && relImg == (void *)0xdead1);

    // Idle eviction after RDNA4IC_IDLE_NS
    rdna4li_add(&lc, &k1_moved, (void *)0xdead2, 1);
    rdna4li_unref(&lc, (void *)0xdead2, 10000, 8);
    rdna4li_trim(&lc, 10000 + RDNA4IC_IDLE_NS + 1, 9);
    CHECK("idle trim retires unused entry past RDNA4IC_IDLE_NS",
          lc.nrl == 1 && rdna4li_pop(&lc, 9, &relImg) == 1 && relImg == (void *)0xdead2);

    // Orphan retirement when underlying RDNA4IC import is evicted/flushed, AND when remapped with a NEW VkDeviceMemory at the SAME (id, base, size)
    rdna4ic_cache ic;
    rdna4ic_init(&ic, 1);
    rdna4ic_add(&ic, k1_moved.id, k1_moved.base, k1_moved.size, (void *)0xbeef1, NULL);
    rdna4li_key k1_mem = k1_moved;
    k1_mem.mem = (void *)0xbeef1;
    rdna4li_add(&lc, &k1_mem, (void *)0xdead3, 1);
    rdna4li_unref(&lc, (void *)0xdead3, 20000, 10);
    rdna4ic_unref(&ic, (void *)0xbeef1, 20000, 10);
    rdna4ic_flush(&ic, 11);
    // Simulate remapped backing at the EXACT same (id, base, size) with a NEW VkDeviceMemory (0xbeef2)
    rdna4ic_add(&ic, k1_moved.id, k1_moved.base, k1_moved.size, (void *)0xbeef2, NULL);
    rdna4li_retire_orphans(&lc, &ic, 11);
    CHECK("orphan linear VkImage retired when its exact VkDeviceMemory was freed even if new mem occupies same address",
          lc.n == 0 && lc.nrl == 1 && rdna4li_pop(&lc, 11, &relImg) == 1 && relImg == (void *)0xdead3);
    rdna4li_add(&lc, &k1_mem, (void *)0xdead4, 0);
    CHECK("rdna4li_is_inited tracks shared initialization bit",
          rdna4li_is_inited(&lc, (void *)0xdead4) == 0);
    rdna4li_mark_inited(&lc, (void *)0xdead4);
    CHECK("rdna4li_is_inited returns 1 after mark_inited",
          rdna4li_is_inited(&lc, (void *)0xdead4) == 1);
    rdna4ic_destroy(&ic);

    rdna4li_destroy(&lc);
    printf("test-bar-policy: %d checks, %d failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
