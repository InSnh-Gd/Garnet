// test-host-memory.c: consolidated host C11 tests for GarnetMetal memory policies:
// nocopy range/protection/readonly, 64-bit buffer ranges & registry, heap offset &
// backing allocators, memory pooling, dual-BAR policy + linear cache, and VRAM surface cache.
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_refuse_realloc;
static void *test_mem_realloc(void *ptr, size_t bytes) {
    return g_refuse_realloc ? NULL : realloc(ptr, bytes);
}
#define realloc test_mem_realloc
#include "garnet_heapalloc.h"
#include "garnet_pool.h"
#undef realloc

#include "garnet_nocopy.h"
#include "garnet_buffer_ranges.h"
#include "garnet_buffer_registry.h"
#include "garnet_bar_policy.h"
#include "garnet_impcache.h"
#include "garnet_vram_surf.h"
#include "garnet_heap_backing.h"

static unsigned g_checks, g_fails;

#define CHECK(name, cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_fails++; \
        fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, (name)); \
    } \
} while (0)

#define KIB (1ULL << 10)
#define MIB (1ULL << 20)
#define GIB (1ULL << 30)
#define SEC 1000000000ULL

static void nc_range_ok(uint64_t p, uint64_t len, uint64_t align,
                        uint64_t wb, uint64_t ws, uint64_t wd) {
    uint64_t base = 0, span = 0, delta = 0;
    CHECK("nc range ok", rdna4nc_import_range(p, len, align, &base, &span, &delta) == 0 &&
          base == wb && span == ws && delta == wd);
}

static void nc_range_bad(uint64_t p, uint64_t len, uint64_t align) {
    uint64_t base = 17, span = 19, delta = 23;
    CHECK("nc range bad", rdna4nc_import_range(p, len, align, &base, &span, &delta) != 0 &&
          base == 17 && span == 19 && delta == 23);
}

static void suite_nocopy(void) {
    char text[4];
    rdna4nc_protection_text(0, text); CHECK("nc prot ---", strcmp(text, "---") == 0);
    rdna4nc_protection_text(RDNA4NC_PROT_READ, text); CHECK("nc prot r--", strcmp(text, "r--") == 0);
    rdna4nc_protection_text(RDNA4NC_PROT_READ | RDNA4NC_PROT_WRITE, text); CHECK("nc prot rw-", strcmp(text, "rw-") == 0);
    rdna4nc_protection_text(RDNA4NC_PROT_READ | RDNA4NC_PROT_WRITE | RDNA4NC_PROT_EXEC, text); CHECK("nc prot rwx", strcmp(text, "rwx") == 0);

    nc_range_ok(0x10000, 65536, 4096, 0x10000, 65536, 0);
    nc_range_ok(0x10001, 65536, 4096, 0x10000, 69632, 1);
    nc_range_ok(0x10000, 65535, 4096, 0x10000, 65536, 0);
    nc_range_ok(0x10fff, 2, 4096, 0x10000, 8192, 4095);
    nc_range_ok(0x14001, 16384, 16384, 0x14000, 32768, 1);
    nc_range_ok(7, 3, 1, 7, 3, 0);
    nc_range_ok(UINT64_MAX - 8191, 4096, 4096, UINT64_MAX - 8191, 4096, 0);
    nc_range_ok(0, 1, UINT64_C(1) << 63, 0, UINT64_C(1) << 63, 0);
    nc_range_bad(UINT64_MAX - 1, 2, 4096);
    nc_range_bad(UINT64_MAX - 1, 1, 4096);
    nc_range_bad(UINT64_MAX - 4095, 4095, 4096);
    nc_range_bad(4096, 0, 4096);
    nc_range_bad(4096, 1, 0);
    nc_range_bad(4096, 1, 3);

    struct rdna4nc_region regions[] = {
        { 0x1000, 0x1000, RDNA4NC_PROT_READ | RDNA4NC_PROT_WRITE },
        { 0x2000, 0x1000, RDNA4NC_PROT_READ | RDNA4NC_PROT_WRITE },
        { 0x3000, 0x1000, RDNA4NC_PROT_READ | RDNA4NC_PROT_WRITE }
    };
    CHECK("nc ro all rw", rdna4nc_range_readonly(regions, 3, 0x1000, 0x3000) == 0);
    regions[1].prot = RDNA4NC_PROT_READ;
    CHECK("nc ro middle ro",
          rdna4nc_range_readonly(regions, 3, 0x1000, 0x3000) == 1 &&
          rdna4nc_range_readonly(regions, 3, 0x1800, 0x1000) == 1 &&
          rdna4nc_range_readonly(regions, 3, 0x1000, 0x1000) == 0 &&
          rdna4nc_range_readonly(regions, 3, 0x2000, 0x1000) == 1 &&
          rdna4nc_range_readonly(regions, 3, 0x3000, 0x1000) == 0 &&
          rdna4nc_range_readonly(regions, 3, 0x4000, 0) == 0 &&
          rdna4nc_range_readonly(regions, 3, 0, 0) == 0 &&
          rdna4nc_range_readonly(regions, 3, 0, 0x2000) == -1 &&
          rdna4nc_range_readonly(regions, 3, 0x1800, 0x3000) == -1 &&
          rdna4nc_range_readonly(regions, 3, 0x4000, 1) == -1 &&
          rdna4nc_range_readonly(regions, 3, UINT64_MAX, 1) == -1);
    regions[1].start = 0x2800; regions[1].size = 0x800;
    CHECK("nc ro gap & null",
          rdna4nc_range_readonly(regions, 3, 0x1000, 0x3000) == -1 &&
          rdna4nc_range_readonly(NULL, 0, 1, 1) == -1);
}

static void suite_buffer_ranges_and_registry(void) {
    uint64_t offset = 0, size = 0;
    CHECK("buf layout", sizeof(rdna4_buffer_table) == 768 &&
          offsetof(rdna4_buffer_table, length) == 256 && offsetof(rdna4_buffer_table, access) == 512);
    CHECK("buf range",
          rdna4_buffer_range(8 * GIB, 4 * GIB - 4, 8) && rdna4_buffer_range(8 * GIB, 8 * GIB, 0) &&
          !rdna4_buffer_range(8 * GIB, 8 * GIB, 1) && !rdna4_buffer_range(8 * GIB, UINT64_MAX, 1) &&
          !rdna4_buffer_range(UINT64_MAX, UINT64_MAX - 1, 2));
    CHECK("buf view 1", rdna4_buffer_view(8 * GIB, 4 * GIB + 256, 4096, 256, UINT32_MAX, &offset, &size) &&
          offset == 4 * GIB + 256 + 4096 && size == 4 * GIB - 256);
    CHECK("buf view 2", rdna4_buffer_view(8 * GIB, 0, 0, 256, 65536, &offset, &size) && offset == 0 && size == 65536);
    CHECK("buf view 3", rdna4_buffer_view(8 * GIB, 8 * GIB - 4, 0, 4, 65536, &offset, &size) && offset == 8 * GIB - 4 && size == 4);
    CHECK("buf view invalid",
          !rdna4_buffer_view(8 * GIB, 8 * GIB, 0, 4, 65536, &offset, &size) &&
          !rdna4_buffer_view(8 * GIB, 1, 0, 256, 65536, &offset, &size) &&
          !rdna4_buffer_view(8 * GIB, 0, 0, 4, 0, &offset, &size) &&
          !rdna4_buffer_view(8 * GIB, 4 * GIB, UINT64_MAX, 1, 65536, &offset, &size) &&
          !rdna4_buffer_view(8 * GIB, 0, UINT64_MAX - GIB, 1, 2 * GIB, &offset, &size));
    CHECK("buf view small", rdna4_buffer_view(128, 16, 4096, 16, 65536, &offset, &size) && offset == 4112 && size == 112);

    rdna4_buffer_table table = {0};
    CHECK("buf table set ro", rdna4_buffer_table_set(&table, 7, 32 * GIB, 8 * GIB, 4 * GIB, 1) &&
          table.address[7] == 36 * GIB && table.length[7] == 4 * GIB && table.access[7] == RDNA4_BUFFER_READ && table.address[6] == 0);
    CHECK("buf table set rw", rdna4_buffer_table_set(&table, 7, 32 * GIB, 8 * GIB, 0, 0) &&
          table.access[7] == (RDNA4_BUFFER_READ | RDNA4_BUFFER_WRITE));
    rdna4_buffer_table saved = table;
    CHECK("buf table reject",
          !rdna4_buffer_table_set(&table, 31, 1, 1, 0, 0) && !rdna4_buffer_table_set(&table, 7, 0, 1, 0, 0) &&
          !rdna4_buffer_table_set(&table, 7, 1, 1, 1, 0) && !rdna4_buffer_table_set(&table, 7, UINT64_MAX - 1, 2, 0, 0) &&
          table.address[7] == saved.address[7] && table.length[7] == saved.length[7] && table.access[7] == saved.access[7]);

    size_t bytes = 0;
    CHECK("reg size",
          rdna4_buffer_registry_size(257, 6968, &bytes) && bytes == 6968 &&
          !rdna4_buffer_registry_size(257, 6967, &bytes) &&
          !rdna4_buffer_registry_size(UINT64_MAX, UINT64_MAX, &bytes) &&
          !rdna4_buffer_registry_size((UINT64_MAX - 800) / 24, UINT64_MAX, &bytes) &&
          !rdna4_buffer_registry_size(0, 799, &bytes) &&
          rdna4_buffer_registry_size(0, 800, &bytes) && bytes == 800);
    rdna4_buffer_table direct = {0};
    assert(rdna4_buffer_table_set(&direct, 4, UINT64_C(0x1800000000), UINT64_C(0x200000000), 256, 1));
    assert(rdna4_buffer_registry_size(257, UINT32_MAX, &bytes));
    uint8_t *reg = malloc(bytes);
    assert(reg && rdna4_buffer_registry_init(reg, bytes, &direct, 257, UINT32_MAX));
    CHECK("reg header", !memcmp(reg, &direct, sizeof direct));
    uint64_t header[4]; memcpy(header, reg + 768, sizeof header);
    CHECK("reg counts", header[0] == 257 && header[1] == 24 && header[2] == bytes && header[3] == 0);
    for (unsigned i = 0; i < 257; ++i)
        assert(rdna4_buffer_registry_set(reg, bytes, i, UINT64_C(0x1800000000) + i, UINT64_C(0x200000000), i & 1));
    rdna4_buffer_registry_row row; memcpy(&row, reg + 800 + 255 * 24, sizeof row);
    CHECK("reg row 255", row.base == UINT64_C(0x18000000ff) && row.length == UINT64_C(0x200000000) && row.access == RDNA4_BUFFER_READ);
    CHECK("reg bounds rejects",
          !rdna4_buffer_registry_set(reg, bytes, 257, 1, 1, 0) &&
          !rdna4_buffer_registry_set(reg, bytes, 0, UINT64_MAX - 1, 2, 0) &&
          !rdna4_buffer_registry_set(reg, bytes, 0, 0, 1, 0) &&
          !rdna4_buffer_registry_set(reg, bytes, 0, 1, 0, 0));
    header[3] = 1; memcpy(reg + 768, header, sizeof header);
    CHECK("reg bad flags", !rdna4_buffer_registry_set(reg, bytes, 0, 1, 1, 0));
    header[3] = 0; header[0] = UINT64_MAX; memcpy(reg + 768, header, sizeof header);
    CHECK("reg huge count", !rdna4_buffer_registry_set(reg, bytes, 0, 1, 1, 0));
    free(reg);
}

static void suite_heapalloc(void) {
    uint64_t off, id, id2, id3;
    RDNA4HeapAlloc h; rdna4ha_init(&h, 64 * MIB);
    CHECK("ha empty", rdna4ha_maxavail(&h, 256) == 64 * MIB && rdna4ha_maxavail(&h, 16384) == 64 * MIB);
    CHECK("ha alloc 1000", rdna4ha_alloc(&h, 1000, 256, &off, &id) && off == 0 && h.used == 1000);
    CHECK("ha max after 1000", rdna4ha_maxavail(&h, 256) == 67107840 && rdna4ha_maxavail(&h, 16384) == 67092480);
    CHECK("ha alloc tex", rdna4ha_alloc(&h, 264192, 128, &off, &id2) && off == 1024 && h.used == 265192);
    rdna4ha_free(&h, id2); CHECK("ha free tex", h.used == 1000);
    rdna4ha_free(&h, id2); CHECK("ha double free", h.used == 1000);
    rdna4ha_free(&h, id); CHECK("ha free buf", h.used == 0 && rdna4ha_maxavail(&h, 256) == 64 * MIB);
    CHECK("ha >heap", !rdna4ha_alloc(&h, 64 * MIB + 1, 256, &off, &id));
    CHECK("ha exact fit", rdna4ha_alloc(&h, 64 * MIB, 256, &off, &id) && h.used == 64 * MIB && rdna4ha_maxavail(&h, 16) == 0);
    CHECK("ha full", !rdna4ha_alloc(&h, 16, 256, &off, &id2));
    rdna4ha_free(&h, id); CHECK("ha reuse", h.used == 0 && rdna4ha_alloc(&h, 4096, 256, &off, &id)); rdna4ha_free(&h, id);
    rdna4ha_alloc(&h, 1000, 256, &off, &id); rdna4ha_alloc(&h, 1000, 256, &off, &id2);
    CHECK("ha c at 1024", off == 1024 && h.used == 2000 && rdna4ha_maxavail(&h, 256) == 67106816);
    rdna4ha_free(&h, id); CHECK("ha after free a", h.used == 1000 && rdna4ha_maxavail(&h, 256) == 67106816);
    CHECK("ha no gap", !rdna4ha_alloc(&h, 64 * MIB - 1000, 256, &off, &id3));
    CHECK("ha 60M fits", rdna4ha_alloc(&h, 60 * MIB, 256, &off, &id3) && rdna4ha_maxavail(&h, 256) == 4192256);
    CHECK("ha hole reused", rdna4ha_alloc(&h, 1000, 256, &off, &id) && off == 0);
    rdna4ha_destroy(&h);

    rdna4ha_init(&h, 16384); rdna4ha_alloc(&h, 100, 256, &off, &id);
    CHECK("ha 16K heap", h.used == 100 && rdna4ha_maxavail(&h, 16) == 16272 && rdna4ha_maxavail(&h, 256) == 16128);
    rdna4ha_destroy(&h);

    rdna4ha_init(&h, 64 * MIB); srand(7); uint64_t ids[32] = {0}, sizes[32] = {0}, sum = 0; int bad = 0;
    for (int i = 0; i < 100000; i++) {
        int k = rand() % 32;
        if (ids[k]) { rdna4ha_free(&h, ids[k]); sum -= sizes[k]; ids[k] = 0; }
        else { uint64_t sz = 1 + (uint64_t)rand() % 3000000, al = (uint64_t)256 << (rand() % 3);
               if (rdna4ha_alloc(&h, sz, al, &off, &ids[k])) { sizes[k] = sz; sum += sz; if (off % al) bad++; } }
        if (h.used != sum) bad++;
        for (size_t j = 1; j < h.nblk; j++) if (h.blk[j].off < h.blk[j - 1].off + h.blk[j - 1].size) bad++;
        if (h.nblk && h.blk[h.nblk - 1].off + h.blk[h.nblk - 1].size > h.size) bad++;
    }
    CHECK("ha 100k churn", bad == 0);
    rdna4ha_destroy(&h);

    rdna4ha_init(&h, 16384);
    CHECK("ha granularity extent",
          rdna4ha_alloc_ext(&h, 100, 4096, 4096, &off, &id) && h.used == 100 &&
          rdna4ha_alloc_ext(&h, 100, 4096, 4096, &off, &id2) && off == 4096);
    rdna4ha_free(&h, id); rdna4ha_free(&h, id2); rdna4ha_destroy(&h);

    rdna4ha_init(&h, UINT64_MAX);
    CHECK("ha invalid & overflow",
          !rdna4ha_alloc(&h, 0, 256, &off, &id) && !rdna4ha_alloc(&h, 1, 0, &off, &id) && !rdna4ha_alloc(&h, 1, 3, &off, &id) &&
          rdna4ha_alup(UINT64_MAX - 1, 4) == UINT64_MAX && rdna4ha_maxavail(&h, UINT64_MAX) == 0);
    g_refuse_realloc = 1;
    CHECK("ha init OOM", !rdna4ha_alloc(&h, 1, 1, &off, &id) && !h.blk && !h.cap && !h.nblk && !h.used && !h.ctr);
    g_refuse_realloc = 0;
    for (unsigned i = 0; i < 64; i++) assert(rdna4ha_alloc(&h, 1, 1, &off, &id));
    RDNA4HeapBlk *blocks = h.blk;
    g_refuse_realloc = 1;
    CHECK("ha growth OOM", !rdna4ha_alloc(&h, 1, 1, &off, &id) && h.blk == blocks && h.cap == 64 && h.nblk == 64 && h.used == 64 && h.ctr == 64);
    g_refuse_realloc = 0;
    h.ctr = UINT64_MAX;
    CHECK("ha id exhaustion", !rdna4ha_alloc(&h, 1, 1, &off, &id) && h.nblk == 64);
    rdna4ha_destroy(&h);
}

static uint64_t pool_fill_slab(rdna4p_pool *p, uint32_t key, uint64_t t, uint64_t off[16]) {
    rdna4p_res r; uint64_t sid = 0;
    int k = rdna4p_alloc(p, key, 256 * KIB, 256, t, &r);
    if (k == RDNA4P_R_NEED_SLAB) {
        sid = rdna4p_add_slab(p, key, RDNA4P_SLAB_SIZE, (void *)0x1000, (void *)0x2000, t);
        k = rdna4p_alloc(p, key, 256 * KIB, 256, t, &r);
    }
    if (k != RDNA4P_R_SUB) return 0;
    sid = r.slab; off[0] = r.off;
    for (int i = 1; i < 16; i++) {
        k = rdna4p_alloc(p, key, 256 * KIB, 256, t, &r);
        if (k != RDNA4P_R_SUB || r.slab != sid) return 0;
        off[i] = r.off;
    }
    return sid;
}

static void suite_pool(void) {
    rdna4p_pool p; rdna4p_res r; rdna4p_rel rel; uint64_t off[16];
    rdna4p_init(&p, 1);
    CHECK("pool fence init", p.submitted == 0 && p.completed == 0);
    uint64_t s1 = rdna4p_submit(&p), s2 = rdna4p_submit(&p), s3 = rdna4p_submit(&p);
    CHECK("pool 3 submits", s1 == 1 && s2 == 2 && s3 == 3 && p.completed == 0);
    rdna4p_done(&p, s2); CHECK("pool out-of-order done", p.completed == 0);
    rdna4p_done(&p, s1); CHECK("pool advance to 2", p.completed == 2);
    rdna4p_done(&p, s1); CHECK("pool idempotent done", p.completed == 2);
    rdna4p_done(&p, s3); CHECK("pool all done", p.completed == 3);
    rdna4p_destroy(&p);

    rdna4p_init(&p, 1); g_refuse_realloc = 1;
    CHECK("pool submit OOM", rdna4p_submit(&p) == 0 && p.submitted == 0 && p.npend == 0 && p.completed == 0);
    g_refuse_realloc = 0;
    CHECK("pool submit after OOM", rdna4p_submit(&p) == 1 && p.submitted == 1 && p.npend == 1);
    rdna4p_done(&p, 1); rdna4p_destroy(&p);

    rdna4p_pool heapTimeline, poolTimeline;
    rdna4p_init(&heapTimeline, 1); rdna4p_init(&poolTimeline, 1);
    uint64_t speculative = rdna4p_submit(&heapTimeline);
    g_refuse_realloc = 1; uint64_t poolFailure = rdna4p_submit(&poolTimeline); g_refuse_realloc = 0;
    CHECK("pool paired rollback",
          speculative == 1 && poolFailure == 0 && poolTimeline.submitted == 0 &&
          rdna4p_cancel_last(&heapTimeline, speculative) && heapTimeline.submitted == 0 && heapTimeline.npend == 0 &&
          !rdna4p_cancel_last(&heapTimeline, speculative));
    uint64_t heapRetry = rdna4p_submit(&heapTimeline), poolRetry = rdna4p_submit(&poolTimeline);
    rdna4p_done(&heapTimeline, heapRetry); rdna4p_done(&poolTimeline, poolRetry);
    CHECK("pool paired retry", heapRetry == 1 && poolRetry == 1 && heapTimeline.completed == 1 && poolTimeline.completed == 1);
    rdna4p_destroy(&heapTimeline); rdna4p_destroy(&poolTimeline);

    rdna4p_init(&p, 1);
    uint64_t sid = pool_fill_slab(&p, 7, 0, off);
    CHECK("pool fill slab", sid && off[15] == 15 * 256 * KIB && rdna4p_alloc(&p, 7, 256 * KIB, 256, 0, &r) == RDNA4P_R_NEED_SLAB);
    uint64_t sub = rdna4p_submit(&p); rdna4p_free_sub(&p, sid, off[3], 256 * KIB, 10);
    CHECK("pool deferred sub", rdna4p_waiting(&p) == 1 && rdna4p_alloc(&p, 7, 256 * KIB, 256, 11, &r) == RDNA4P_R_NEED_SLAB);
    uint64_t sub2 = rdna4p_submit(&p); rdna4p_done(&p, sub2);
    CHECK("pool later done keeps locked", rdna4p_alloc(&p, 7, 256 * KIB, 256, 12, &r) == RDNA4P_R_NEED_SLAB);
    rdna4p_done(&p, sub);
    CHECK("pool released after done", rdna4p_alloc(&p, 7, 256 * KIB, 256, 13, &r) == RDNA4P_R_SUB && r.off == off[3] && rdna4p_waiting(&p) == 0);
    uint64_t sw = rdna4p_submit(&p);
    CHECK("pool whole gated",
          rdna4p_alloc(&p, 3, 1 * MIB, 4096, 20, &r) == RDNA4P_R_NEED_WHOLE &&
          rdna4p_free_whole(&p, 3, 1 * MIB, (void *)0xAA, NULL, 21) == 1 && p.nrc == 0 &&
          rdna4p_alloc(&p, 3, 1 * MIB, 4096, 22, &r) == RDNA4P_R_NEED_WHOLE);
    rdna4p_done(&p, sw);
    CHECK("pool whole reuse & best-fit",
          rdna4p_alloc(&p, 3, 1 * MIB, 4096, 23, &r) == RDNA4P_R_REC && r.mem == (void *)0xAA &&
          rdna4p_free_whole(&p, 3, 1 * MIB, (void *)0xAB, NULL, 24) == 1 &&
          rdna4p_alloc(&p, 4, 1 * MIB, 4096, 25, &r) == RDNA4P_R_NEED_WHOLE &&
          rdna4p_alloc(&p, 3, 1 * MIB + 4096, 4096, 25, &r) == RDNA4P_R_NEED_WHOLE &&
          rdna4p_alloc(&p, 3, 1 * MIB, 4096, 25, &r) == RDNA4P_R_REC && r.size == 1 * MIB &&
          rdna4p_free_whole(&p, 3, 1 * MIB + 128 * KIB, (void *)0xB1, NULL, 26) == 1 &&
          rdna4p_free_whole(&p, 3, 1 * MIB, (void *)0xB0, NULL, 26) == 1 &&
          rdna4p_free_whole(&p, 3, 1 * MIB + 256 * KIB, (void *)0xB2, NULL, 26) == 1 &&
          rdna4p_free_whole(&p, 3, 1 * MIB + 260 * KIB, (void *)0xB3, NULL, 26) == 1 &&
          rdna4p_alloc(&p, 3, 1 * MIB, 4096, 27, &r) == RDNA4P_R_REC && r.mem == (void *)0xB0 && r.size == 1 * MIB &&
          rdna4p_alloc(&p, 3, 1 * MIB, 4096, 27, &r) == RDNA4P_R_REC && r.mem == (void *)0xB1 && r.size == 1 * MIB + 128 * KIB &&
          rdna4p_alloc(&p, 3, 1 * MIB, 4096, 27, &r) == RDNA4P_R_REC && r.mem == (void *)0xB2 && r.size == 1 * MIB + 256 * KIB &&
          rdna4p_alloc(&p, 3, 1 * MIB, 4096, 27, &r) == RDNA4P_R_NEED_WHOLE && p.nrc == 1 && p.recBytes == 1 * MIB + 260 * KIB);
    uint64_t sh = rdna4p_submit(&p);
    CHECK("pool handles", rdna4p_h_push(&p, 0x77, 32) == 1 && rdna4p_h_count(&p) == 1 && rdna4p_h_pop(&p) == 0);
    rdna4p_done(&p, sh);
    CHECK("pool handles pop", rdna4p_h_pop(&p) == 0x77 && rdna4p_h_count(&p) == 0);
    for (int i = 0; i < 32; i++) rdna4p_h_push(&p, 0x100 + i, 32);
    CHECK("pool handles cap", rdna4p_h_push(&p, 0x999, 32) == 0 && rdna4p_h_count(&p) == 32);
    rdna4p_destroy(&p);

    rdna4p_init(&p, 1); srand(11);
    static const uint64_t als[] = { 1, 4, 16, 256, 4096, 65536 };
    struct { uint64_t slab, off, size; } live[400]; int nl = 0, bad = 0, ov = 0;
    for (int i = 0; i < 3000; i++) {
        if (nl < 400 && (rand() % 3 || !nl)) {
            uint64_t al = als[rand() % 6], sz = 1 + (uint64_t)(rand() % (int)(RDNA4P_SUB_MAX));
            int kk = rdna4p_alloc(&p, 1, sz, al, 0, &r);
            if (kk == RDNA4P_R_NEED_SLAB) { rdna4p_add_slab(&p, 1, RDNA4P_SLAB_SIZE, (void *)1, NULL, 0); kk = rdna4p_alloc(&p, 1, sz, al, 0, &r); }
            if (kk != RDNA4P_R_SUB) { bad++; continue; }
            if (r.off % al || r.off + sz > RDNA4P_SLAB_SIZE) bad++;
            for (int j = 0; j < nl; j++) if (live[j].slab == r.slab && r.off < live[j].off + live[j].size && live[j].off < r.off + sz) ov++;
            live[nl].slab = r.slab; live[nl].off = r.off; live[nl].size = sz; nl++;
        } else { int j = rand() % nl; rdna4p_free_sub(&p, live[j].slab, live[j].off, live[j].size, 0); live[j] = live[--nl]; }
    }
    CHECK("pool alignment 3k", bad == 0 && ov == 0);
    rdna4p_destroy(&p);
    rdna4p_init(&p, 1); rdna4p_add_slab(&p, 2, RDNA4P_SLAB_SIZE, (void *)1, NULL, 0);
    rdna4p_alloc(&p, 2, 1, 1, 0, &r); uint64_t a0 = r.off; rdna4p_alloc(&p, 2, 100, 4096, 0, &r);
    CHECK("pool align gaps & direct",
          a0 == 0 && r.off == 4096 &&
          (rdna4p_alloc(&p, 2, 100, 65536, 0, &r), r.off == 65536) &&
          (rdna4p_alloc(&p, 2, 100, 256, 0, &r), r.off == 256) &&
          rdna4p_alloc(&p, 2, 100, 8 * MIB, 0, &r) == RDNA4P_R_DIRECT &&
          rdna4p_alloc(&p, 2, 65 * MIB, 4096, 0, &r) == RDNA4P_R_DIRECT);
    rdna4p_destroy(&p);

    rdna4p_init(&p, 1); sid = pool_fill_slab(&p, 5, 0, off); rdna4p_slab *s = rdna4p_find_slab(&p, sid);
    static const int ord[16] = { 5, 7, 6, 4, 0, 15, 14, 1, 3, 2, 8, 12, 10, 11, 9, 13 };
    for (int i = 0; i < 16; i++) rdna4p_free_sub(&p, sid, off[ord[i]], 256 * KIB, 100);
    CHECK("pool coalesce scrambled", s->nfr == 1 && s->fr[0].off == 0 && s->fr[0].size == RDNA4P_SLAB_SIZE && s->empty);
    sid = pool_fill_slab(&p, 6, 0, off); s = rdna4p_find_slab(&p, sid);
    rdna4p_free_sub(&p, sid, off[4], 256 * KIB, 0); rdna4p_free_sub(&p, sid, off[6], 256 * KIB, 0);
    CHECK("pool 2 holes", s->nfr == 2);
    rdna4p_free_sub(&p, sid, off[5], 256 * KIB, 0);
    CHECK("pool merged hole", s->nfr == 1 && s->fr[0].off == off[4] && s->fr[0].size == 768 * KIB &&
          rdna4p_alloc(&p, 6, 256 * KIB, 256, 0, &r) == RDNA4P_R_SUB && r.off == off[4]);
    rdna4p_destroy(&p);

    rdna4p_init(&p, 1); sid = pool_fill_slab(&p, 9, 1000, off);
    for (int i = 0; i < 16; i++) rdna4p_free_sub(&p, sid, off[i], 256 * KIB, 5 * SEC);
    rdna4p_trim(&p, 5 * SEC + 2 * SEC - 1); CHECK("pool slab <2s kept", p.nsl == 1 && !rdna4p_pop_release(&p, &rel));
    rdna4p_trim(&p, 5 * SEC + 2 * SEC); CHECK("pool slab 2s freed", p.nsl == 0 && rdna4p_pop_release(&p, &rel) && rel.slab == 1 && rel.mem == (void *)0x1000);
    sid = pool_fill_slab(&p, 9, 10 * SEC, off); rdna4p_free_sub(&p, sid, off[0], 256 * KIB, 10 * SEC); rdna4p_trim(&p, 20 * SEC);
    CHECK("pool slab live kept", p.nsl == 1 && !rdna4p_pop_release(&p, &rel));
    rdna4p_destroy(&p); rdna4p_init(&p, 1);
    sid = pool_fill_slab(&p, 9, 0, off); sub = rdna4p_submit(&p);
    for (int i = 0; i < 16; i++) rdna4p_free_sub(&p, sid, off[i], 256 * KIB, 1 * SEC);
    rdna4p_trim(&p, 100 * SEC); CHECK("pool deferred keeps slab", p.nsl == 1 && !rdna4p_pop_release(&p, &rel) && rdna4p_waiting(&p) == 16);
    rdna4p_done(&p, sub); rdna4p_trim(&p, 101 * SEC); CHECK("pool empty clock starts at reap", p.nsl == 1);
    rdna4p_trim(&p, 103 * SEC); CHECK("pool slab freed 2s later", p.nsl == 0);
    rdna4p_destroy(&p);

    rdna4p_init(&p, 1);
    for (int i = 0; i < 5; i++) rdna4p_free_whole(&p, 1, 64 * MIB, (void *)(uintptr_t)(0x10 + i), NULL, (uint64_t)(i + 1) * SEC / 100);
    CHECK("pool recycle cap", p.nrc == 4 && p.recBytes == 256 * MIB &&
          rdna4p_pop_release(&p, &rel) && rel.mem == (void *)0x10 && !rel.slab && !rdna4p_pop_release(&p, &rel) &&
          rdna4p_free_whole(&p, 1, 64 * MIB + 4096, (void *)0x99, NULL, 0) == 0);
    uint64_t t0 = SEC / 100 * 5;
    rdna4p_trim(&p, t0 + 2 * SEC); CHECK("pool recycle 2s exact", p.nrc == 1 && p.rc[0].mem == (void *)0x14);
    rdna4p_trim(&p, t0 + 2 * SEC + 1); CHECK("pool recycle expired", p.nrc == 0 && p.recBytes == 0);
    int nrel = 0; while (rdna4p_pop_release(&p, &rel)) nrel++;
    CHECK("pool 4 releases", nrel == 4);
    rdna4p_free_whole(&p, 1, 1 * MIB, (void *)0x30, NULL, 100 * SEC);
    rdna4p_alloc(&p, 1, 1 * MIB, 4096, 101 * SEC, &r); rdna4p_free_whole(&p, 1, 1 * MIB, (void *)0x30, NULL, 101 * SEC);
    rdna4p_trim(&p, 102 * SEC + 1 * SEC / 2); CHECK("pool re-put clock", p.nrc == 1);
    rdna4p_destroy(&p);

    rdna4p_init(&p, 1); uint64_t sbusy = rdna4p_submit(&p); int pooled = 0, direct = 0;
    for (int i = 0; i < 10; i++) { if (rdna4p_free_whole(&p, 1, 64 * MIB, (void *)(uintptr_t)(0x40 + i), NULL, 0)) pooled++; else direct++; }
    CHECK("pool deferred bound", pooled == 8 && direct == 2);
    rdna4p_done(&p, sbusy); rdna4p_reap(&p, 1);
    CHECK("pool deferred reaped into 256M cap", p.nrc == 4 && p.nd == 0);
    rdna4p_destroy(&p);

    rdna4p_init(&p, 0);
    CHECK("pool off switch",
          rdna4p_submit(&p) == 0 && p.submitted == 0 && p.npend == 0 &&
          rdna4p_alloc(&p, 1, 4096, 256, 0, &r) == RDNA4P_R_DIRECT &&
          rdna4p_alloc(&p, 1, 1 * MIB, 256, 0, &r) == RDNA4P_R_DIRECT &&
          rdna4p_alloc(&p, 1, 100 * MIB, 256, 0, &r) == RDNA4P_R_DIRECT &&
          (rdna4p_free_sub(&p, 1, 0, 4096, 0), rdna4p_free_whole(&p, 1, 1 * MIB, (void *)1, NULL, 0) == 0) &&
          (rdna4p_done(&p, 1), rdna4p_h_push(&p, 5, 32) == 0 && rdna4p_h_pop(&p) == 0 && rdna4p_h_count(&p) == 0) &&
          !p.nsl && !p.nrc && !p.nd && !p.nrl && !p.subAlloc && !p.subFree && !p.recMiss && !p.avoided && !p.directAlloc && !p.slabNew);
    rdna4p_destroy(&p); CHECK("pool off stays off", p.on == 0);

    { rdna4p_init(&p, 1); srand(23);
      enum { NL = 300, NF = 4000 };
      struct { uint64_t slab, off, size; } lv[NL]; int nlv = 0;
      struct { int whole; uint64_t slab, off, size, stamp; uintptr_t mem; int claimed; } fr[NF]; int nfr = 0;
      uint64_t open[64]; int nopen = 0, viol = 0, reused = 0, wreused = 0; uintptr_t memctr = 0x1000;
      struct { uintptr_t mem; uint64_t size; } wl[64]; int nwl = 0;
      for (int i = 0; i < 40000; i++) {
          int op = rand() % 8; uint64_t now = (uint64_t)i * 1000;
          if (op == 0 && nopen < 64) open[nopen++] = rdna4p_submit(&p);
          else if (op == 1 && nopen) { int j = rand() % nopen; rdna4p_done(&p, open[j]); open[j] = open[--nopen]; }
          else if (op <= 4) {
              uint64_t sz = 256 * (1 + (uint64_t)(rand() % 64)), al = als[rand() % 6]; rdna4p_res rs;
              int kk = rdna4p_alloc(&p, 1, sz, al, now, &rs);
              if (kk == RDNA4P_R_NEED_SLAB) { rdna4p_add_slab(&p, 1, RDNA4P_SLAB_SIZE, (void *)1, NULL, now); kk = rdna4p_alloc(&p, 1, sz, al, now, &rs); }
              if (kk == RDNA4P_R_SUB && nlv < NL) {
                  for (int f = 0; f < nfr; f++) if (!fr[f].whole && !fr[f].claimed && fr[f].slab == rs.slab && rs.off < fr[f].off + fr[f].size && fr[f].off < rs.off + sz) {
                      reused++; if (fr[f].stamp > p.completed) viol++;
                      if (rs.off <= fr[f].off && rs.off + sz >= fr[f].off + fr[f].size) fr[f].claimed = 1;
                  }
                  lv[nlv].slab = rs.slab; lv[nlv].off = rs.off; lv[nlv].size = sz; nlv++;
              }
          } else if (op == 5 && nlv) {
              int j = rand() % nlv; rdna4p_free_sub(&p, lv[j].slab, lv[j].off, lv[j].size, now);
              if (nfr < NF) { fr[nfr].whole = 0; fr[nfr].slab = lv[j].slab; fr[nfr].off = lv[j].off; fr[nfr].size = lv[j].size; fr[nfr].stamp = p.submitted; fr[nfr].claimed = 0; nfr++; }
              lv[j] = lv[--nlv];
          } else if (op == 6) {
              uint64_t sz = (1 + (uint64_t)(rand() % 4)) * MIB; rdna4p_res rs; int kk = rdna4p_alloc(&p, 2, sz, 4096, now, &rs);
              if (kk == RDNA4P_R_REC) { wreused++; for (int f = 0; f < nfr; f++) if (fr[f].whole && fr[f].mem == (uintptr_t)rs.mem && !fr[f].claimed) { if (fr[f].stamp > p.completed) viol++; fr[f].claimed = 1; } }
              if ((kk == RDNA4P_R_REC || kk == RDNA4P_R_NEED_WHOLE) && nwl < 64) { uintptr_t m = kk == RDNA4P_R_REC ? (uintptr_t)rs.mem : ++memctr; wl[nwl].mem = m; wl[nwl].size = kk == RDNA4P_R_REC ? rs.size : sz; nwl++; }
          } else if (op == 7 && nwl) {
              int j = rand() % nwl; int pl = rdna4p_free_whole(&p, 2, wl[j].size, (void *)wl[j].mem, NULL, now);
              if (pl && nfr < NF) { fr[nfr].whole = 1; fr[nfr].mem = wl[j].mem; fr[nfr].stamp = p.submitted; fr[nfr].claimed = 0; nfr++; }
              wl[j] = wl[--nwl];
          }
          if (i % 50 == 0) { rdna4p_trim(&p, now); rdna4p_rel q; while (rdna4p_pop_release(&p, &q)) {} }
          if (nfr >= NF - 2) nfr = 0;
      }
      CHECK("pool 40k fence property", viol == 0 && reused > 100 && wreused > 20);
      rdna4p_destroy(&p); }

    rdna4p_init(&p, 1);
    CHECK("pool slab adaptive target",
          rdna4p_slab_target(&p, 0, 1, 16 * KIB, 1000) == RDNA4P_SLAB_SIZE &&
          rdna4p_slab_target(&p, 5, 0, 16 * KIB, 1000) == RDNA4P_GTT_SLAB_SIZE &&
          rdna4p_slab_stepdown(RDNA4P_SLAB_SIZE, 16 * KIB) == RDNA4P_GTT_SLAB_SIZE &&
          rdna4p_slab_stepdown(RDNA4P_GTT_SLAB_SIZE, 16 * KIB) == RDNA4P_MIN_SLAB_SIZE &&
          rdna4p_slab_stepdown(RDNA4P_MIN_SLAB_SIZE, 16 * KIB) == 0);
    rdna4p_slab_note_ok(&p, 5, RDNA4P_MIN_SLAB_SIZE);
    CHECK("pool slab remembers ok", rdna4p_slab_target(&p, 5, 0, 16 * KIB, 2000) == RDNA4P_MIN_SLAB_SIZE);
    rdna4p_slab_note_fail(&p, 5, 10000);
    CHECK("pool slab backoff",
          rdna4p_slab_target(&p, 5, 0, 16 * KIB, 10000 + RDNA4P_SLAB_BACKOFF_NS - 1) == 0 &&
          rdna4p_slab_target(&p, 5, 0, 16 * KIB, 10000 + RDNA4P_SLAB_BACKOFF_NS) == RDNA4P_MIN_SLAB_SIZE);
    rdna4p_destroy(&p);

    { int badB = 0;
      CHECK("pool bucket edges",
            rdna4p_bucket_size(0) == 0 && rdna4p_bucket_size(1) == 1 &&
            rdna4p_bucket_size(RDNA4P_SUB_MAX) == RDNA4P_SUB_MAX &&
            rdna4p_bucket_size(RDNA4P_REC_MAX + 4096) == RDNA4P_REC_MAX + 4096 &&
            rdna4p_bucket_size(512 * KIB) == 512 * KIB && rdna4p_bucket_size(1 * MIB) == 1 * MIB &&
            rdna4p_bucket_size(64 * MIB) == 64 * MIB && rdna4p_bucket_size(256 * KIB + 1) == 320 * KIB &&
            rdna4p_bucket_size(1 * MIB + 1) == 1280 * KIB && rdna4p_bucket_size(10 * MIB + 1) == 12 * MIB &&
            rdna4p_bucket_size(64 * MIB - 1) == 64 * MIB);
      for (uint64_t sz = RDNA4P_SUB_MAX + 1; sz <= RDNA4P_REC_MAX; sz += (sz >> 5) + 4093) {
          uint64_t b = rdna4p_bucket_size(sz);
          if (b < sz || b > RDNA4P_REC_MAX || b - sz >= (sz >> 2) || rdna4p_bucket_size(b) != b || (b & (64 * KIB - 1)) != 0) badB++;
      }
      CHECK("pool bucket sweep", badB == 0); }

    rdna4p_init(&p, 1); pool_fill_slab(&p, 7, 0, off); char line[512]; rdna4p_fmt(&p, line, sizeof line);
    CHECK("pool fmt stats", strstr(line, "slabs(key:n)=[7:1]") && strstr(line, "live_sub=16") && strstr(line, "kernel_allocs_avoided="));
    rdna4p_destroy(&p);
}

static void suite_bar_policy(void) {
    garnet_bar_policy_t p;
    p = garnet_bar_choose_policy(0, 256, 0, 0);
    CHECK("bar small", p.full_bar == 0 && p.enable_nosam == 1 && p.vram_surf_cap_bytes == GARNET_VRAM_SURF_CAP_SMALL_BAR);
    p = garnet_bar_choose_policy(0, 0, 0, 0);
    CHECK("bar probe fail", p.full_bar == 0 && p.enable_nosam == 1 && p.vram_surf_cap_bytes == GARNET_VRAM_SURF_CAP_SMALL_BAR);
    p = garnet_bar_choose_policy(1, 256, 0, 0);
    CHECK("bar fullMode small bar0", p.full_bar == 0 && p.enable_nosam == 1 && p.vram_surf_cap_bytes == GARNET_VRAM_SURF_CAP_SMALL_BAR);
    p = garnet_bar_choose_policy(1, 8192, 0, 0);
    CHECK("bar 8G full", p.full_bar == 1 && p.enable_nosam == 0 && p.vram_surf_cap_bytes == GARNET_VRAM_SURF_CAP_FULL_BAR);
    p = garnet_bar_choose_policy(1, 16384, 0, 0);
    CHECK("bar 16G full", p.full_bar == 1 && p.enable_nosam == 0 && p.vram_surf_cap_bytes == GARNET_VRAM_SURF_CAP_FULL_BAR);
    p = garnet_bar_choose_policy(1, 16384, 1, 0);
    CHECK("bar force nosam", p.full_bar == 1 && p.enable_nosam == 1 && p.vram_surf_cap_bytes == GARNET_VRAM_SURF_CAP_FULL_BAR);
    p = garnet_bar_choose_policy(0, 256, 0, 1);
    CHECK("bar nosam-off", p.full_bar == 0 && p.enable_nosam == 0 && p.vram_surf_cap_bytes == GARNET_VRAM_SURF_CAP_SMALL_BAR);
    p = garnet_bar_choose_policy(0, 256, 1, 1);
    CHECK("bar force precedence", p.enable_nosam == 1);

    rdna4li_cache lc; rdna4li_init(&lc, 1, 64);
    rdna4li_key k1 = { .id = 41, .base = 0x100000, .size = 33554432, .poff = 0,
                       .w = 3840, .h = 2160, .bpr = 15360, .vk_fmt = 44, .usage = 0x17, .flags = 0 };
    void *img = NULL, *relImg = NULL; int inited = 0;
    CHECK("li cold miss", rdna4li_acquire(&lc, &k1, 1000, 1, &img, &inited) == 0);
    rdna4li_add(&lc, &k1, (void *)0xdead1, 0);
    CHECK("li uninit add", lc.n == 1 && lc.created == 1);
    rdna4li_mark_inited(&lc, (void *)0xdead1);
    CHECK("li warm hit", rdna4li_acquire(&lc, &k1, 2000, 2, &img, &inited) == 1 && img == (void *)0xdead1 && inited == 1);
    CHECK("li unref 1st", rdna4li_unref(&lc, (void *)0xdead1, 3000, 3) == 1 && lc.nrl == 0);
    CHECK("li unref 2nd", rdna4li_unref(&lc, (void *)0xdead1, 4000, 4) == 1 && lc.nrl == 0 && lc.n == 1);
    CHECK("li reacquire", rdna4li_acquire(&lc, &k1, 5000, 5, &img, &inited) == 1 && img == (void *)0xdead1 && inited == 1);
    rdna4li_unref(&lc, (void *)0xdead1, 6000, 6);
    rdna4li_key k1_moved = k1; k1_moved.base = 0x200000;
    CHECK("li moved retires old", rdna4li_acquire(&lc, &k1_moved, 7000, 7, &img, &inited) == 0 && lc.nrl == 1 && lc.mismatches == 1);
    CHECK("li fence pop", rdna4li_pop(&lc, 6, &relImg) == 0 && rdna4li_pop(&lc, 7, &relImg) == 1 && relImg == (void *)0xdead1);
    rdna4li_add(&lc, &k1_moved, (void *)0xdead2, 1); rdna4li_unref(&lc, (void *)0xdead2, 10000, 8);
    rdna4li_trim(&lc, 10000 + RDNA4IC_IDLE_NS + 1, 9);
    CHECK("li idle trim", lc.nrl == 1 && rdna4li_pop(&lc, 9, &relImg) == 1 && relImg == (void *)0xdead2);

    rdna4ic_cache ic; rdna4ic_init(&ic, 1);
    rdna4ic_add(&ic, k1_moved.id, k1_moved.base, k1_moved.size, (void *)0xbeef1, NULL);
    rdna4li_key k1_mem = k1_moved; k1_mem.mem = (void *)0xbeef1;
    rdna4li_add(&lc, &k1_mem, (void *)0xdead3, 1); rdna4li_unref(&lc, (void *)0xdead3, 20000, 10);
    rdna4ic_unref(&ic, (void *)0xbeef1, 20000, 10); rdna4ic_flush(&ic, 11);
    rdna4ic_add(&ic, k1_moved.id, k1_moved.base, k1_moved.size, (void *)0xbeef2, NULL);
    rdna4li_retire_orphans(&lc, &ic, 11);
    CHECK("li orphan retire on remap", lc.n == 0 && lc.nrl == 1 && rdna4li_pop(&lc, 11, &relImg) == 1 && relImg == (void *)0xdead3);
    rdna4li_add(&lc, &k1_mem, (void *)0xdead4, 0);
    CHECK("li is_inited", rdna4li_is_inited(&lc, (void *)0xdead4) == 0);
    rdna4li_mark_inited(&lc, (void *)0xdead4);
    CHECK("li mark_inited", rdna4li_is_inited(&lc, (void *)0xdead4) == 1);
    rdna4ic_destroy(&ic); rdna4li_destroy(&lc);
}

static void suite_vram_surf(void) {
    CHECK("vs enable gate",
          garnet_vs_should_enable(1, 0, 0) == 1 && garnet_vs_should_enable(1, 0, 1) == 0 &&
          garnet_vs_should_enable(0, 0, 0) == 0 && garnet_vs_should_enable(0, 1, 0) == 1 &&
          garnet_vs_should_enable(0, 1, 1) == 0);

    uint32_t baseFmt = 0, list[2] = {0, 0}, listCnt = 0;
    uint32_t fam = garnet_vs_format_family(GARNET_VS_VK_BGRA8_SRGB, &baseFmt, list, &listCnt);
    CHECK("vs bgra8 family", fam == GARNET_VS_FAM_BGRA8 && baseFmt == GARNET_VS_VK_BGRA8_UNORM &&
          listCnt == 2 && list[0] == GARNET_VS_VK_BGRA8_UNORM && list[1] == GARNET_VS_VK_BGRA8_SRGB &&
          garnet_vs_format_compatible(fam, baseFmt, GARNET_VS_VK_BGRA8_UNORM) == 1 &&
          garnet_vs_format_compatible(fam, baseFmt, GARNET_VS_VK_BGRA8_SRGB) == 1 &&
          garnet_vs_format_compatible(fam, baseFmt, GARNET_VS_VK_RGBA8_UNORM) == 0);
    uint32_t f16Base = 0, f16List[2] = {0, 0}, f16Cnt = 99;
    CHECK("vs rgba16f dcc", garnet_vs_format_family(GARNET_VS_VK_RGBA16_SFLOAT, &f16Base, f16List, &f16Cnt) == GARNET_VS_FAM_RGBA16F &&
          f16Base == GARNET_VS_VK_RGBA16_SFLOAT && f16Cnt == 0);

    CHECK("vs capture symbols",
          garnet_vs_symbol_is_capture("-[WSDisplay CaptureSurfaceMetal:]") == 1 &&
          garnet_vs_symbol_is_capture("_XHWCaptureDesktop") == 1 &&
          garnet_vs_symbol_is_capture("SLXCreateCompositeDestination") == 1 &&
          garnet_vs_symbol_is_capture("ScreenCaptureCopyFrame") == 1 &&
          garnet_vs_symbol_is_capture("WSCompositeLayerTree") == 0 &&
          garnet_vs_symbol_is_capture(NULL) == 0);

    garnet_vs_cache_t c; garnet_vs_init(&c, 1, 100ULL << 20);
    CHECK("vs class1 admission",
          garnet_vs_should_promote(&c, 101, 3840, 2160, 1, 0, 1, 1) == 1 &&
          garnet_vs_should_promote(&c, 101, 3840, 2160, 1, 1, 1, 1) == 0 &&
          garnet_vs_should_promote(&c, 101, 3840, 2160, 1, 0, 2, 1) == 0 &&
          garnet_vs_should_promote(&c, 101, 3840, 2160, 1, 0, 1, 0) == 0 &&
          garnet_vs_should_promote(&c, 200, 2648, 1880, 0, 0, 1, 1) == 0);
    CHECK("vs class2 bt & 2 writes",
          garnet_vs_obs_needs_bt(&c, 201) == 1 &&
          garnet_vs_note_ws_write(&c, 201, 1920, 1080, 0, 0, 1000000000ULL) == 0 &&
          garnet_vs_should_promote(&c, 201, 1920, 1080, 0, 0, 1, 1) == 0 &&
          garnet_vs_obs_needs_bt(&c, 201) == 0 &&
          garnet_vs_note_ws_write(&c, 201, 1920, 1080, 0, 0, 1500000000ULL) == 1 &&
          garnet_vs_should_promote(&c, 201, 1920, 1080, 0, 0, 1, 1) == 1);
    garnet_vs_note_ws_write(&c, 202, 128, 128, 0, 0, 1000000000ULL);
    garnet_vs_note_ws_write(&c, 202, 128, 128, 0, 0, 1100000000ULL);
    CHECK("vs small excluded", garnet_vs_should_promote(&c, 202, 128, 128, 0, 0, 1, 1) == 0);
    garnet_vs_note_ws_write(&c, 203, 3840, 2160, 0, 1, 1000000000ULL);
    garnet_vs_note_ws_write(&c, 203, 3840, 2160, 0, 0, 1100000000ULL);
    garnet_vs_note_ws_write(&c, 203, 3840, 2160, 0, 0, 1200000000ULL);
    CHECK("vs capture excluded", garnet_vs_should_promote(&c, 203, 3840, 2160, 0, 0, 1, 1) == 0 && c.capture_excluded_sids == 1);
    garnet_vs_note_ws_write(&c, 204, 1920, 1080, 0, 0, 1000000000ULL);
    CHECK("vs window expiry", garnet_vs_note_ws_write(&c, 204, 1920, 1080, 0, 0, 2500000000ULL) == 0 &&
          garnet_vs_should_promote(&c, 204, 1920, 1080, 0, 0, 1, 1) == 0);

    const uint32_t supersetUse = 0x1F;
    garnet_vs_range_t r1 = { .sid = 101, .base = 0x1000000, .ialloc = 33554432, .poff = 0, .w = 3840, .h = 2160, .bpr = 15360 };
    uint64_t hitId = 0, incompatId = 0;
    CHECK("vs cold probe", garnet_vs_probe(&c, &r1, GARNET_VS_VK_BGRA8_UNORM, 0x17, 1000, 1, &hitId) == GARNET_VS_PROBE_MISS);
    uint64_t id1 = garnet_vs_add(&c, &r1, GARNET_VS_VK_BGRA8_UNORM, supersetUse, 32ULL << 20,
                                 (void *)0x1001, (void *)0x2001, (void *)0x3001, (void *)0x4001, (void *)0x5001, 1000);
    CHECK("vs add & sRGB hit", id1 != 0 && c.n == 1 && c.live_bytes == (32ULL << 20) &&
          garnet_vs_probe(&c, &r1, GARNET_VS_VK_BGRA8_SRGB, 0x05, 2000, 2, &hitId) == GARNET_VS_PROBE_HIT && hitId == id1 && c.hits == 1);
    CHECK("vs incompat flush",
          garnet_vs_probe(&c, &r1, GARNET_VS_VK_RGBA8_UNORM, 0x05, 3000, 3, &incompatId) == GARNET_VS_PROBE_INCOMPAT_FLUSH &&
          incompatId == id1 && c.incompat_flushes == 1);
    garnet_vs_unref(&c, id1, 3100, 3); garnet_vs_unref(&c, id1, 3200, 3);
    garnet_vs_rel_t rel;
    CHECK("vs stale retire", c.n == 0 && garnet_vs_pop_release(&c, 2, &rel) == 0 &&
          garnet_vs_pop_release(&c, 3, &rel) == 1 && rel.opt_img == (void *)0x1001 && rel.ibuf == (void *)0x3001);

    uint64_t id2 = garnet_vs_add(&c, &r1, GARNET_VS_VK_BGRA8_UNORM, supersetUse, 32ULL << 20,
                                 (void *)0x1002, (void *)0x2002, (void *)0x3002, (void *)0x4002, (void *)0x5002, 4000);
    CHECK("vs upload elision",
          garnet_vs_begin_access(&c, id2, 7, 1, 0, 0) == 1 &&
          garnet_vs_begin_access(&c, id2, 7, 1, 0, 0) == 0 &&
          garnet_vs_begin_access(&c, id2, 8, 1, 0, 0) == 1 &&
          garnet_vs_begin_access(&c, id2, 9, 0, 1, 1) == 0 &&
          garnet_vs_begin_access(&c, id2, 9, 1, 0, 0) == 0);
    garnet_vs_invalidate_cpu_write(&c, id2);
    CHECK("vs cpu inv", garnet_vs_begin_access(&c, id2, 9, 1, 0, 0) == 1);
    garnet_vs_set_layout(&c, id2, 2); garnet_vs_cb_begin(&c, id2);
    garnet_vs_note_gpu_write(&c, id2, 9, 1000000000ULL, 0);
    uint64_t fid = 0, fgen = 0;
    CHECK("vs idle flush gating",
          c.dirty_count == 1 && c.lazy_writes == 1 &&
          garnet_vs_pick_idle_flush(&c, 1000000000ULL + GARNET_VS_IDLE_FLUSH_NS + 1, &fid, &fgen) == 0);
    garnet_vs_cb_end(&c, id2);
    CHECK("vs idle flush 50ms",
          garnet_vs_pick_idle_flush(&c, 1000000000ULL + GARNET_VS_IDLE_FLUSH_NS - 1, &fid, &fgen) == 0 &&
          garnet_vs_pick_idle_flush(&c, 1000000000ULL + GARNET_VS_IDLE_FLUSH_NS, &fid, &fgen) == 1 && fid == id2 && fgen == 1);
    garnet_vs_note_gpu_write(&c, id2, 10, 1100000000ULL, 0);
    garnet_vs_finish_flush(&c, id2, fgen, 1);
    CHECK("vs stale writeGen keeps dirty", c.dirty_count == 1 &&
          garnet_vs_pick_idle_flush(&c, 1100000000ULL + GARNET_VS_IDLE_FLUSH_NS, &fid, &fgen) == 1 && fid == id2 && fgen == 2);
    garnet_vs_finish_flush(&c, id2, fgen, 1);
    CHECK("vs matching writeGen clears dirty", c.dirty_count == 0 && c.idle_flushes == 1);

    garnet_vs_unref(&c, id2, 10, 4);
    garnet_vs_range_t r2 = r1; r2.sid = 102; r2.base = 0x2000000;
    garnet_vs_range_t r3 = r1; r3.sid = 103; r3.base = 0x3000000;
    uint64_t id3 = garnet_vs_add(&c, &r2, GARNET_VS_VK_BGRA8_UNORM, supersetUse, 40ULL << 20,
                                 (void *)0x1003, (void *)0x2003, (void *)0x3003, (void *)0x4003, (void *)0x5003, 5000);
    garnet_vs_set_layout(&c, id3, 2); garnet_vs_note_gpu_write(&c, id3, 1, 5000, 0); garnet_vs_unref(&c, id3, 20, 5);
    uint64_t id4 = garnet_vs_add(&c, &r3, GARNET_VS_VK_BGRA8_UNORM, supersetUse, 40ULL << 20,
                                 (void *)0x1004, (void *)0x2004, (void *)0x3004, (void *)0x4004, (void *)0x5004, 6000);
    garnet_vs_unref(&c, id4, 30, 6);
    uint64_t evId = 0, syncGen = 0; int evDirty = 0;
    CHECK("vs lru evict clean",
          garnet_vs_pick_evict_candidate(&c, &evId, &evDirty) == 1 && evId == id2 && evDirty == 0 &&
          garnet_vs_retire(&c, evId, 7) == 1 && c.unused_bytes == (80ULL << 20) &&
          garnet_vs_pick_evict_candidate(&c, &evId, &evDirty) == 0);
    c.cap_bytes = 50ULL << 20;
    CHECK("vs lru evict dirty",
          garnet_vs_pick_evict_candidate(&c, &evId, &evDirty) == 1 && evId == id3 && evDirty == 1 &&
          garnet_vs_retire(&c, evId, 8) == 0 && c.dirty_count == 1 &&
          garnet_vs_begin_sync_flush(&c, evId, &syncGen) == 1);
    garnet_vs_finish_flush(&c, evId, syncGen, 1);
    CHECK("vs lru dirty retired", c.dirty_count == 0 && garnet_vs_retire(&c, evId, 8) == 1 && c.unused_bytes == (40ULL << 20));
    CHECK("vs peek & capture mark", garnet_vs_peek_needs_upload(&c, id4, 0) == 1);
    garnet_vs_mark_capture_excluded(&c, 999);
    CHECK("vs capture mark blocks", garnet_vs_note_ws_write(&c, 999, 1920, 1080, 0, 0, 2000000000ULL) == 0 &&
          garnet_vs_should_promote(&c, 999, 1920, 1080, 0, 0, 1, 1) == 0);

    const uint64_t pb0 = garnet_vs_pathb_pack(0u), pb42 = garnet_vs_pathb_pack(42u);
    CHECK("vs pathb seed elision",
          garnet_vs_pathb_should_upload(0ULL, 0u) == 1 && garnet_vs_pathb_should_upload(0ULL, 7u) == 1 &&
          garnet_vs_pathb_should_upload(pb0, 0u) == 0 && garnet_vs_pathb_should_upload(pb0, 1u) == 1 &&
          garnet_vs_pathb_should_upload(pb42, 42u) == 0 && garnet_vs_pathb_should_upload(pb42, 43u) == 1);

    garnet_vs_range_t rd = r1; rd.sid = 109; rd.base = 0x9000000;
    uint64_t idDisp = garnet_vs_add(&c, &rd, GARNET_VS_VK_BGRA8_UNORM, supersetUse, 32ULL << 20,
                                    (void *)0x1009, (void *)0x2009, (void *)0x3009, (void *)0x4009, (void *)0x5009, 7000);
    garnet_vs_set_disp(&c, idDisp, 1); garnet_vs_set_layout(&c, idDisp, 2);
    garnet_vs_note_gpu_write(&c, idDisp, 50, 7000, 0);
    CHECK("vs dirty disp ignores seed", garnet_vs_peek_needs_upload(&c, idDisp, 51) == 0 && garnet_vs_begin_access(&c, idDisp, 51, 1, 0, 0) == 0);
    uint64_t dgen = 0; CHECK("vs disp sync flush", garnet_vs_begin_sync_flush(&c, idDisp, &dgen) == 1);
    garnet_vs_finish_flush(&c, idDisp, dgen, 1);
    CHECK("vs clean disp re-uploads", garnet_vs_peek_needs_upload(&c, idDisp, 52) == 1 && garnet_vs_begin_access(&c, idDisp, 52, 1, 0, 0) == 1);
    garnet_vs_note_gpu_write(&c, idDisp, 52, 8000, 0); garnet_vs_invalidate_cpu_write(&c, idDisp);
    CHECK("vs disp cpu inv", garnet_vs_peek_needs_upload(&c, idDisp, 52) == 1);
    garnet_vs_destroy(&c);
}

static int hb_add(rdna4hb_heap *h, uint64_t size, uint32_t type, int host, int dedicated,
                  int exclusive, uintptr_t token, uint64_t *id) {
    return rdna4hb_add_block(h, size, type, host, dedicated, exclusive, 0,
                             (void *)token, host ? (void *)(token + 1) : NULL, id);
}

static void suite_heap_backing(void) {
    rdna4hb_heap h; rdna4hb_res r; rdna4hb_reclaim gone; uint64_t block, lease, lease2;
    rdna4hb_init(&h, 64 * MIB, RDNA4HB_MAX_NORMAL);
    int k = rdna4hb_alloc(&h, 1000, 256, 64, 1, 0, 0, 0, NULL, &r);
    CHECK("hb need block", k == RDNA4HB_R_NEED_BLOCK && r.block_size == 4 * MIB && h.backing == 0);
    CHECK("hb add block", hb_add(&h, r.block_size, 0, 0, 0, 0, 0x1000, &block) && h.backing == 4 * MIB);
    k = rdna4hb_alloc(&h, 1000, 256, 64, 1, 0, 0, 0, NULL, &r); lease = r.lease;
    CHECK("hb first alloc", k == RDNA4HB_R_OK && r.block == block && r.offset == 0 && rdna4hb_handle_open(&h, block));
    CHECK("hb first-fit", rdna4hb_alloc(&h, 1000, 256, 64, 1, 0, 0, 0, NULL, &r) == RDNA4HB_R_OK && r.block == block && r.offset == 1024);
    lease2 = r.lease;
    CHECK("hb release stamped", rdna4hb_release(&h, lease, 7));
    rdna4hb_reap(&h, 6);
    CHECK("hb inflight held", rdna4hb_alloc(&h, 1000, 256, 64, 1, 0, 0, 0, NULL, &r) == RDNA4HB_R_OK && r.offset == 2048);
    rdna4hb_release(&h, r.lease, 8); rdna4hb_reap(&h, 7);
    CHECK("hb reuses hole", rdna4hb_alloc(&h, 1000, 256, 64, 1, 0, 0, 0, NULL, &r) == RDNA4HB_R_OK && r.offset == 0);
    rdna4hb_release(&h, r.lease, 9); rdna4hb_reap(&h, 9);
    CHECK("hb idempotent release", !rdna4hb_release(&h, lease, 10));
    rdna4hb_release(&h, lease2, 9); rdna4hb_reap(&h, 9);

    rdna4hb_destroy(&h); rdna4hb_init(&h, 4 * MIB, RDNA4HB_MAX_NORMAL);
    CHECK("hb maxAvail unbacked", rdna4hb_max_available(&h, 64 * KIB, 64 * KIB, 0) == 4 * MIB);
    hb_add(&h, 4 * MIB, 0, 0, 0, 0, 0x1800, &block);
    CHECK("hb full alloc", rdna4hb_alloc(&h, 4 * MIB, 64 * KIB, 64 * KIB, 1, 0, 0, 0, NULL, &r) == RDNA4HB_R_OK);
    lease = r.lease; rdna4hb_release(&h, lease, 7); rdna4hb_reap(&h, 6);
    CHECK("hb maxAvail inflight 0", rdna4hb_max_available(&h, 256, 64 * KIB, 0) == 0);
    rdna4hb_reap(&h, 7); CHECK("hb maxAvail reaped 4M", rdna4hb_max_available(&h, 256, 64 * KIB, 0) == 4 * MIB);

    rdna4hb_destroy(&h); rdna4hb_init(&h, 64 * MIB, RDNA4HB_MAX_NORMAL);
    k = rdna4hb_alloc(&h, 4096, 4096, 4096, 1, 0, 0, 0, NULL, &r);
    CHECK("hb class identity",
          k == RDNA4HB_R_NEED_BLOCK && hb_add(&h, r.block_size, 0, 0, 0, 0, 0x2000, &block) &&
          rdna4hb_alloc(&h, 4096, 4096, 4096, 2, 0, 0, 0, NULL, &r) == RDNA4HB_R_NEED_BLOCK &&
          rdna4hb_alloc(&h, 4096, 4096, 4096, 1, 1, 0, 0, NULL, &r) == RDNA4HB_R_NEED_BLOCK &&
          rdna4hb_alloc(&h, 4096, 4096, 4096, 1, 0, 0, 0, NULL, &r) == RDNA4HB_R_OK && r.block == block);

    rdna4hb_destroy(&h); rdna4hb_init(&h, MIB, RDNA4HB_MAX_NORMAL);
    k = rdna4hb_alloc(&h, 1000, 256, 16, 1, 0, 0, 0, NULL, &r);
    CHECK("hb small heap mixed",
          k == RDNA4HB_R_NEED_BLOCK && r.block_size == 1008 && hb_add(&h, r.block_size, 0, 0, 0, 0, 0x2800, &block) &&
          rdna4hb_alloc(&h, 1000, 256, 16, 1, 0, 0, 0, NULL, &r) == RDNA4HB_R_OK &&
          (k = rdna4hb_alloc(&h, 1024, 256, 16, 2, 0, 0, 1, NULL, &r)) == RDNA4HB_R_NEED_BLOCK && r.dedicated && r.block_size == 1024 &&
          hb_add(&h, r.block_size, 1, 0, 1, 0, 0x2900, &block) &&
          rdna4hb_alloc(&h, 1024, 256, 16, 2, 0, 0, 1, NULL, &r) == RDNA4HB_R_OK);

    rdna4hb_destroy(&h); rdna4hb_init(&h, 64 * MIB, RDNA4HB_MAX_NORMAL);
    k = rdna4hb_alloc(&h, 3 * MIB, 64 * KIB, 4 * KIB, 1, 0, 0, 1, NULL, &r);
    CHECK("hb dedicated", k == RDNA4HB_R_NEED_BLOCK && r.dedicated && r.block_size == 3 * MIB && hb_add(&h, r.block_size, 0, 0, 1, 0, 0x3000, &block));
    k = rdna4hb_alloc(&h, 3 * MIB, 64 * KIB, 4 * KIB, 1, 0, 0, 1, (void *)0xabc, &r); lease = r.lease;
    CHECK("hb dedicated bind & finalize",
          k == RDNA4HB_R_OK && r.offset == 0 && r.dedicated &&
          !rdna4hb_lease_owner_clear(&h, lease, (void *)0xdef) &&
          rdna4hb_alloc(&h, 1, 1, 1, 1, 0, 0, 1, NULL, &r) == RDNA4HB_R_NEED_BLOCK &&
          rdna4hb_handle_open(&h, block));
    rdna4hb_release(&h, lease, 12); rdna4hb_reap(&h, 11);
    CHECK("hb dedicated waits", !rdna4hb_finalize_candidate(&h, NULL, NULL, NULL));
    rdna4hb_reap(&h, 12); void *owner = NULL;
    CHECK("hb dedicated finalizes", rdna4hb_finalize_candidate(&h, &lease, &block, &owner) && owner == (void *)0xabc);
    rdna4hb_handle_close(&h, block); rdna4hb_finalized(&h, lease);
    CHECK("hb dedicated reclaimed", rdna4hb_take_reclaimable(&h, &gone) && gone.size == 3 * MIB && h.backing == 0);

    rdna4hb_destroy(&h); rdna4hb_init(&h, 1 * MIB, RDNA4HB_MAX_NORMAL);
    rdna4hb_alloc(&h, 64 * KIB, 256, 256, 1, 0, 0, 0, NULL, &r); hb_add(&h, r.block_size, 0, 0, 0, 0, 0x4000, &block);
    rdna4hb_alloc(&h, 64 * KIB, 256, 256, 1, 0, 0, 0, NULL, &r); lease = r.lease;
    rdna4hb_handle_open(&h, block); rdna4hb_release(&h, lease, 0); rdna4hb_reap(&h, 0);
    CHECK("hb handle lifetime", !rdna4hb_take_reclaimable(&h, &gone) && h.backing == r.block_size &&
          rdna4hb_alloc(&h, 64 * KIB, 256, 256, 1, 0, 0, 0, NULL, &r) == RDNA4HB_R_OK && r.offset == 0);
    rdna4hb_release(&h, r.lease, 0); rdna4hb_reap(&h, 0); rdna4hb_handle_close(&h, block);
    CHECK("hb handle close reclaims", rdna4hb_take_reclaimable(&h, &gone));

    rdna4hb_destroy(&h); rdna4hb_init(&h, 128 * MIB, RDNA4HB_MAX_NORMAL);
    k = rdna4hb_alloc(&h, 65 * MIB, 4096, 4096, 1, 0, 0, 0, NULL, &r);
    CHECK("hb large exclusive",
          k == RDNA4HB_R_NEED_BLOCK && r.exclusive && !r.dedicated && r.block_size == 65 * MIB &&
          hb_add(&h, r.block_size, 0, 0, 0, 1, 0x5000, &block) &&
          rdna4hb_alloc(&h, 65 * MIB, 4096, 4096, 1, 0, 0, 0, NULL, &r) == RDNA4HB_R_OK && r.offset == 0 &&
          rdna4hb_alloc(&h, 64 * MIB, 4096, 4096, 1, 0, 0, 0, NULL, &r) == RDNA4HB_R_NO_SPACE && h.backing <= h.size);

    rdna4hb_destroy(&h); rdna4hb_init(&h, 8 * MIB, RDNA4HB_MAX_NORMAL);
    rdna4hb_alloc(&h, 64 * KIB, 256, 256, 1, 0, 0, 0, NULL, &r); hb_add(&h, r.block_size, 0, 0, 0, 0, 0x6000, &block);
    rdna4hb_alloc(&h, 64 * KIB, 256, 256, 1, 0, 0, 0, NULL, &r); lease = r.lease;
    rdna4hb_handle_open(&h, block); rdna4hb_release(&h, lease, 0); rdna4hb_reap(&h, 0); rdna4hb_handle_close(&h, block);
    CHECK("hb clean evict", rdna4hb_take_reclaimable(&h, &gone) && gone.id == block && h.backing == 0);

    rdna4hb_destroy(&h); rdna4hb_init(&h, 1 * MIB, RDNA4HB_MAX_NORMAL); hb_add(&h, 1 * MIB, 0, 0, 0, 0, 0x7000, &block);
    CHECK("hb granularity",
          rdna4hb_alloc(&h, 1, 1, 64 * KIB, 1, 0, 0, 0, NULL, &r) == RDNA4HB_R_OK && r.offset == 0 &&
          (lease = r.lease, rdna4hb_alloc(&h, 1, 1, 64 * KIB, 1, 0, 0, 0, NULL, &r) == RDNA4HB_R_OK && r.offset == 64 * KIB));
    rdna4hb_release(&h, lease, 0); rdna4hb_release(&h, r.lease, 0); rdna4hb_reap(&h, 0);

    rdna4hb_destroy(&h); rdna4hb_init(&h, 1 * MIB, RDNA4HB_MAX_NORMAL);
    CHECK("hb invalid requests",
          rdna4hb_alloc(&h, 0, 1, 1, 1, 0, 0, 0, NULL, &r) == RDNA4HB_R_INVALID &&
          rdna4hb_alloc(&h, 1, 0, 1, 1, 0, 0, 0, NULL, &r) == RDNA4HB_R_INVALID &&
          rdna4hb_alloc(&h, 1, 1, 1, 0, 0, 0, 0, NULL, &r) == RDNA4HB_R_INVALID && !h.nblk && !h.nlease &&
          !rdna4hb_add_block(&h, 1 * MIB + 1, 0, 0, 0, 0, 0, (void *)1, NULL, NULL) && !h.nblk);

    rdna4hb_destroy(&h); rdna4hb_init(&h, 16 * MIB, RDNA4HB_MAX_NORMAL); hb_add(&h, 16 * MIB, 0, 0, 0, 0, 0x8000, &block);
    enum { NL = 64 };
    typedef struct { uint64_t lease, off, size; } live_range;
    live_range live[NL]; int nl = 0, overlaps = 0, stale = 0; uint64_t serial = 0; srand(31);
    for (int i = 0; i < 20000; i++) {
        if (nl < NL && (rand() & 1)) {
            uint64_t size = 1 + (uint64_t)(rand() % 65536), align = 1ULL << (rand() % 13);
            if (rdna4hb_alloc(&h, size, align, 256, 1, 0, 0, 0, NULL, &r) == RDNA4HB_R_OK) {
                uint64_t need = (size + 255) & ~UINT64_C(255);
                if (r.offset % align) stale++;
                for (int j = 0; j < nl; j++) if (r.offset < live[j].off + live[j].size && live[j].off < r.offset + need) overlaps++;
                live[nl++] = (live_range){ r.lease, r.offset, need };
            }
        } else if (nl) { int j = rand() % nl; rdna4hb_release(&h, live[j].lease, ++serial); live[j] = live[--nl]; }
        if ((i & 7) == 0) rdna4hb_reap(&h, serial > 2 ? serial - 2 : 0);
        if (h.backing > h.size) stale++;
    }
    rdna4hb_reap(&h, serial); CHECK("hb 20k random no overlap", overlaps == 0 && stale == 0);
    for (int j = 0; j < nl; j++) rdna4hb_release(&h, live[j].lease, serial + 1);
    rdna4hb_reap(&h, serial + 1); CHECK("hb random retire", h.nlease == 0);
    rdna4hb_destroy(&h);

    rdna4hb_init(&h, 29229056ULL, RDNA4HB_MAX_NORMAL);
    const uint64_t s2_sz[3] = { 24949248ULL, 164608ULL, 4103168ULL };
    int s2_ok = 1; uint64_t s2_max1 = 0;
    for (int i = 0; i < 3; i++) {
        k = rdna4hb_alloc(&h, s2_sz[i], 256, 256, 1, 0, 0, 0, NULL, &r);
        if (k == RDNA4HB_R_NEED_BLOCK) {
            if (!hb_add(&h, r.block_size, 0, 0, 0, r.exclusive, (uintptr_t)(0x9000 + i * 0x100), &block)) { s2_ok = 0; break; }
            k = rdna4hb_alloc(&h, s2_sz[i], 256, 256, 1, 0, 0, 0, NULL, &r);
        }
        if (k != RDNA4HB_R_OK) { s2_ok = 0; break; }
        if (i == 1) s2_max1 = rdna4hb_max_available(&h, 256, 256, 0);
    }
    CHECK("hb sample2 tight auto heap", s2_ok && h.backing == 29217024ULL && h.backing <= h.size && s2_max1 >= 4103168ULL);
    rdna4hb_destroy(&h);

    rdna4hb_init(&h, 29229056ULL, RDNA4HB_MAX_NORMAL); int mixed_ok = 1;
    k = rdna4hb_alloc(&h, 24949248ULL, 256, 256, 1, 0, 0, 0, NULL, &r);
    mixed_ok &= (k == RDNA4HB_R_NEED_BLOCK && hb_add(&h, r.block_size, 0, 0, 0, 0, 0x9400, &block) && rdna4hb_alloc(&h, 24949248ULL, 256, 256, 1, 0, 0, 0, NULL, &r) == RDNA4HB_R_OK);
    k = rdna4hb_alloc(&h, 164608ULL, 256, 256, 1, 0, 0, 0, NULL, &r);
    mixed_ok &= (k == RDNA4HB_R_NEED_BLOCK && r.block_size == 164608ULL && hb_add(&h, r.block_size, 0, 0, 0, 0, 0x9500, &block) && rdna4hb_alloc(&h, 164608ULL, 256, 256, 1, 0, 0, 0, NULL, &r) == RDNA4HB_R_OK);
    k = rdna4hb_alloc(&h, 4103168ULL, 256, 256, 2, 0, 0, 1, NULL, &r);
    mixed_ok &= (k == RDNA4HB_R_NEED_BLOCK && r.dedicated && r.block_size == 4103168ULL && hb_add(&h, r.block_size, 1, 0, 1, 0, 0x9600, &block) && rdna4hb_alloc(&h, 4103168ULL, 256, 256, 2, 0, 0, 1, NULL, &r) == RDNA4HB_R_OK);
    CHECK("hb tight mixed remainder", mixed_ok && h.backing == 29217024ULL);
    rdna4hb_destroy(&h);

    rdna4hb_init(&h, 5 * MIB, RDNA4HB_MAX_NORMAL);
    k = rdna4hb_alloc(&h, 164 * KIB, 256, 256, 1, 0, 0, 0, NULL, &r);
    int tight5 = (k == RDNA4HB_R_NEED_BLOCK && r.block_size == 164 * KIB && hb_add(&h, r.block_size, 0, 0, 0, 0, 0x9700, &block) && rdna4hb_alloc(&h, 164 * KIB, 256, 256, 1, 0, 0, 0, NULL, &r) == RDNA4HB_R_OK);
    k = rdna4hb_alloc(&h, 4900 * KIB, 256, 256, 1, 0, 0, 0, NULL, &r);
    tight5 &= (k == RDNA4HB_R_NEED_BLOCK && r.block_size == 4900 * KIB && hb_add(&h, r.block_size, 0, 0, 0, 0, 0x9800, &block) && rdna4hb_alloc(&h, 4900 * KIB, 256, 256, 1, 0, 0, 0, NULL, &r) == RDNA4HB_R_OK);
    CHECK("hb tight 5M", tight5 && h.backing <= h.size);
    rdna4hb_destroy(&h);

    rdna4hb_init(&h, 6 * MIB, RDNA4HB_MAX_NORMAL);
    k = rdna4hb_alloc(&h, 1024, 256, 256, 1, 0, 0, 0, NULL, &r);
    int mixed6 = (k == RDNA4HB_R_NEED_BLOCK && r.block_size == 1024 && hb_add(&h, r.block_size, 0, 0, 0, 0, 0x9900, &block) && rdna4hb_alloc(&h, 1024, 256, 256, 1, 0, 0, 0, NULL, &r) == RDNA4HB_R_OK);
    k = rdna4hb_alloc(&h, 5 * MIB, 256, 256, 2, 0, 0, 1, NULL, &r);
    mixed6 &= (k == RDNA4HB_R_NEED_BLOCK && r.dedicated && r.block_size == 5 * MIB && hb_add(&h, r.block_size, 1, 0, 1, 0, 0x9a00, &block) && rdna4hb_alloc(&h, 5 * MIB, 256, 256, 2, 0, 0, 1, NULL, &r) == RDNA4HB_R_OK);
    CHECK("hb tight 6M mixed", mixed6 && h.backing <= h.size);
    rdna4hb_destroy(&h);
}

int main(void) {
    suite_nocopy();
    suite_buffer_ranges_and_registry();
    suite_heapalloc();
    suite_pool();
    suite_bar_policy();
    suite_vram_surf();
    suite_heap_backing();
    printf("test-host-memory: %s (%u checks, %u failed)\n", g_fails ? "FAIL" : "PASS", g_checks, g_fails);
    return g_fails != 0;
}
