// test-drawcache.c: garnet_drawcache.h (P5b per-draw redundancy decisions) on the host.
// Build/run on the host Mac:  cc -O1 -Wall -Wextra -Werror -o /tmp/test-drawcache test-drawcache.c && /tmp/test-drawcache
#include <stdio.h>
#include <stdlib.h>
#include "garnet_drawcache.h"
static int fails;
#define CHECK(name, cond, ...) do { int ok_ = (cond); printf("%s %s: ", ok_ ? "ok  " : "FAIL", name); printf(__VA_ARGS__); printf("\n"); if (!ok_) fails++; } while (0)

// a stage signature like GarnetDevice.m builds: layout, dsl, then buffer (handle, offset), texture (object, view), sampler
static void mk(rdna4dc_sig *s, uint64_t layout, uint64_t dsl, uint64_t buf, uint64_t off, uint64_t tex, uint64_t view, uint64_t smp) {
    rdna4dc_sig_begin(s, layout, dsl); rdna4dc_sig_push(s, buf); rdna4dc_sig_push(s, off); rdna4dc_sig_push(s, tex); rdna4dc_sig_push(s, view); rdna4dc_sig_push(s, smp);
}
static rdna4dc_rpkey rk(uint32_t na, uint32_t fetch) {
    rdna4dc_rpkey k; memset(&k, 0, sizeof k); k.na = na; k.fetch = fetch;
    for (uint32_t i = 0; i < na; i++) k.a[i] = (rdna4dc_att){ 44, 1, 2, 0, 2, 1, 0, 2 };
    return k;
}

int main(void) {
    rdna4dc_enc e; rdna4dc_sig a, b; uint64_t set;
    rdna4dc_enc_init(&e);

    // ---- 1. signature equality ----
    mk(&a, 1, 10, 0xB0, 0, 0xE0, 0xF0, 0xA0);
    CHECK("empty stage: no match", !rdna4dc_stage_match(&e.st[0], &a, &set), "miss");
    rdna4dc_stage_store(&e.st[0], &a, 0x5E7);
    mk(&b, 1, 10, 0xB0, 0, 0xE0, 0xF0, 0xA0);
    CHECK("identical bindings: same set re-used", rdna4dc_stage_match(&e.st[0], &b, &set) && set == 0x5E7, "set %#llx", (unsigned long long)set);
    mk(&b, 1, 10, 0xB0, 256, 0xE0, 0xF0, 0xA0);
    CHECK("offset-only change: new set", !rdna4dc_stage_match(&e.st[0], &b, &set), "miss");
    mk(&b, 1, 11, 0xB0, 0, 0xE0, 0xF0, 0xA0);
    CHECK("set-layout (dsl) change: new set", !rdna4dc_stage_match(&e.st[0], &b, &set), "miss");
    mk(&b, 2, 10, 0xB0, 0, 0xE0, 0xF0, 0xA0);
    CHECK("pipeline-layout change: new set", !rdna4dc_stage_match(&e.st[0], &b, &set), "miss");
    mk(&b, 1, 10, 0xB0, 0, 0xE1, 0xF0, 0xA0);
    CHECK("texture swapped: new set", !rdna4dc_stage_match(&e.st[0], &b, &set), "miss");
    mk(&b, 1, 10, 0xB0, 0, 0xE0, 0xF1, 0xA0);
    CHECK("same texture, other view: new set", !rdna4dc_stage_match(&e.st[0], &b, &set), "miss");
    mk(&b, 1, 10, 0xB0, 0, 0xE0, 0xF0, 0xA1);
    CHECK("sampler swapped: new set", !rdna4dc_stage_match(&e.st[0], &b, &set), "miss");
    mk(&b, 1, 10, 0xB1, 0, 0xE0, 0xF0, 0xA0);
    CHECK("buffer swapped: new set", !rdna4dc_stage_match(&e.st[0], &b, &set), "miss");
    rdna4dc_sig_begin(&b, 1, 10); rdna4dc_sig_push(&b, 0xB0);
    CHECK("shorter signature: new set", !rdna4dc_stage_match(&e.st[0], &b, &set), "miss");
    CHECK("the other stage is independent", !rdna4dc_stage_match(&e.st[1], &a, &set), "fragment stage empty");
    { rdna4dc_sig big; rdna4dc_sig_begin(&big, 1, 10); for (int i = 0; i < RDNA4DC_MAXW + 5; i++) rdna4dc_sig_push(&big, 7);
      rdna4dc_stage_store(&e.st[1], &big, 0x99);
      CHECK("over-long signature is never cached", big.over && !e.st[1].valid && !rdna4dc_stage_match(&e.st[1], &big, &set), "over %d valid %d", big.over, e.st[1].valid); }

    // ---- 2. reset at encoder start / layout change ----
    mk(&a, 1, 10, 0xB0, 0, 0xE0, 0xF0, 0xA0);
    rdna4dc_enc_init(&e); rdna4dc_enc_layout(&e, 1); rdna4dc_stage_store(&e.st[0], &a, 0x5E7);
    CHECK("same layout again keeps the cached set", (rdna4dc_enc_layout(&e, 1), rdna4dc_stage_match(&e.st[0], &a, &set)), "kept");
    rdna4dc_enc_layout(&e, 2);
    CHECK("another pipeline layout forgets the cached sets", !e.st[0].valid && !e.st[1].valid, "valid %d %d", e.st[0].valid, e.st[1].valid);
    rdna4dc_stage_store(&e.st[0], &a, 0x5E7); e.lastPipe = 5;
    rdna4dc_enc_reset_bound(&e);
    CHECK("reset (encoder start / new pass) forgets sets, layout and pipeline", !e.st[0].valid && e.lastPipe == 0 && e.layout == 0, "valid %d lastPipe %llu layout %llu", e.st[0].valid, (unsigned long long)e.lastPipe, (unsigned long long)e.layout);
    { rdna4dc_enc f; rdna4dc_enc_init(&f);
      CHECK("a fresh encoder has nothing cached", !f.st[0].valid && !f.st[1].valid && f.lastPipe == 0 && !f.pcValid, "fresh"); }

    // ---- 3. pipeline bind skipping ----
    rdna4dc_enc_init(&e);
    CHECK("first bind is done", !rdna4dc_enc_pipe_same(&e, 0x77), "bound");
    CHECK("same pipeline: skipped", rdna4dc_enc_pipe_same(&e, 0x77), "skipped");
    CHECK("other pipeline: done", !rdna4dc_enc_pipe_same(&e, 0x78), "bound");
    CHECK("then back to the first: done (not skipped)", !rdna4dc_enc_pipe_same(&e, 0x77), "bound");
    rdna4dc_enc_reset_bound(&e);
    CHECK("after a reset the same pipeline is bound again", !rdna4dc_enc_pipe_same(&e, 0x77), "bound");
    CHECK("counters", e.pipesSkipped == 1 && e.pipesBound == 4, "skipped %llu bound %llu", (unsigned long long)e.pipesSkipped, (unsigned long long)e.pipesBound);

    // ---- 4. pipelineForTopology cache ----
    { uint64_t p; int dummy1 = 1, dummy2 = 2;
      rdna4dc_enc_init(&e);
      CHECK("pc miss on empty", !rdna4dc_enc_pc_get(&e, 3, 0, 1, &dummy1, &p), "miss");
      rdna4dc_enc_pc_put(&e, 3, 0, 1, &dummy1, 0xAA);
      CHECK("pc hit", rdna4dc_enc_pc_get(&e, 3, 0, 1, &dummy1, &p) && p == 0xAA, "pipe %#llx", (unsigned long long)p);
      CHECK("pc topology differs", !rdna4dc_enc_pc_get(&e, 4, 0, 1, &dummy1, &p), "miss");
      CHECK("pc cull differs", !rdna4dc_enc_pc_get(&e, 3, 1, 1, &dummy1, &p), "miss");
      CHECK("pc front differs", !rdna4dc_enc_pc_get(&e, 3, 0, 0, &dummy1, &p), "miss");
      CHECK("pc pso differs", !rdna4dc_enc_pc_get(&e, 3, 0, 1, &dummy2, &p), "miss");
      rdna4dc_enc_pc_put(&e, 3, 0, 1, &dummy1, 0);
      CHECK("a failed (null) pipeline is not cached", !rdna4dc_enc_pc_get(&e, 3, 0, 1, &dummy1, &p), "miss");
      int psos[10];
      rdna4dc_enc_init(&e);
      for (int i = 0; i < (int)RDNA4DC_PC_CAP; i++) rdna4dc_enc_pc_put(&e, 3, 0, 1, &psos[i], 0x1000u + (uint64_t)i);
      int allHit = 1;
      for (int i = 0; i < (int)RDNA4DC_PC_CAP; i++) {
          if (!rdna4dc_enc_pc_get(&e, 3, 0, 1, &psos[i], &p) || p != 0x1000u + (uint64_t)i) allHit = 0;
      }
      CHECK("pc 8-slot cache hits all 8 alternating PSOs", allHit && e.pcValid == (int)RDNA4DC_PC_CAP, "allHit=%d valid=%d", allHit, e.pcValid);
      rdna4dc_enc_pc_put(&e, 3, 0, 1, &psos[8], 0x2000u);
      CHECK("pc 9th entry evicts slot 0 and keeps slots 1..7 + new",
            !rdna4dc_enc_pc_get(&e, 3, 0, 1, &psos[0], &p) &&
            rdna4dc_enc_pc_get(&e, 3, 0, 1, &psos[8], &p) && p == 0x2000u &&
            rdna4dc_enc_pc_get(&e, 3, 0, 1, &psos[7], &p) && p == 0x1007u, "evicted slot 0"); }

    // ---- 4b. CB-shared segment-scoped descriptor-set LRU & pc cache across render encoders ----
    { rdna4dc_cbcache cb; rdna4dc_enc e1, e2; rdna4dc_sig sa, sb, slong; uint64_t s, p; int pso1 = 1, pso2 = 2;
      rdna4dc_cb_init(&cb); rdna4dc_enc_init(&e1); rdna4dc_enc_init(&e2);
      rdna4dc_cb_pc_put(&cb, 3, 0, 1, &pso1, 0xA1);
      rdna4dc_cb_pc_put(&cb, 3, 0, 1, &pso2, 0xA2);
      CHECK("CB pc cache hits across separate encoders",
            rdna4dc_cb_pc_get(&cb, 3, 0, 1, &pso1, &p) && p == 0xA1 &&
            rdna4dc_cb_pc_get(&cb, 3, 0, 1, &pso2, &p) && p == 0xA2, "ok");
      rdna4dc_cb_pc_put(&cb, 3, 0, 1, &pso1, 0);
      CHECK("CB pc null invalidates entry while keeping others",
            !rdna4dc_cb_pc_get(&cb, 3, 0, 1, &pso1, &p) && rdna4dc_cb_pc_get(&cb, 3, 0, 1, &pso2, &p) && p == 0xA2, "ok");
      mk(&sa, 1, 10, 0xB0, 0, 0xE0, 0xF0, 0xA0);
      mk(&sb, 2, 20, 0xB1, 64, 0xE1, 0xF1, 0xA1);
      rdna4dc_enc_layout(&e1, 1); rdna4dc_stage_store(&e1.st[0], &sa, 0x501); rdna4dc_cb_lru_put(&cb, 0, &sa, 0x501);
      rdna4dc_enc_layout(&e1, 2); rdna4dc_stage_store(&e1.st[0], &sb, 0x502); rdna4dc_cb_lru_put(&cb, 0, &sb, 0x502);
      CHECK("new encoder in same segment hits CB LRU despite empty st[0]",
            !rdna4dc_stage_match(&e2.st[0], &sa, &s) && rdna4dc_cb_lru_find(&cb, 0, &sa, &s) && s == 0x501 &&
            rdna4dc_cb_lru_find(&cb, 0, &sb, &s) && s == 0x502, "lru %#llx", (unsigned long long)s);
      CHECK("CB LRU stages stay independent", !rdna4dc_cb_lru_find(&cb, 1, &sa, &s), "stage 1 empty");
      rdna4dc_cb_reset_segment(&cb);
      CHECK("reset_segment drops CB LRU while preserving CB pc cache",
            !rdna4dc_cb_lru_find(&cb, 0, &sa, &s) && !rdna4dc_cb_lru_find(&cb, 0, &sb, &s) &&
            rdna4dc_cb_pc_get(&cb, 3, 0, 1, &pso2, &p) && p == 0xA2, "cleared LRU, kept pc");
      for (uint32_t i = 0; i <= RDNA4DC_LRU_CAP; i++) { mk(&sa, 1, 10, 0xB0 + i, 0, 0xE0, 0xF0, 0xA0); rdna4dc_cb_lru_put(&cb, 0, &sa, 0x700u + i); }
      mk(&sa, 1, 10, 0xB0, 0, 0xE0, 0xF0, 0xA0);
      mk(&sb, 1, 10, 0xB0 + RDNA4DC_LRU_CAP, 0, 0xE0, 0xF0, 0xA0);
      CHECK("CB LRU 17th entry evicts slot 0 and keeps newest",
            !rdna4dc_cb_lru_find(&cb, 0, &sa, &s) && rdna4dc_cb_lru_find(&cb, 0, &sb, &s) && s == 0x700u + RDNA4DC_LRU_CAP, "evicted 0");
      rdna4dc_sig_begin(&slong, 1, 10);
      for (uint32_t i = 0; i < RDNA4DC_LRU_MAXW; i++) rdna4dc_sig_push(&slong, 0x100u + i);
      rdna4dc_cb_lru_put(&cb, 0, &slong, 0x999);
      CHECK("signature longer than RDNA4DC_LRU_MAXW is skipped by CB LRU", !rdna4dc_cb_lru_find(&cb, 0, &slong, &s), "n=%u", slong.n); }

    // ---- 5. render-pass key ----
    { rdna4dc_rpcache *c = (rdna4dc_rpcache *)calloc(1, sizeof *c); rdna4dc_rpkey k = rk(2, 0), k2;
      CHECK("rp miss on empty cache", rdna4dc_rp_find(c, &k) == 0, "miss");
      CHECK("rp add", rdna4dc_rp_add(c, &k, 0x100) && c->created == 1, "created %llu", (unsigned long long)c->created);
      k2 = rk(2, 0); CHECK("equal key hits", rdna4dc_rp_find(c, &k2) == 0x100 && c->hits == 1, "hits %llu", (unsigned long long)c->hits);
      k2 = rk(2, 1); CHECK("fetch differs", rdna4dc_rp_find(c, &k2) == 0, "miss");
      k2 = rk(3, 0); CHECK("attachment count differs", rdna4dc_rp_find(c, &k2) == 0, "miss");
      k2 = rk(2, 0); k2.a[1].format++;           CHECK("format differs", rdna4dc_rp_find(c, &k2) == 0, "miss");
      k2 = rk(2, 0); k2.a[0].samples++;          CHECK("samples differs", rdna4dc_rp_find(c, &k2) == 0, "miss");
      k2 = rk(2, 0); k2.a[1].loadOp++;           CHECK("loadOp differs", rdna4dc_rp_find(c, &k2) == 0, "miss");
      k2 = rk(2, 0); k2.a[0].storeOp++;          CHECK("storeOp differs", rdna4dc_rp_find(c, &k2) == 0, "miss");
      k2 = rk(2, 0); k2.a[0].stencilLoadOp++;    CHECK("stencilLoadOp differs", rdna4dc_rp_find(c, &k2) == 0, "miss");
      k2 = rk(2, 0); k2.a[0].stencilStoreOp++;   CHECK("stencilStoreOp differs", rdna4dc_rp_find(c, &k2) == 0, "miss");
      k2 = rk(2, 0); k2.a[1].initialLayout++;    CHECK("initialLayout differs", rdna4dc_rp_find(c, &k2) == 0, "miss");
      k2 = rk(2, 0); k2.a[1].finalLayout++;      CHECK("finalLayout differs", rdna4dc_rp_find(c, &k2) == 0, "miss");
      k2 = rk(2, 0); k2.a[5].format = 99;        CHECK("garbage beyond na is ignored", rdna4dc_rp_find(c, &k2) == 0x100, "hit");
      for (uint32_t i = 1; i < RDNA4DC_RP_CAP; i++) { k2 = rk(1, 0); k2.a[0].format = 1000 + i; rdna4dc_rp_add(c, &k2, 0x200 + i); }
      k2 = rk(1, 1); CHECK("cache full at the cap: add refused, caller keeps ownership", c->n == RDNA4DC_RP_CAP && !rdna4dc_rp_add(c, &k2, 0x999) && c->uncached == 1, "n %u uncached %llu", c->n, (unsigned long long)c->uncached);
      k2 = rk(1, 0); k2.a[0].format = 1000 + 7; CHECK("entries still found when full", rdna4dc_rp_find(c, &k2) == 0x207, "hit");
      free(c); }

    printf("%s: %d failure(s)\n", fails ? "FAILED" : "all passed", fails);
    return fails != 0;
}
