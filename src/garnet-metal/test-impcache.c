// test-impcache.c: garnet_impcache.h (P4 import cache + classify cache decisions) on the host.
// Build/run on the host Mac:  cc -O1 -Wall -Wextra -Werror -o /tmp/test-impcache test-impcache.c && /tmp/test-impcache
// Also exercise the actual admission path with host stubs (run from this directory):
// sed -n '/^static VkResult rdna4_imp_get(/,/^}/p' GarnetDevice.m > /tmp/rdna4-imp-get.inc
// cc -O1 -Wall -Wextra -Werror -pthread -DRDNA4_IMP_GET_BODY='"/tmp/rdna4-imp-get.inc"' -o /tmp/test-impcache test-impcache.c && /tmp/test-impcache
#include <stdio.h>
#include "garnet_impcache.h"
static int fails;
#define CHECK(name, cond, ...) do { int ok_ = (cond); printf("%s %s: ", ok_ ? "ok  " : "FAIL", name); printf(__VA_ARGS__); printf("\n"); if (!ok_) fails++; } while (0)
#define MIB (1ULL << 20)
#define SEC 1000000000ULL
#define M(n) ((void *)(uintptr_t)(n))

#ifdef RDNA4_IMP_GET_BODY
#include <pthread.h>
typedef int BOOL;
#define YES 1
#define NO 0
#define VK_SUCCESS 0
#define VK_ERROR_OUT_OF_DEVICE_MEMORY (-2)
#define VK_NULL_HANDLE NULL
typedef int VkResult;
typedef void *VkDeviceMemory;
typedef struct { uint64_t allocationSize; } VkMemoryAllocateInfo;
typedef struct { uint32_t id; int retains; } test_surface;
typedef test_surface *IOSurfaceRef;
static struct { pthread_mutex_t mu; rdna4ic_cache c; rdna4li_cache li; } RDNA4IC = { .mu = PTHREAD_MUTEX_INITIALIZER };
static struct { void *dev; uint64_t impTotal; } RDNA4R;
static test_surface competing;
static VkDeviceMemory competing_mem;
static BOOL competing_hit;
static int allocations, frees, interleave, unref_competing;
static int freed[8];
static int allocation_attempts;
static uint64_t import_budget, completed;
static uint64_t rdna4_now(void) { return 10; }
static void rdna4_imp_fence(uint64_t *st, uint64_t *comp) { *st = 7; *comp = completed; }
static uint32_t IOSurfaceGetID(IOSurfaceRef s) { return s->id; }
static void *CFRetain(IOSurfaceRef s) { s->retains++; return s; }
static void CFRelease(IOSurfaceRef s) { s->retains--; }
static void check_unlocked(const char *name) {
    int r = pthread_mutex_trylock(&RDNA4IC.mu);
    if (!r) pthread_mutex_unlock(&RDNA4IC.mu);
    CHECK(name, r == 0, "trylock %d", r);
}
static void vkFreeMemory(void *dev, VkDeviceMemory mem, const void *allocator) {
    (void)dev; (void)allocator;
    check_unlocked("Vulkan free runs outside cache mutex");
    uintptr_t i = (uintptr_t)mem;
    CHECK("allocation freed exactly once", i > 0 && i < 8 && !freed[i], "memory %p", mem);
    if (i > 0 && i < 8) freed[i]++;
    frees++;
}
static BOOL rdna4_imp_drain(void) {
    rdna4ic_rel o;
    BOOL reclaimed = NO;
    for (;;) {
        pthread_mutex_lock(&RDNA4IC.mu); int got = rdna4ic_pop(&RDNA4IC.c, completed, &o); pthread_mutex_unlock(&RDNA4IC.mu);
        if (!got) return reclaimed;
        vkFreeMemory(RDNA4R.dev, o.mem, NULL); RDNA4R.impTotal -= o.size; CFRelease(o.owner);
        reclaimed = YES;
    }
}
static VkResult rdna4_imp_get(IOSurfaceRef, void *, size_t, const VkMemoryAllocateInfo *, VkDeviceMemory *, BOOL *);
static VkResult rdna4_import_alloc(const VkMemoryAllocateInfo *ma, VkDeviceMemory *out) {
    check_unlocked("Vulkan allocation runs outside cache mutex");
    allocation_attempts++;
    if (import_budget && RDNA4R.impTotal + ma->allocationSize > import_budget) {
        *out = VK_NULL_HANDLE;
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    *out = M(++allocations);
    // A has missed; complete B's miss and admission before A can admit its allocation.
    if (interleave) {
        interleave = 0;
        VkResult r = rdna4_imp_get(&competing, M(0x1000), ma->allocationSize, ma, &competing_mem, &competing_hit);
        CHECK("interleaved wrapper imports successfully", r == VK_SUCCESS, "result %d", r);
        if (r == VK_SUCCESS && !competing_hit) RDNA4R.impTotal += ma->allocationSize;
        if (unref_competing) {
            pthread_mutex_lock(&RDNA4IC.mu); rdna4ic_unref(&RDNA4IC.c, competing_mem, 9, 7); pthread_mutex_unlock(&RDNA4IC.mu);
        }
    }
    return VK_SUCCESS;
}
#include RDNA4_IMP_GET_BODY
static void test_admission(int unused) {
    test_surface first = { 42, 0 }; competing = (test_surface){ 42, 0 };
    VkMemoryAllocateInfo ma = { MIB }; VkDeviceMemory mem = NULL, warm_mem = NULL; BOOL hit = NO, warm_hit = NO;
    allocations = frees = 0; memset(freed, 0, sizeof freed); RDNA4R.impTotal = 0;
    completed = 7;
    interleave = 1; unref_competing = unused; competing_hit = YES;
    rdna4ic_init(&RDNA4IC.c, 1);
    VkResult r = rdna4_imp_get(&first, M(0x1000), MIB, &ma, &mem, &hit);
    if (r == VK_SUCCESS && !hit) RDNA4R.impTotal += MIB;
    rdna4ic_cache *c = &RDNA4IC.c;
    CHECK("two initial misses admit one shared import", r == VK_SUCCESS && allocations == 2 && c->n == 1 && mem == competing_mem && hit && !competing_hit,
          "unused %d allocations %d entries %zu hit %d competitor-hit %d", unused, allocations, c->n, hit, competing_hit);
    CHECK("loser freed and only winner owner retained", frees == 1 && freed[1] == 1 && first.retains == 0 && competing.retains == 1,
          "frees %d owner refs %d/%d", frees, first.retains, competing.retains);
    CHECK("admission has balanced refs, bytes and caller hit accounting", c->n == 1 && c->e[0].refs == (unused ? 1 : 2) && c->created == 1 && c->reused == 1 && c->liveBytes == MIB && c->cachedBytes == 0 && RDNA4R.impTotal == MIB,
          "created %llu reused %llu imported %llu", (unsigned long long)c->created, (unsigned long long)c->reused, (unsigned long long)RDNA4R.impTotal);
    r = rdna4_imp_get(&first, M(0x1000), MIB, &ma, &warm_mem, &warm_hit);
    CHECK("warm hit does not allocate or retain another owner", r == VK_SUCCESS && warm_hit && warm_mem == competing_mem && allocations == 2 && first.retains == 0 && competing.retains == 1,
          "allocations %d warm hit %d", allocations, warm_hit);
    rdna4ic_unref(c, warm_mem, 11, 7); rdna4ic_unref(c, mem, 12, 7);
    if (!unused) rdna4ic_unref(c, competing_mem, 13, 7);
    CHECK("all wrappers released leave exactly one unused import", c->n == 1 && c->e[0].refs == 0 && c->cachedBytes == MIB && rdna4ic_unused(c) == 1, "cached %llu", (unsigned long long)c->cachedBytes);
    size_t flushed = rdna4ic_flush(c, 7); rdna4ic_rel o;
    CHECK("winner release remains fence gated", flushed == 1 && c->nrl == 1 && !rdna4ic_pop(c, 6, &o), "queued %zu", c->nrl);
    rdna4_imp_drain();
    CHECK("both allocations freed and all owner/byte accounting balanced", frees == 2 && freed[1] == 1 && freed[2] == 1 && first.retains == 0 && competing.retains == 0 && RDNA4R.impTotal == 0 && c->n == 0 && c->nrl == 0 && c->liveBytes == 0 && c->cachedBytes == 0,
          "frees %d owner refs %d/%d imported %llu entries %zu queued %zu", frees, first.retains, competing.retains, (unsigned long long)RDNA4R.impTotal, c->n, c->nrl);
    rdna4ic_destroy(c);
}

static void test_budget_recovery(int already_queued, uint64_t retired_serial) {
    test_surface previous = { 41, 1 }, next = { 42, 0 };
    VkMemoryAllocateInfo ma = { MIB };
    VkDeviceMemory mem = NULL;
    BOOL hit = YES;
    allocations = 1;
    frees = allocation_attempts = 0;
    memset(freed, 0, sizeof freed);
    RDNA4R.impTotal = import_budget = MIB;
    completed = retired_serial;
    rdna4ic_cache *cache = &RDNA4IC.c;
    rdna4ic_init(cache, 1);
    rdna4ic_add(cache, previous.id, 0x1000, MIB, M(1), &previous);
    rdna4ic_unref(cache, M(1), 1, 7);
    if (already_queued) rdna4ic_trim(cache, 3 * SEC, 7);

    VkResult result = rdna4_imp_get(&next, M(0x2000), MIB, &ma, &mem, &hit);
    if (retired_serial == 7) {
        CHECK("budget recovery reclaims retired import and retries once", result == VK_SUCCESS && mem == M(2) &&
              !hit && allocation_attempts == 2 && frees == 1 && previous.retains == 0 && cache->nrl == 0,
              "already queued %d result %d attempts %d frees %d queued %zu", already_queued, result, allocation_attempts, frees, cache->nrl);
    } else {
        CHECK("budget recovery preserves an import still in GPU use", result == VK_ERROR_OUT_OF_DEVICE_MEMORY &&
              !mem && !hit && allocation_attempts == 1 && frees == 0 && previous.retains == 1 && cache->nrl == 1,
              "result %d attempts %d frees %d queued %zu", result, allocation_attempts, frees, cache->nrl);
    }
    rdna4ic_destroy(cache);
}
#endif

int main(void) {
    rdna4ic_cache c; void *m; rdna4ic_rel o = {0};

#ifdef RDNA4_IMP_GET_BODY
    test_admission(0);
    test_admission(1);
    test_budget_recovery(1, 7);
    test_budget_recovery(0, 7);
    test_budget_recovery(1, 6);
#endif
    // A lookup must take just one reference even if duplicate entries already exist.
    rdna4ic_init(&c, 1);
    rdna4ic_add(&c, 10, 0x1000, MIB, M(1), M(11));
    rdna4ic_add(&c, 10, 0x1000, MIB, M(2), M(12));
    int duplicate_hit = rdna4ic_acquire(&c, 10, 0x1000, MIB, 1, 0, &m);
    CHECK("duplicate-key lookup takes one reference and records one hit", duplicate_hit && m == M(1) && c.e[0].refs == 2 && c.e[1].refs == 1 && c.reused == 1,
          "refs %d/%d reused %llu", c.e[0].refs, c.e[1].refs, (unsigned long long)c.reused);
    rdna4ic_unref(&c, m, 2, 0); rdna4ic_unref(&c, M(1), 3, 0); rdna4ic_unref(&c, M(2), 4, 0);
    CHECK("duplicate-key lookup leaves no phantom reference", rdna4ic_flush(&c, 7) == 2 && c.n == 0 && c.cachedBytes == 0 && c.liveBytes == 0 && c.nrl == 2,
          "entries %zu queued %zu", c.n, c.nrl);
    CHECK("both original imports retain their release owners", rdna4ic_pop(&c, 7, &o) && o.mem == M(1) && o.owner == M(11) && rdna4ic_pop(&c, 7, &o) && o.mem == M(2) && o.owner == M(12) && c.nrl == 0, "queue %zu", c.nrl);
    rdna4ic_destroy(&c);
    // ---- 1. refcount: one import shared by two live textures -----------------------------------------------------------------
    rdna4ic_init(&c, 1);
    CHECK("miss on an empty cache", !rdna4ic_acquire(&c, 10, 0x1000, 8 * MIB, 0, 0, &m), "miss");
    rdna4ic_add(&c, 10, 0x1000, 8 * MIB, M(0xA1), M(0xB1));
    CHECK("second texture of the same surface hits and gets the same memory", rdna4ic_acquire(&c, 10, 0x1000, 8 * MIB, 1, 0, &m) && m == M(0xA1) && c.reused == 1 && c.created == 1, "reused %llu created %llu", (unsigned long long)c.reused, (unsigned long long)c.created);
    rdna4ic_unref(&c, M(0xA1), 2, 0);
    CHECK("one texture still alive: entry in use, nothing cached or released", c.cachedBytes == 0 && c.nrl == 0 && c.e[0].refs == 1, "refs %d cached %llu", c.e[0].refs, (unsigned long long)c.cachedBytes);
    rdna4ic_unref(&c, M(0xA1), 3, 0);
    CHECK("last texture gone: kept (unused) rather than released", c.cachedBytes == 8 * MIB && c.nrl == 0 && c.n == 1 && c.e[0].refs == 0, "cached %llu released %zu", (unsigned long long)c.cachedBytes, c.nrl);

    // ---- 2. 2 s expiry ---------------------------------------------------------------------------------------------------------
    rdna4ic_trim(&c, 3 + 2 * SEC, 0);
    CHECK("exactly 2 s idle is not yet expired", c.nrl == 0 && c.n == 1, "released %zu", c.nrl);
    CHECK("the next frame's texture within 2 s reuses it (and the entry leaves the unused set)", rdna4ic_acquire(&c, 10, 0x1000, 8 * MIB, 3 + SEC, 0, &m) && c.cachedBytes == 0 && c.reused == 2, "cached %llu", (unsigned long long)c.cachedBytes);
    rdna4ic_unref(&c, M(0xA1), 3 + SEC, 0);
    rdna4ic_trim(&c, 3 + SEC + 2 * SEC + 1, 0);
    CHECK("unused > 2 s after the LAST unref: released", c.nrl == 1 && c.n == 0 && c.cachedBytes == 0 && c.liveBytes == 0 && c.evIdle == 1, "released %zu idle %llu", c.nrl, (unsigned long long)c.evIdle);
    CHECK("the release carries the entry's memory and owner", rdna4ic_pop(&c, 0, &o) && o.mem == M(0xA1) && o.owner == M(0xB1) && o.size == 8 * MIB, "mem %p owner %p", o.mem, o.owner);
    CHECK("expired entry no longer matches (new import needed)", !rdna4ic_acquire(&c, 10, 0x1000, 8 * MIB, 4 * SEC, 0, &m), "miss");
    rdna4ic_destroy(&c);

    // ---- 3. byte cap and eviction order ------------------------------------------------------------------------------------------
    rdna4ic_init(&c, 1);
    for (int i = 0; i < 4; i++) { rdna4ic_add(&c, 100 + i, 0x10000 * (i + 1), 40 * MIB, M(0x100 + i), M(0x200 + i)); }   // 160 MiB live
    rdna4ic_unref(&c, M(0x100), 10, 0); rdna4ic_unref(&c, M(0x101), 20, 0); rdna4ic_unref(&c, M(0x102), 30, 0);   // 120 MiB cached: under the cap
    CHECK("120 MiB unused <= 128 MiB cap: nothing evicted", c.cachedBytes == 120 * MIB && c.nrl == 0, "cached %llu", (unsigned long long)c.cachedBytes);
    rdna4ic_unref(&c, M(0x103), 40, 0);   // 160 MiB > cap: oldest first
    CHECK("160 MiB unused > cap: oldest unused (0x100) evicted first, then stop under the cap", c.nrl == 1 && c.rl[0].mem == M(0x100) && c.cachedBytes == 120 * MIB && c.evCap == 1, "released %zu first %p cached %llu", c.nrl, c.rl[0].mem, (unsigned long long)c.cachedBytes);
    CHECK("the evicted one no longer hits, the others do", !rdna4ic_acquire(&c, 100, 0x10000, 40 * MIB, 41, 0, &m) && rdna4ic_acquire(&c, 101, 0x20000, 40 * MIB, 41, 0, &m) && m == M(0x101), "ok");
    rdna4ic_unref(&c, M(0x101), 42, 0);   // refreshed: now newest; unused = 102(30) 103(40) 101(42) = 120
    rdna4ic_add(&c, 104, 0x50000, 40 * MIB, M(0x104), M(0x204)); rdna4ic_unref(&c, M(0x104), 50, 0);   // 160 -> evict oldest = 0x102 (t30)
    CHECK("eviction follows lastUnref (0x102 at t30 goes before 0x103 at t40 and the refreshed 0x101)", c.nrl == 2 && c.rl[1].mem == M(0x102), "second released %p", c.rl[1].mem);
    rdna4ic_destroy(&c);
    rdna4ic_init(&c, 1);
    rdna4ic_add(&c, 1, 0x1000, 40 * MIB, M(1), M(1)); rdna4ic_add(&c, 2, 0x2000, 40 * MIB, M(2), M(2)); rdna4ic_add(&c, 3, 0x3000, 40 * MIB, M(3), M(3)); rdna4ic_add(&c, 4, 0x4000, 40 * MIB, M(4), M(4));
    rdna4ic_unref(&c, M(1), 1, 0); rdna4ic_unref(&c, M(2), 2, 0); rdna4ic_unref(&c, M(3), 3, 0); rdna4ic_unref(&c, M(4), 4, 0);
    CHECK("a LIVE entry is never evicted by the cap (only unused ones)", c.nrl == 1 && c.rl[0].mem == M(1) && c.liveBytes == 120 * MIB, "released %zu live %llu", c.nrl, (unsigned long long)c.liveBytes);
    rdna4ic_destroy(&c);

    // ---- 4. id reuse with another key ----------------------------------------------------------------------------------------------
    rdna4ic_init(&c, 1);
    rdna4ic_add(&c, 7, 0x1000, 8 * MIB, M(0x71), M(0x72)); rdna4ic_unref(&c, M(0x71), 5, 0);
    CHECK("same id, different size: unused entry released, miss", !rdna4ic_acquire(&c, 7, 0x1000, 16 * MIB, 6, 0, &m) && c.nrl == 1 && c.mismatches == 1 && c.n == 0, "released %zu mismatches %llu", c.nrl, (unsigned long long)c.mismatches);
    rdna4ic_add(&c, 7, 0x1000, 16 * MIB, M(0x81), M(0x82));   // live (refs 1)
    CHECK("same id, different base while the old is LIVE: old goes stale (still held), miss", !rdna4ic_acquire(&c, 7, 0x9000, 16 * MIB, 7, 0, &m) && c.n == 1 && c.e[0].stale && c.nrl == 1, "stale %d", c.e[0].stale);
    CHECK("a stale entry never matches even with its own key", !rdna4ic_acquire(&c, 7, 0x1000, 16 * MIB, 8, 0, &m), "miss");
    rdna4ic_unref(&c, M(0x81), 9, 0);
    CHECK("the stale entry is released the moment its last texture lets go (not cached)", c.n == 0 && c.nrl == 2 && c.cachedBytes == 0 && c.evStale == 2, "n %zu released %zu", c.n, c.nrl);
    rdna4ic_destroy(&c);

    // ---- 5. fence gating of the release queue ------------------------------------------------------------------------------------------
    rdna4ic_init(&c, 1);
    rdna4ic_add(&c, 5, 0x1000, 4 * MIB, M(0x51), M(0x52)); rdna4ic_unref(&c, M(0x51), 1, 0);
    rdna4ic_trim(&c, 1 + 3 * SEC, 7);   // given up while serial 7 was the newest submitted
    CHECK("a release stamped 7 is not handed out while completed is 6", c.nrl == 1 && !rdna4ic_pop(&c, 6, &o), "held back");
    CHECK("... and is handed out once completed reaches 7", rdna4ic_pop(&c, 7, &o) && o.mem == M(0x51), "popped");
    rdna4ic_destroy(&c);

    // ---- 6. flush (out of kernel imports) ------------------------------------------------------------------------------------------
    rdna4ic_init(&c, 1);
    rdna4ic_add(&c, 1, 0x1000, MIB, M(1), M(1)); rdna4ic_add(&c, 2, 0x2000, MIB, M(2), M(2)); rdna4ic_unref(&c, M(1), 1, 0);
    CHECK("flush releases the unused entry only", rdna4ic_flush(&c, 0) == 1 && c.nrl == 1 && c.n == 1 && c.e[0].mem == M(2) && c.evFlush == 1, "n %zu", c.n);
    rdna4ic_destroy(&c);

    // ---- 7. OFF path -------------------------------------------------------------------------------------------------------------------
    rdna4ic_init(&c, 0);
    rdna4ic_trim(&c, 100 * SEC, 0);
    CHECK("OFF: trim and flush are no-ops, nothing is ever queued", rdna4ic_flush(&c, 0) == 0 && c.nrl == 0 && c.n == 0 && c.on == 0, "off");
    rdna4ic_destroy(&c);

    // ---- 8. classify cache -------------------------------------------------------------------------------------------------------------
    static rdna4cc cc; int v = -1;
    memset(&cc, 0, sizeof cc);
    CHECK("classify: first sight misses", !rdna4cc_lookup(&cc, 50, 0x1000, 2560, 1440, 10240, 14745600, &v) && cc.misses == 1, "miss");
    rdna4cc_put(&cc, 50, 0x1000, 2560, 1440, 10240, 14745600, 1);
    rdna4cc_put(&cc, 51, 0x2000, 2560, 1440, 10240, 14745600, 0);
    CHECK("classify: positive verdict cached", rdna4cc_lookup(&cc, 50, 0x1000, 2560, 1440, 10240, 14745600, &v) && v == 1, "v %d", v);
    CHECK("classify: negative verdict cached", rdna4cc_lookup(&cc, 51, 0x2000, 2560, 1440, 10240, 14745600, &v) && v == 0, "v %d", v);
    CHECK("classify: hits counted", cc.hits == 2 && cc.misses == 1, "hits %llu misses %llu", (unsigned long long)cc.hits, (unsigned long long)cc.misses);
    CHECK("classify: same id, other size -> mismatch, miss, entry dropped", !rdna4cc_lookup(&cc, 50, 0x1000, 2560, 1600, 10240, 16384000, &v) && cc.mismatches == 1, "mismatch %llu", (unsigned long long)cc.mismatches);
    CHECK("classify: ... and the old key no longer hits (it was invalidated)", !rdna4cc_lookup(&cc, 50, 0x1000, 2560, 1440, 10240, 14745600, &v), "miss");
    rdna4cc_put(&cc, 50, 0x1000, 2560, 1600, 10240, 16384000, 0);
    CHECK("classify: same id, other base -> mismatch", !rdna4cc_lookup(&cc, 50, 0x7000, 2560, 1600, 10240, 16384000, &v) && cc.mismatches == 2, "mismatch %llu", (unsigned long long)cc.mismatches);
    CHECK("classify: same id, other pitch -> mismatch", (rdna4cc_put(&cc, 52, 0x1000, 1024, 768, 4096, 3145728, 1), !rdna4cc_lookup(&cc, 52, 0x1000, 1024, 768, 4352, 3145728, &v)) && cc.mismatches == 3, "mismatch %llu", (unsigned long long)cc.mismatches);
    memset(&cc, 0, sizeof cc);
    rdna4cc_put(&cc, 60, 0x1000, 2560, 1440, 10240, 14745600, 1);
    CHECK("classify: same id, only the alloc size differs -> mismatch", !rdna4cc_lookup(&cc, 60, 0x1000, 2560, 1440, 10240, 14745600 + 16384, &v) && cc.mismatches == 1, "mismatch %llu", (unsigned long long)cc.mismatches);
    memset(&cc, 0, sizeof cc);
    for (uint32_t i = 0; i < RDNA4CC_N; i++) rdna4cc_put(&cc, 1000 + i, 0x1000, 1024, 768, 4096, 3145728, (int)(i & 1));
    rdna4cc_lookup(&cc, 1000, 0x1000, 1024, 768, 4096, 3145728, &v);   // id 1000 becomes the most recently used
    rdna4cc_put(&cc, 2000, 0x1000, 1024, 768, 4096, 3145728, 1);       // 65th id: evicts the least recently used = 1001
    CHECK("classify: bounded to 64 ids, LRU evicted (1001 gone, the touched 1000 and the new 2000 stay)",
          !rdna4cc_lookup(&cc, 1001, 0x1000, 1024, 768, 4096, 3145728, &v) && rdna4cc_lookup(&cc, 1000, 0x1000, 1024, 768, 4096, 3145728, &v) && rdna4cc_lookup(&cc, 2000, 0x1000, 1024, 768, 4096, 3145728, &v) && rdna4cc_lookup(&cc, 1063, 0x1000, 1024, 768, 4096, 3145728, &v), "ok");

    // ---- 4. build 16: F2 default, F1 decision, the failure-injection hook -------------------------------------------------------------
    CHECK("F2: the cache is ON by default (no kill file)", rdna4ic_default_on(0) == 1, "on");
    CHECK("F2: the kill file turns it off", rdna4ic_default_on(1) == 0, "off");
    {   rdna4ic_cache d; rdna4ic_init(&d, rdna4ic_default_on(0)); void *mm;
        rdna4ic_add(&d, 5, 0x1000, MIB, M(0xC1), M(0xD1));
        CHECK("F2: a default-initialised cache shares the import of one surface between two wrappers", d.on && rdna4ic_acquire(&d, 5, 0x1000, MIB, 1, 0, &mm) && mm == M(0xC1) && d.reused == 1, "on %d reused %llu", d.on, (unsigned long long)d.reused);
        rdna4ic_cache o; rdna4ic_init(&o, rdna4ic_default_on(1));
        CHECK("F2: with the kill file the cache is off (the glue then imports per wrapper as before) and trim/flush do nothing", o.on == 0 && rdna4ic_flush(&o, 0) == 0, "on %d", o.on);
        rdna4ic_destroy(&d); rdna4ic_destroy(&o);
    }
    CHECK("F1: an imported texture goes on", rdna4f1_action(0) == RDNA4F1_GO, "go");
    CHECK("F1: ANY refused import falls back to the GPU-only image, never to nil", rdna4f1_action(-2) == RDNA4F1_FALLBACK && rdna4f1_action(-1) == RDNA4F1_FALLBACK && rdna4f1_action(-1000072003) == RDNA4F1_FALLBACK, "fallback");
    { unsigned long c = 0; int hits = 0; for (int i = 0; i < 100; i++) hits += rdna4f1_inject(4, &c);
      CHECK("F1 hook: every 4th attempt fails (25 of 100), 0 = never", hits == 25 && (c = 0, rdna4f1_inject(0, &c) == 0 && c == 0), "hits %d", hits); }

    // ---- 9. per-process cap (128 MiB app default vs 512 MiB WindowServer) --------------------------------------------------------------
    CHECK("cap policy: non-WindowServer stays 128 MiB, WindowServer gets 512 MiB",
          rdna4ic_cap_for_process(0) == 128 * MIB && rdna4ic_cap_for_process(1) == 512 * MIB,
          "app %llu MiB ws %llu MiB", (unsigned long long)(rdna4ic_cap_for_process(0) >> 20), (unsigned long long)(rdna4ic_cap_for_process(1) >> 20));
    {
        // Zero-init and set_cap(0) both fall back safely to the 128 MiB default cap.
        rdna4ic_cache z;
        memset(&z, 0, sizeof z);
        z.on = 1;
        for (uint32_t i = 0; i < 4; i++) {
            rdna4ic_add(&z, 300 + i, 0x10000ULL * (i + 1), 40 * MIB, M(0x300 + i), M(0x380 + i));
            rdna4ic_unref(&z, M(0x300 + i), 10 + i, 0);
        }
        CHECK("zero-init cache enforces 128 MiB fallback cap", z.evCap == 1 && z.cachedBytes == 120 * MIB && z.nrl == 1 && z.rl[0].mem == M(0x300),
              "evCap %llu cached %llu MiB", (unsigned long long)z.evCap, (unsigned long long)(z.cachedBytes >> 20));
        rdna4ic_set_cap(&z, 0);
        CHECK("rdna4ic_set_cap(0) keeps 128 MiB default cap", z.capBytes == 128 * MIB, "cap %llu MiB", (unsigned long long)(z.capBytes >> 20));
        rdna4ic_destroy(&z);
    }
    {
        // WindowServer holds >128 MiB (208 MiB: 160 MiB desktop windows + 3x16 MiB rotating swapchain) with 0 cap evictions and 100% reuse.
        rdna4ic_cache ws;
        rdna4ic_init(&ws, 1);
        rdna4ic_set_cap(&ws, rdna4ic_cap_for_process(1));
        for (uint32_t i = 0; i < 4; i++) {
            rdna4ic_add(&ws, 400 + i, 0x10000ULL * (i + 1), 40 * MIB, M(0x400 + i), M(0x440 + i));
            rdna4ic_unref(&ws, M(0x400 + i), 1000 + i, 0);
        }
        for (uint32_t i = 0; i < 3; i++) {
            rdna4ic_add(&ws, 500 + i, 0x100000ULL * (i + 1), 16 * MIB, M(0x500 + i), M(0x540 + i));
            rdna4ic_unref(&ws, M(0x500 + i), 2000 + i, 0);
        }
        int swapHits = 0;
        uint64_t t = 10000;
        for (int f = 0; f < 30; f++) {
            uint32_t slot = (uint32_t)(f % 3);
            void *hitMem = NULL;
            if (rdna4ic_acquire(&ws, 500 + slot, 0x100000ULL * (slot + 1), 16 * MIB, t, 0, &hitMem) && hitMem == M(0x500 + slot)) {
                swapHits++;
                rdna4ic_unref(&ws, hitMem, t + 1000000ULL, 0);
            }
            t += 6250000ULL;   // ~160 Hz frame spacing
        }
        CHECK("WindowServer cache holds 208 MiB rotating swapchain with 0 cap evictions and 100% reuse",
              ws.capBytes == 512 * MIB && ws.cachedBytes == 208 * MIB && ws.evCap == 0 && ws.nrl == 0 && swapHits == 30 && ws.reused == 30,
              "cached %llu MiB evCap %llu hits %d reused %llu", (unsigned long long)(ws.cachedBytes >> 20), (unsigned long long)ws.evCap, swapHits, (unsigned long long)ws.reused);

        // Idle trim (> RDNA4IC_IDLE_NS) still reclaims idle desktop entries; flush reclaims the rest.
        rdna4ic_trim(&ws, 1003 + RDNA4IC_IDLE_NS + 1, 7);
        CHECK("WindowServer cache still reclaims idle entries after RDNA4IC_IDLE_NS",
              ws.evIdle == 4 && ws.cachedBytes == 48 * MIB && ws.n == 3 && ws.nrl == 4,
              "evIdle %llu cached %llu MiB n %zu queued %zu", (unsigned long long)ws.evIdle, (unsigned long long)(ws.cachedBytes >> 20), ws.n, ws.nrl);
        size_t flushed = rdna4ic_flush(&ws, 8);
        CHECK("WindowServer cache still reclaims all unused entries on rdna4ic_flush",
              flushed == 3 && ws.evFlush == 3 && ws.cachedBytes == 0 && ws.liveBytes == 0 && ws.n == 0 && ws.nrl == 7,
              "flushed %zu cached %llu live %llu queued %zu", flushed, (unsigned long long)ws.cachedBytes, (unsigned long long)ws.liveBytes, ws.nrl);
        rdna4ic_destroy(&ws);
    }
    {
        // WindowServer cache still enforces the 512 MiB cap, evicting oldest unused first.
        rdna4ic_cache ws;
        rdna4ic_init(&ws, 1);
        rdna4ic_set_cap(&ws, rdna4ic_cap_for_process(1));
        for (uint32_t i = 0; i < 8; i++) {
            rdna4ic_add(&ws, 600 + i, 0x20000ULL * (i + 1), 64 * MIB, M(0x600 + i), M(0x680 + i));
            rdna4ic_unref(&ws, M(0x600 + i), 100 + i * 10, 0);
        }
        CHECK("WindowServer cache holds exactly 512 MiB unused with 0 evictions",
              ws.cachedBytes == 512 * MIB && ws.evCap == 0 && ws.nrl == 0,
              "cached %llu MiB evCap %llu", (unsigned long long)(ws.cachedBytes >> 20), (unsigned long long)ws.evCap);
        rdna4ic_add(&ws, 608, 0x20000ULL * 9, 64 * MIB, M(0x608), M(0x688));
        rdna4ic_unref(&ws, M(0x608), 200, 0);
        CHECK("WindowServer cache > 512 MiB evicts oldest unused (0x600) first",
              ws.cachedBytes == 512 * MIB && ws.evCap == 1 && ws.nrl == 1 && ws.rl[0].mem == M(0x600),
              "cached %llu MiB evCap %llu evicted %p", (unsigned long long)(ws.cachedBytes >> 20), (unsigned long long)ws.evCap, ws.nrl ? ws.rl[0].mem : NULL);
        CHECK("WindowServer evicted entry misses while surviving entries still hit",
              !rdna4ic_acquire(&ws, 600, 0x20000ULL, 64 * MIB, 201, 0, &m) &&
              rdna4ic_acquire(&ws, 601, 0x40000ULL, 64 * MIB, 201, 0, &m) && m == M(0x601),
              "hit %p", m);
        rdna4ic_destroy(&ws);
    }

    printf("%s (%d failure%s)\n", fails ? "FAILED" : "ALL PASSED", fails, fails == 1 ? "" : "s");
    return fails != 0;
}
