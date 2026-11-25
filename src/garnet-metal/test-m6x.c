// test-m6x.c: bundle build 13 (M6 Stage 1b) + 15 (M6 Stage 2): rdna4_m6x.h on the host - the HDMI scanout decisions PER INSTANCE (the monitor B, and the monitor A since bundle 15) (enable, pool budget, the acquire sequence with failure injection, encode-time and completion-time routing, the 1 s keep-alive tick,
// the re-copy memory, R6) - plus the review items R2 (property generation, never an older read over a newer one), R3 (bundle half) and R6, plus source pins on the glue in GarnetDevice.m and the kernel / Mesa mirrors.
// Build/run on the host Mac:  cc -O1 -D_GNU_SOURCE -Wall -Wextra -Werror -o /tmp/test-m6x test-m6x.c && /tmp/test-m6x .      (the argument is src/garnet-metal)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rdna4_scanabi.h"
#include "rdna4_m6route.h"
#include "rdna4_dispflip.h"
#include "rdna4_m6x.h"
static int fails, checks;
#define CHECK(name, cond) do { checks++; if (!(cond)) { fails++; printf("FAIL: %s\n", name); } } while (0)
static char *slurp(const char *dir, const char *rel) {
    char path[1024]; snprintf(path, sizeof path, "%s/%s", dir, rel);
    FILE *f = fopen(path, "rb"); if (!f) { printf("FAIL: cannot read %s\n", path); fails++; return strdup(""); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1); size_t r = fread(b, 1, (size_t)n, f); b[r] = 0; fclose(f); return b;
}
static size_t countof(const char *h, const char *n) { size_t c = 0; const char *p = h; while ((p = strstr(p, n))) { c++; p += strlen(n); } return c; }
static const char *after(const char *h, const char *n) { const char *p = h ? strstr(h, n) : NULL; return p; }
static void w32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static size_t blob2(uint8_t *out, uint32_t gen, uint32_t n, const uint32_t (*ents)[3]) {
    w32(out, RDNA4M6_BLOB_VERSION2); w32(out + 4, n); w32(out + 8, gen);
    for (uint32_t i = 0; i < n; i++) { w32(out + 12 + i * 12, ents[i][0]); w32(out + 16 + i * 12, ents[i][1]); w32(out + 20 + i * 12, ents[i][2]); }
    return 12 + (size_t)n * 12;
}
static size_t blob1(uint8_t *out, uint32_t n, const uint32_t (*ents)[3]) {
    w32(out, RDNA4M6_BLOB_VERSION); w32(out + 4, n);
    for (uint32_t i = 0; i < n; i++) { w32(out + 8 + i * 12, ents[i][0]); w32(out + 12 + i * 12, ents[i][1]); w32(out + 16 + i * 12, ents[i][2]); }
    return 8 + (size_t)n * 12;
}
// ---- the fake operations of the acquire driver, with failure injection and a record of EVERYTHING it was asked
typedef struct { int geom, pool, kacq, failAlloc, failReg, calls_pool, calls_kacq, marked, allocs, regs, offs, off_reason, freed, activated, released_by_off, dp_calls; } fk_t;
static int f_pool(void *c) { fk_t *f = c; f->calls_pool++; return f->pool; }
static int f_geom(void *c) { fk_t *f = c; return f->geom; }
static int f_kacq(void *c) { fk_t *f = c; f->calls_kacq++; return f->kacq; }
static void f_mark(void *c) { fk_t *f = c; f->marked = 1; }
static int f_alloc(void *c, int i) { fk_t *f = c; f->allocs++; return f->failAlloc == i ? -1 : 0; }
static int f_reg(void *c, int i) { fk_t *f = c; f->regs++; return f->failReg == i ? -1 : 0; }
static void f_off(void *c, int r, const char *w) { fk_t *f = c; (void)w; f->offs++; f->off_reason = r; if (f->marked) f->released_by_off = 1; }
static void f_free(void *c) { fk_t *f = c; f->freed++; }
static void f_act(void *c) { fk_t *f = c; f->activated++; }
static rdna4x_ops_t mkops(fk_t *f) { rdna4x_ops_t o = { .ctx = f, .pool_verdict = f_pool, .k_acquire = f_kacq, .mark_acquired = f_mark, .geom_verdict = f_geom, .alloc_slot = f_alloc, .register_slot = f_reg, .off = f_off, .free_slots = f_free, .activate = f_act }; return o; }

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    uint8_t b[512];
    // ---- constants: the bundle's copies equal the kernel's
    CHECK("scan_query flag bit 6 is the kernel's", RDNA4X_SCANQ_M6FLIP == RDNA4N_SCANQ_M6FLIP && RDNA4N_SCANQ_M6FLIP == (1u << 6) && RDNA4N_SCANQ_M6FLIP != RDNA4N_SCANQ_M6);
    CHECK("scan_query flag bit 7 is the kernel's third latch", RDNA4X_SCANQ_M6FLIP1 == RDNA4N_SCANQ_M6FLIP1 && RDNA4N_SCANQ_M6FLIP1 == (1u << 7) && RDNA4N_SCANQ_M6FLIP1 != RDNA4N_SCANQ_M6FLIP);
    CHECK("selectors 22..26, the instances and the tags", RDNA4N_SEL_SCANX_ACQUIRE == 22 && RDNA4N_SEL_SCANX_REGISTER == 23 && RDNA4N_SEL_SCANX_PRESENT == 24 && RDNA4N_SEL_SCANX_STATUS == 25 && RDNA4N_SEL_SCANX_RELEASE == 26 && RDNA4X_INST_MONB == 2 && RDNA4X_INST_MONA == 1 && rdna4x_desc(2)->tag == RDNA4N_SCANX_SLOT_TAG && rdna4x_desc(1)->tag == RDNA4N_SCANX_SLOT_TAG_MONA && rdna4x_desc(2)->tag == 0x20u && rdna4x_desc(1)->tag == 0x10u);
    CHECK("the descriptors: ONE lookup, nothing for the DP or any other number", rdna4x_desc(0) == NULL && rdna4x_desc(3) == NULL && rdna4x_desc(1)->inst == 1 && rdna4x_desc(2)->inst == 2 && rdna4x_desc(1) != rdna4x_desc(2));
    CHECK("the descriptors' geometry and slot sizes", rdna4x_desc(1)->w == 1920 && rdna4x_desc(1)->h == 1080 && rdna4x_desc(1)->pitch_bytes == 7680 && rdna4x_desc(1)->slot_bytes == 8323072ull && rdna4x_desc(2)->w == 2560 && rdna4x_desc(2)->h == 1440 && rdna4x_desc(2)->pitch_bytes == 10240 && rdna4x_desc(2)->slot_bytes == 14745600ull);
    CHECK("slot_bytes = pitch * h rounded up to 64 KiB, for both", ({ int ok = 1; for (int i = 1; i <= 2; i++) { const rdna4x_desc_t *d = rdna4x_desc(i); const uint64_t f = rdna4x_frame_bytes(d); ok = ok && d->slot_bytes == ((f + 65535) & ~65535ull) && f <= d->slot_bytes; } ok; }));
    CHECK("the kill files are per instance", !strcmp(rdna4x_desc(1)->killfile, "/private/tmp/rdna4m-noflip1") && !strcmp(rdna4x_desc(2)->killfile, "/private/tmp/rdna4m-noflip2"));
    CHECK("tagged ids are per instance", rdna4x_id_ok(2, 0x20) && rdna4x_id_ok(2, 0x22) && !rdna4x_id_ok(2, 0x1f) && !rdna4x_id_ok(2, 0x23) && !rdna4x_id_ok(2, 0) && !rdna4x_id_ok(2, 2) && !rdna4x_id_ok(2, 0x10) && rdna4x_id_ok(1, 0x10) && rdna4x_id_ok(1, 0x12) && !rdna4x_id_ok(1, 0x13) && !rdna4x_id_ok(1, 0x20) && !rdna4x_id_ok(1, 0x0f) && !rdna4x_id_ok(0, 0x10) && !rdna4x_id_ok(3, 0x20));
    CHECK("the surface geometry gate: exactly the instance's size", rdna4x_surface_geom_ok(2, 2560, 1440, 10240) && !rdna4x_surface_geom_ok(2, 1920, 1080, 7680) && !rdna4x_surface_geom_ok(2, 2560, 1440, 10304) && !rdna4x_surface_geom_ok(2, 2560, 1600, 10240) &&
          rdna4x_surface_geom_ok(1, 1920, 1080, 7680) && !rdna4x_surface_geom_ok(1, 2560, 1440, 10240) && !rdna4x_surface_geom_ok(1, 1920, 1080, 7744) && !rdna4x_surface_geom_ok(0, 1920, 1080, 7680));
    CHECK("the classifier: the DP's plane OR an ENABLED HDMI instance's own geometry", rdna4x_classify_mask(2560, 1440, 10240, 0, 2560, 1440, 10240) == 1u && rdna4x_classify_mask(2560, 1440, 10240, 0x4, 2560, 1440, 10240) == 5u && rdna4x_classify_mask(2560, 1440, 10240, 0, 1920, 1080, 7680) == 0u &&
          rdna4x_classify_mask(2560, 1440, 10240, 0x2, 1920, 1080, 7680) == 2u && rdna4x_classify_mask(2560, 1440, 10240, 0x4, 1920, 1080, 7680) == 0u && rdna4x_classify_mask(2560, 1440, 10240, 0x6, 1920, 1080, 7680) == 2u);
    // ---- the Acquire geometry cross-check (ABI 1.12 fifth word)
    { const uint64_t mona = 1920ull | (1080ull << 16) | (1920ull << 32), monb = 2560ull | (1440ull << 16) | (2560ull << 32);
      CHECK("acq geometry: the monitor A's word equals the monitor A's descriptor, the monitor B's the monitor B's", rdna4x_acq_geom_ok(rdna4x_desc(1), 5, mona) && rdna4x_acq_geom_ok(rdna4x_desc(2), 5, monb));
      CHECK("acq geometry: ANOTHER display's word is refused (no swap: the monitor A given the monitor B's, the monitor B the monitor A's)", !rdna4x_acq_geom_ok(rdna4x_desc(1), 5, monb) && !rdna4x_acq_geom_ok(rdna4x_desc(2), 5, mona));
      CHECK("acq geometry: a wrong pitch / height / width is refused", !rdna4x_acq_geom_ok(rdna4x_desc(1), 5, 1920ull | (1080ull << 16) | (1984ull << 32)) && !rdna4x_acq_geom_ok(rdna4x_desc(1), 5, 1920ull | (1088ull << 16) | (1920ull << 32)) && !rdna4x_acq_geom_ok(rdna4x_desc(1), 5, 1856ull | (1080ull << 16) | (1920ull << 32)));
      CHECK("acq geometry: a four-word answer (a 0.0.661 kernel) is accepted for the monitor B ONLY - the monitor A requires the fifth word", rdna4x_acq_geom_ok(rdna4x_desc(2), 4, 0) && !rdna4x_acq_geom_ok(rdna4x_desc(1), 4, 0) && !rdna4x_acq_geom_ok(NULL, 5, mona)); }
    // ---- enable: ALL of them
    { rdna4x_enable_t e = { 2, 1, 1, 1, 1, 1, 0 };
      CHECK("enable (monitor B): everything present -> ON", rdna4x_enabled(&e));
      rdna4x_enable_t t;
      t = e; t.kernel_m6 = 0; CHECK("enable: the kernel m6 latch OFF -> OFF", !rdna4x_enabled(&t));
      t = e; t.kernel_m6flip = 0; CHECK("enable: the kernel m6flip latch OFF -> OFF", !rdna4x_enabled(&t));
      t = e; t.have_call = 0; CHECK("enable: the rdna4n_call export missing -> OFF", !rdna4x_enabled(&t));
      t = e; t.have_handle = 0; CHECK("enable: the bo_handle export missing -> OFF", !rdna4x_enabled(&t));
      t = e; t.killfile = 1; CHECK("enable: the kill file present -> OFF", !rdna4x_enabled(&t));
      t = e; t.kernel_m6flip1 = 0; CHECK("enable (monitor B): the THIRD latch is not needed", rdna4x_enabled(&t));
      t = e; t.inst = 0; CHECK("enable: the DP / an unknown instance is never an HDMI instance", !rdna4x_enabled(&t)); t.inst = 3; CHECK("enable: instance 3 neither", !rdna4x_enabled(&t));
      rdna4x_enable_t a = e; a.inst = 1;
      CHECK("enable (monitor A): everything present -> ON (from its OWN constants: no plane geometry of the DP is an input)", rdna4x_enabled(&a));
      t = a; t.kernel_m6flip1 = 0; CHECK("enable (monitor A): the garnet-m6flip1 latch OFF -> OFF", !rdna4x_enabled(&t));
      t = a; t.kernel_m6flip = 0; CHECK("enable (monitor A): m6flip OFF -> OFF", !rdna4x_enabled(&t));
      t = a; t.killfile = 1; CHECK("enable (monitor A): ITS kill file present -> OFF", !rdna4x_enabled(&t));
      t = a; t.have_call = 0; CHECK("enable (monitor A): the export missing -> OFF", !rdna4x_enabled(&t)); }
    // ---- the pool budget (review S4): the kernel's FREE visible-VRAM figure, not RADV's heap size
    { const uint64_t slot = 14745600ull;
      rdna4x_pool_t p = { .free_bytes = 100 * RDNA4X_MIB, .slot_bytes = slot, .slots = 3, .margin_bytes = RDNA4X_MARGIN_BYTES };
      CHECK("pool: 100 MiB FREE holds the monitor B's 3 slots (42.2 MiB) + the 32 MiB margin", rdna4x_pool_verdict(&p) == RDNA4X_POOL_OK);
      CHECK("pool: need = 3 slots + margin", rdna4x_pool_need(&p) == 3 * slot + RDNA4X_MARGIN_BYTES);
      p.free_bytes = 60 * RDNA4X_MIB; CHECK("pool: 60 MiB free (a full 256 MiB heap mostly in use) does NOT - the figure is what is LEFT", rdna4x_pool_verdict(&p) == RDNA4X_POOL_TOO_SMALL);
      p.free_bytes = rdna4x_pool_need(&p); CHECK("pool: exactly the need fits", rdna4x_pool_verdict(&p) == RDNA4X_POOL_OK);
      p.free_bytes = rdna4x_pool_need(&p) - 1; CHECK("pool: one byte short does not", rdna4x_pool_verdict(&p) == RDNA4X_POOL_TOO_SMALL);
      p.free_bytes = 0; CHECK("pool: an unreported figure (an older kernel, out[3] = 0) is a refusal, never a guess", rdna4x_pool_verdict(&p) == RDNA4X_POOL_NO_FIGURE);
      p.free_bytes = 256 * RDNA4X_MIB; p.slots = 0; CHECK("pool: no monitor B slots asked is a refusal", rdna4x_pool_verdict(&p) == RDNA4X_POOL_NO_FIGURE); }
    // ---- the acquire driver with failure injection: allocation failure leaves instance 2 OFF, DP untouched
    { fk_t f; rdna4x_ops_t o;
      memset(&f, 0, sizeof f); f.failAlloc = f.failReg = -1; o = mkops(&f);
      CHECK("acquire: all steps succeed -> ACTIVE, 3 slots, nothing released, not freed", rdna4x_acquire_run(&o) == 0 && f.allocs == 3 && f.regs == 3 && f.activated == 1 && f.offs == 0 && !f.freed && f.marked);
      memset(&f, 0, sizeof f); f.failAlloc = f.failReg = -1; f.pool = RDNA4X_POOL_TOO_SMALL; o = mkops(&f);
      CHECK("acquire: the free-VRAM figure does not cover the slots -> checked AFTER SCANX_ACQUIRE (it returns the figure), BEFORE any slot is allocated; OFF, instance 2 RELEASED, never ACTIVE", rdna4x_acquire_run(&o) == RDNA4DF_R_ALLOC && f.calls_kacq == 1 && f.calls_pool == 1 && f.allocs == 0 && f.offs == 1 && f.released_by_off && !f.activated);
      memset(&f, 0, sizeof f); f.failAlloc = f.failReg = -1; f.kacq = -5; o = mkops(&f);
      CHECK("acquire: SCANX_ACQUIRE refused -> OFF, no slot allocated, nothing to release", rdna4x_acquire_run(&o) == RDNA4DF_R_ERROR && f.allocs == 0 && f.offs == 1 && !f.marked && !f.released_by_off && !f.activated);
      for (int i = 0; i < 3; i++) {
          memset(&f, 0, sizeof f); f.failAlloc = i; f.failReg = -1; o = mkops(&f);
          CHECK("acquire: slot allocation fails (slot 0 / 1 / 2) -> OFF, instance 2 RELEASED, slots freed, never ACTIVE", rdna4x_acquire_run(&o) == RDNA4DF_R_ALLOC && f.allocs == i + 1 && f.offs == 1 && f.off_reason == RDNA4DF_R_ALLOC && f.released_by_off && f.freed == 1 && !f.activated);
          memset(&f, 0, sizeof f); f.failAlloc = -1; f.failReg = i; o = mkops(&f);
          CHECK("acquire: slot registration fails (slot 0 / 1 / 2) -> OFF, released, freed, never ACTIVE", rdna4x_acquire_run(&o) == RDNA4DF_R_ALLOC && f.regs == i + 1 && f.offs == 1 && f.released_by_off && f.freed == 1 && !f.activated);
      }
      memset(&f, 0, sizeof f); f.failAlloc = f.failReg = -1; f.geom = -1; o = mkops(&f);
      CHECK("acquire (bundle 15): the kernel's geometry word is NOT the instance's -> checked right after SCANX_ACQUIRE, BEFORE the pool and any slot; OFF with the geometry reason, the instance RELEASED, never ACTIVE", rdna4x_acquire_run(&o) == RDNA4DF_R_GEOM && f.calls_kacq == 1 && f.calls_pool == 0 && f.allocs == 0 && f.offs == 1 && f.off_reason == RDNA4DF_R_GEOM && f.released_by_off && !f.activated);
      CHECK("the driver has no operation that reaches the DP (the ops table carries none)", f.dp_calls == 0 && sizeof(rdna4x_ops_t) == 10 * sizeof(void *)); }
    // ---- per-instance pool verdicts (the monitor A's slots are 8,323,072 B; contract section 3: the monitor A needs ~23.8 MiB of slots + the 32 MiB margin)
    { rdna4x_pool_t p = { .free_bytes = 0, .slot_bytes = rdna4x_desc(1)->slot_bytes, .slots = 3, .margin_bytes = RDNA4X_MARGIN_BYTES };
      p.free_bytes = 56 * RDNA4X_MIB; CHECK("pool (monitor A): 56 MiB free holds its 3 slots (23.8 MiB) + the margin", rdna4x_pool_verdict(&p) == RDNA4X_POOL_OK && rdna4x_pool_need(&p) == 3ull * 8323072ull + RDNA4X_MARGIN_BYTES);
      p.free_bytes = 50 * RDNA4X_MIB; CHECK("pool (monitor A): 50 MiB does not", rdna4x_pool_verdict(&p) == RDNA4X_POOL_TOO_SMALL);
      p.slot_bytes = rdna4x_desc(2)->slot_bytes; p.free_bytes = 56 * RDNA4X_MIB; CHECK("pool (monitor B): the same 56 MiB does NOT hold the monitor B's 42.2 MiB of slots + margin (the budget is per instance)", rdna4x_pool_verdict(&p) == RDNA4X_POOL_TOO_SMALL); }
    // ---- encode-time plan (3-way: DP, monitor A, monitor B). xon mask: bit i = instance i enabled and not OFF
    { rdna4m6_t o; memset(&o, 0, sizeof o); int t = 7;
      const uint32_t e[5][3] = { { 101, 0, 0 }, { 201, 2, 0 }, { 202, 2, 0 }, { 401, 1, 0 }, { 301, 0, RDNA4M6_ENT_AMBIGUOUS } };
      size_t n = blob1(b, 5, e); rdna4m6_parse(&o, b, n);
      CHECK("plan: latch OFF -> instance 0 for everything (build 10)", rdna4x_plan_inst(&o, 201, 0x6, &t) == RDNA4X_PLAN_DP && t == 0 && rdna4x_plan_inst(&o, 999, 0x6, &t) == RDNA4X_PLAN_DP && rdna4x_plan_inst(&o, 401, 0x6, &t) == RDNA4X_PLAN_DP);
      o.latch = 1;
      CHECK("plan: a DP surface -> instance 0, not tentative", rdna4x_plan_inst(&o, 101, 0x6, &t) == RDNA4X_PLAN_DP && t == 0);
      CHECK("plan: a monitor B surface -> instance 2 when enabled", rdna4x_plan_inst(&o, 201, 0x6, &t) == RDNA4X_PLAN_MONB && t == 0 && rdna4x_plan_inst(&o, 201, 0x4, &t) == RDNA4X_PLAN_MONB);
      CHECK("plan: an monitor A surface -> instance 1 when enabled (and ONLY instance 1)", rdna4x_plan_inst(&o, 401, 0x6, &t) == RDNA4X_PLAN_MONA && t == 0 && rdna4x_plan_inst(&o, 401, 0x2, &t) == RDNA4X_PLAN_MONA && rdna4x_plan_inst(&o, 201, 0x2, &t) == RDNA4X_PLAN_SKIP);
      CHECK("plan: a monitor B surface with instance 2 OFF -> no slot at all; an monitor A surface with instance 1 OFF likewise", rdna4x_plan_inst(&o, 201, 0x2, &t) == RDNA4X_PLAN_SKIP && rdna4x_plan_inst(&o, 401, 0x4, &t) == RDNA4X_PLAN_SKIP && rdna4x_plan_inst(&o, 401, 0, &t) == RDNA4X_PLAN_SKIP);
      CHECK("plan: UNKNOWN -> instance 0 only, TENTATIVELY - never an HDMI instance (enabled or not)", rdna4x_plan_inst(&o, 999, 0x6, &t) == RDNA4X_PLAN_DP && t == 1 && rdna4x_plan_inst(&o, 999, 0, &t) == RDNA4X_PLAN_DP && t == 1 && rdna4x_plan_inst(&o, 0, 0x6, &t) == RDNA4X_PLAN_DP && t == 1);
      { int ok = 1; for (uint32_t id = 1000; id < 1100; id++) { const int p = rdna4x_plan_inst(&o, id, 0x6, &t); ok = ok && p != RDNA4X_PLAN_MONB && p != RDNA4X_PLAN_MONA; } CHECK("plan: no unknown ID is EVER planned to instance 1 or 2", ok); }
      CHECK("plan: an ambiguous surface takes no slot", rdna4x_plan_inst(&o, 301, 0x6, &t) == RDNA4X_PLAN_SKIP);
      CHECK("plan: the empty table (just reset) is all tentative", ({ rdna4m6_t z; memset(&z, 0, sizeof z); z.latch = 1; rdna4x_plan_inst(&z, 5, 0x6, &t) == RDNA4X_PLAN_DP && t == 1; }));
      CHECK("plan: the instance numbers are the kernel's (DP 0, monitor A 1, monitor B 2) and the table's", RDNA4X_PLAN_DP == 0 && RDNA4X_PLAN_MONA == 1 && RDNA4X_PLAN_MONB == 2 && RDNA4X_PLAN_MONA == (int)RDNA4M6_INST_MONA && RDNA4X_PLAN_MONB == (int)RDNA4M6_INST_MONB && RDNA4X_PLAN_MONA == (int)RDNA4X_INST_MONA && RDNA4X_PLAN_MONB == (int)RDNA4X_INST_MONB);
      // THE SIZE GATE (test 12): a surface the table maps to instance i whose size is not geom(i) takes no slot (inst_geom_mismatch)
      CHECK("size gate: the monitor A's plan needs 1920x1080 x 7680 B; the monitor B's 2560x1440 x 10240 B; the DP's plan needs the DP plane (R-B1)", rdna4x_plan_geom_ok(1, 1920, 1080, 7680, 2560, 1440, 10240) && !rdna4x_plan_geom_ok(1, 2560, 1440, 10240, 2560, 1440, 10240) && rdna4x_plan_geom_ok(2, 2560, 1440, 10240, 2560, 1440, 10240) && !rdna4x_plan_geom_ok(2, 1920, 1080, 7680, 2560, 1440, 10240) && rdna4x_plan_geom_ok(0, 2560, 1440, 10240, 2560, 1440, 10240));
      CHECK("size gate: an monitor A-planned surface of the DP's size takes no slot, and so does a monitor B-planned one of the monitor A's (the copy rectangle would not fit the slot)", !rdna4x_plan_geom_ok(RDNA4X_PLAN_MONA, 2560, 1440, 10240, 2560, 1440, 10240) && !rdna4x_plan_geom_ok(RDNA4X_PLAN_MONB, 1920, 1080, 7680, 2560, 1440, 10240));
      // R-B1: an monitor A-size surface with an UNKNOWN ID is planned tentatively to the DP; at the DP's size the copy would read 14,745,600 B from an 8,294,400 B surface
      { rdna4m6_t z; memset(&z, 0, sizeof z); z.latch = 1; int tt = 0; const int pi = rdna4x_plan_inst(&z, 4242, 0x6, &tt);
        CHECK("R-B1: monitor A-size surface, UNKNOWN ID -> planned tentatively to the DP, and the DP's size gate gives it NO slot (1920x1080x7680 vs the DP plane); a DP-size one passes", pi == RDNA4X_PLAN_DP && tt == 1 && !rdna4x_plan_geom_ok(pi, 1920, 1080, 7680, 2560, 1440, 10240) && rdna4x_plan_geom_ok(pi, 2560, 1440, 10240, 2560, 1440, 10240));
        CHECK("R-B1: the DP gate checks width, height and pitch each (one wrong -> no slot), and an unset DP plane admits nothing", !rdna4x_plan_geom_ok(0, 2559, 1440, 10240, 2560, 1440, 10240) && !rdna4x_plan_geom_ok(0, 2560, 1439, 10240, 2560, 1440, 10240) && !rdna4x_plan_geom_ok(0, 2560, 1440, 10236, 2560, 1440, 10240) && !rdna4x_plan_geom_ok(0, 0, 0, 0, 0, 0, 0)); }
      // R-S2: only the five-word shape refusal (-EINVAL) is retried with four words, only for the monitor B
      CHECK("R-S2: the monitor B is retried with four words after -EINVAL only; the monitor A never; no other error (-EBUSY -ENOMEM -EACCES -ETIME -ECANCELED -ENODEV -ENOSYS -EIO) and no success is retried", rdna4x_acquire_retry4(2, -EINVAL) && !rdna4x_acquire_retry4(1, -EINVAL) && !rdna4x_acquire_retry4(0, -EINVAL) && !rdna4x_acquire_retry4(2, 0) && !rdna4x_acquire_retry4(2, -EBUSY) && !rdna4x_acquire_retry4(2, -ENOMEM) && !rdna4x_acquire_retry4(2, -EACCES) && !rdna4x_acquire_retry4(2, -ETIME) && !rdna4x_acquire_retry4(2, -ECANCELED) && !rdna4x_acquire_retry4(2, -ENODEV) && !rdna4x_acquire_retry4(2, -ENOSYS) && !rdna4x_acquire_retry4(2, -EIO)); }
    // ---- completion-time plan: present only when the final instance equals the slot's
    { rdna4m6_t o; memset(&o, 0, sizeof o); o.latch = 1;
      const uint32_t e[5][3] = { { 101, 0, 0 }, { 201, 2, 0 }, { 401, 1, 0 }, { 301, 0, RDNA4M6_ENT_AMBIGUOUS }, { 202, 2, 0 } };
      size_t n = blob1(b, 5, e); rdna4m6_parse(&o, b, n);
      CHECK("complete: latch OFF presents everything", ({ rdna4m6_t z; memset(&z, 0, sizeof z); rdna4x_complete_plan(&z, 5, 2, 0, 1, 0x6) == RDNA4X_C_PRESENT; }));
      CHECK("complete: a DP frame of a DP surface -> PRESENT", rdna4x_complete_plan(&o, 101, 0, 0, 0, 0x6) == RDNA4X_C_PRESENT);
      CHECK("complete: a monitor B frame of a monitor B surface -> PRESENT; an monitor A frame of an monitor A surface -> PRESENT", rdna4x_complete_plan(&o, 201, 2, 0, 0, 0x6) == RDNA4X_C_PRESENT && rdna4x_complete_plan(&o, 401, 1, 0, 0, 0x6) == RDNA4X_C_PRESENT);
      CHECK("complete: a monitor B-slot frame whose surface is the DP's -> MISMATCH, never presented", rdna4x_complete_plan(&o, 101, 2, 0, 0, 0x6) == RDNA4X_C_MISMATCH && !rdna4x_may_present(RDNA4X_C_MISMATCH));
      CHECK("complete: an MONA-slot frame whose surface is the monitor B's (and the reverse) -> MISMATCH: monitor A frames go only to monitor A slots", rdna4x_complete_plan(&o, 201, 1, 0, 0, 0x6) == RDNA4X_C_MISMATCH && rdna4x_complete_plan(&o, 401, 2, 0, 0, 0x6) == RDNA4X_C_MISMATCH && rdna4x_complete_plan(&o, 101, 1, 0, 0, 0x6) == RDNA4X_C_MISMATCH);
      CHECK("complete: a TENTATIVE DP-slot frame whose surface is the monitor B's -> RECOPY, the monitor A's -> RECOPY (drop + remember), never presented", rdna4x_complete_plan(&o, 201, 0, 0, 0, 0x6) == RDNA4X_C_RECOPY && rdna4x_complete_plan(&o, 401, 0, 0, 0, 0x6) == RDNA4X_C_RECOPY && !rdna4x_may_present(RDNA4X_C_RECOPY));
      CHECK("complete: ... with THAT instance OFF it is a plain MISMATCH (nothing to re-copy to)", rdna4x_complete_plan(&o, 201, 0, 0, 0, 0x2) == RDNA4X_C_MISMATCH && rdna4x_complete_plan(&o, 401, 0, 0, 0, 0x4) == RDNA4X_C_MISMATCH);
      CHECK("complete: an ambiguous surface -> SKIP at every slot instance", rdna4x_complete_plan(&o, 301, 0, 0, 0, 0x6) == RDNA4X_C_SKIP && rdna4x_complete_plan(&o, 301, 1, 0, 0, 0x6) == RDNA4X_C_SKIP && rdna4x_complete_plan(&o, 301, 2, 0, 0, 0x6) == RDNA4X_C_SKIP);
      { int ok = 1; for (int t = 0; t < RDNA4X_REFRESH_TRIES; t++) ok = ok && rdna4x_complete_plan(&o, 999, 0, t, 0, 0x6) == RDNA4X_C_RETRY && rdna4x_complete_plan(&o, 999, 1, t, 0, 0x6) == RDNA4X_C_RETRY && rdna4x_complete_plan(&o, 999, 2, t, 0, 0x6) == RDNA4X_C_RETRY; CHECK("complete: an UNKNOWN surface is RETRIED (tries 0..7), at every instance", ok); }
      CHECK("complete: ... and DROPPED after the last try, never presented", rdna4x_complete_plan(&o, 999, 0, RDNA4X_REFRESH_TRIES, 0, 0x6) == RDNA4X_C_DROP && rdna4x_complete_plan(&o, 999, 1, 50, 0, 0x6) == RDNA4X_C_DROP && rdna4x_complete_plan(&o, 999, 2, 50, 0, 0x6) == RDNA4X_C_DROP);
      CHECK("complete: a STALE table never lets a frame through: RETRY, then DROP", rdna4x_complete_plan(&o, 101, 0, 0, 1, 0x6) == RDNA4X_C_RETRY && rdna4x_complete_plan(&o, 201, 2, 3, 1, 0x6) == RDNA4X_C_RETRY && rdna4x_complete_plan(&o, 401, 1, 3, 1, 0x6) == RDNA4X_C_RETRY && rdna4x_complete_plan(&o, 101, 0, RDNA4X_REFRESH_TRIES, 1, 0x6) == RDNA4X_C_DROP && rdna4x_complete_plan(&o, 401, 1, RDNA4X_REFRESH_TRIES, 1, 0x6) == RDNA4X_C_DROP);
      CHECK("complete: the matrix: PRESENT only when slot_inst == the table's instance (0, 1 and 2, every surface)", ({ int ok = 1; const uint32_t ids[] = { 101, 201, 202, 401, 301, 999 };
          for (unsigned i = 0; i < 6; i++) for (int si = 0; si <= 2; si++) { const int p = rdna4x_complete_plan(&o, ids[i], si, 0, 0, 0x6); const rdna4m6_ent_t *en = rdna4m6_find(&o, ids[i]); const int expect = en && !(en->flags & RDNA4M6_ENT_AMBIGUOUS) && (int)en->inst == si;
              if ((p == RDNA4X_C_PRESENT) != expect) ok = 0; } ok; })); }
    // ---- R2: the property generation; refreshes out of order; the stale rule
    { rdna4m6_t s; memset(&s, 0, sizeof s);
      const uint32_t e1[2][3] = { { 101, 0, 0 }, { 201, 2, 0 } }, e2[1][3] = { { 101, 0, 0 } };
      size_t n5 = blob2(b, 5, 2, e1);
      CHECK("R2: a version-2 blob parses with its generation", rdna4m6_parse(&s, b, n5) == 0 && s.gen == 5 && s.have_gen && s.n == 2);
      uint8_t c[128]; size_t n4 = blob2(c, 4, 1, e2);
      CHECK("R2: an OLDER generation is refused (-2) and the cached table is KEPT", rdna4m6_parse(&s, c, n4) == -2 && s.gen == 5 && s.n == 2 && s.store_refused == 1);
      size_t n6 = blob2(c, 6, 1, e2);
      CHECK("R2: a NEWER generation replaces it", rdna4m6_parse(&s, c, n6) == 0 && s.gen == 6 && s.n == 1);
      size_t n6b = blob2(c, 6, 2, e1);
      CHECK("R2: the SAME generation is accepted (>=)", rdna4m6_parse(&s, c, n6b) == 0 && s.n == 2 && s.gen == 6);
      CHECK("R2: generation wrap: 0xFFFFFFFF -> 1 is NEWER", ({ rdna4m6_t w; memset(&w, 0, sizeof w); blob2(c, 0xFFFFFFFFu, 1, e2); rdna4m6_parse(&w, c, 20); blob2(c, 1, 2, e1); rdna4m6_parse(&w, c, 12 + 24) == 0 && w.gen == 1 && w.n == 2; }));
      // the two refresh paths store through rdna4m6_store, with a scratch table parsed outside the lock
      rdna4m6_t cached; memset(&cached, 0, sizeof cached); rdna4m6_t sa, sb; memset(&sa, 0, sizeof sa); memset(&sb, 0, sizeof sb);
      blob2(c, 9, 2, e1); rdna4m6_parse(&sa, c, 12 + 24); blob2(c, 7, 1, e2); rdna4m6_parse(&sb, c, 12 + 12);
      CHECK("R2: store: the NEWER read (9) goes in first", rdna4m6_store(&cached, &sa) == 0 && cached.gen == 9 && cached.n == 2);
      CHECK("R2: store: the OLDER read (7) completing LATER is refused and does not overwrite", rdna4m6_store(&cached, &sb) == -2 && cached.gen == 9 && cached.n == 2 && cached.store_refused == 1);
      CHECK("R2: store: a version-1 read always stores (the latch-OFF kernel)", ({ rdna4m6_t v1, d; memset(&v1, 0, sizeof v1); memset(&d, 0, sizeof d); blob1(c, 1, e2); rdna4m6_parse(&v1, c, 8 + 12); rdna4m6_store(&d, &v1) == 0 && d.n == 1 && !d.have_gen; }));
      // stale rule
      rdna4m6_t t; memset(&t, 0, sizeof t);
      CHECK("R2: status gen 0 (no information) is never stale", !rdna4m6_table_stale(&t, 0) && !rdna4m6_table_stale(&cached, 0));
      CHECK("R2: no table yet + a published generation -> stale", rdna4m6_table_stale(&t, 3));
      CHECK("R2: the cache is behind the status -> stale; equal or ahead -> current", rdna4m6_table_stale(&cached, 10) && !rdna4m6_table_stale(&cached, 9) && !rdna4m6_table_stale(&cached, 8));
      // a blob bigger than the table, or a v2 header with a short length, is still refused
      CHECK("R2: a v2 blob with a short header is refused", ({ rdna4m6_t z; memset(&z, 0, sizeof z); rdna4m6_parse(&z, b, 10) == -1; })); }
    // ---- the 1 s tick: the keep-alive is unconditional while active
    { rdna4m6_t o; memset(&o, 0, sizeof o); o.latch = 1; rdna4x_tick_t t = { 0 };
      CHECK("tick: instance 2 inactive -> nothing (not even a status call)", rdna4x_tick_plan(&t, &o) == 0);
      t.x_active = 1;
      CHECK("tick: ACTIVE -> Status2 EVERY tick (the keep-alive), whatever else", (rdna4x_tick_plan(&t, &o) & RDNA4X_T_STATUS) != 0);
      t.recopy_pending = 1; t.status_gen_known = 1; t.status_gen = 4;
      { const uint32_t a = rdna4x_tick_plan(&t, &o); CHECK("tick: + a stale table -> a refresh; + a pending re-copy -> the re-copy; the keep-alive stays", (a & RDNA4X_T_STATUS) && (a & RDNA4X_T_REFRESH) && (a & RDNA4X_T_RECOPY) && !(a & RDNA4X_T_RELEASE)); }
      t.killfile = 1; CHECK("tick: the kill file -> RELEASE and nothing else (no keep-alive of a plane that is being given back)", rdna4x_tick_plan(&t, &o) == RDNA4X_T_RELEASE);
      { int calls = 0; for (int sec = 0; sec < 30; sec++) { rdna4x_tick_t k = { .x_active = 1 }; if (rdna4x_tick_plan(&k, &o) & RDNA4X_T_STATUS) calls++; } CHECK("tick: 30 seconds of a STATIC monitor B make 30 keep-alive calls (the kernel's idle limit is 5 s)", calls == 30); } }
    // ---- the re-copy memory
    { rdna4x_stats_t x; memset(&x, 0, sizeof x);
      rdna4x_recopy_note(&x, 201, 77); CHECK("recopy: noted", x.recopy_pending && x.recopy_sid == 201 && x.recopy_seq == 77 && x.recopy_noted == 1);
      rdna4x_recopy_done(&x, 1); CHECK("recopy: done once", !x.recopy_pending && x.recopy_done == 1 && x.recopy_fail == 0);
      rdna4x_recopy_note(&x, 202, 78); rdna4x_recopy_done(&x, 0); CHECK("recopy: a failed one is counted and cleared", !x.recopy_pending && x.recopy_fail == 1 && x.recopy_done == 1); }
    // ---- R6: a tentative frame is a FULL copy and never the chain head
    { rdna4df_t s; rdna4df_init(&s, 0); rdna4df_activate(&s, 3); s.damage = 1;
      uint32_t fl[3] = { RDNA4DF_SLOT_REUSABLE, RDNA4DF_SLOT_REUSABLE, RDNA4DF_SLOT_REUSABLE };
      rdna4df_plan_t p; rdna4df_rect_t full = { 0, 0, 2560, 1440 }, part = { 10, 10, 50, 50 };
      CHECK("R6: a normal PART frame is chained as before (first frame full, head set)", rdna4df_plan_ex(&s, fl, RDNA4DF_K_PART, part, 0, &p) >= 0 && p.kind == RDNA4DF_K_FULL && s.head == p.slot);
      const int h0 = s.head;
      CHECK("R6: the next normal PART frame chains onto it", rdna4df_plan_ex(&s, fl, RDNA4DF_K_PART, part, 0, &p) >= 0 && p.kind == RDNA4DF_K_PART && p.base == h0);
      const int s2 = p.slot;
      CHECK("R6: a TENTATIVE partial frame is a FULL copy, with no base", rdna4df_plan_ex(&s, fl, RDNA4DF_K_PART, part, 1, &p) >= 0 && p.kind == RDNA4DF_K_FULL && p.base == -1 && p.tent == 1);
      CHECK("R6: ... and it is NOT the chain head (head -1: the next frame restarts)", s.head == -1 && s.head != p.slot);
      const int tslot = p.slot;
      for (int k = 0; k < 3; k++) if (k != tslot) rdna4df_release_slot(&s, k);   // the two earlier frames completed; the tentative one is still in flight
      CHECK("R6: the frame after a tentative one does NOT chain onto its slot (a full copy, base -1)", rdna4df_plan_ex(&s, fl, RDNA4DF_K_PART, part, 0, &p) >= 0 && p.base == -1 && p.kind == RDNA4DF_K_FULL && p.slot != tslot);
      (void)s2; (void)full;
      CHECK("R6: rdna4df_plan (the old entry point) is plan_ex with tentative 0", ({ rdna4df_t q; rdna4df_init(&q, 0); rdna4df_activate(&q, 3); q.damage = 1; rdna4df_plan_t pa, pb; int a = rdna4df_plan(&q, fl, RDNA4DF_K_PART, part, &pa);
          rdna4df_t q2; rdna4df_init(&q2, 0); rdna4df_activate(&q2, 3); q2.damage = 1; int bq = rdna4df_plan_ex(&q2, fl, RDNA4DF_K_PART, part, 0, &pb); a == bq && pa.kind == pb.kind && q.head == q2.head; }));
      CHECK("R6: the pure helpers", rdna4x_kind_for(1, RDNA4DF_K_FULL, RDNA4DF_K_PART) == RDNA4DF_K_FULL && rdna4x_kind_for(0, RDNA4DF_K_FULL, RDNA4DF_K_PART) == RDNA4DF_K_PART && !rdna4x_may_chain(1) && rdna4x_may_chain(0)); }
    // ---- source pins on the glue
    { char *m = slurp(dir, "GarnetDevice.m"), *pl = slurp(dir, "Info.plist"), *ins = slurp(dir, "../../CHANGES.md"), *abi = slurp(dir, "rdna4_scanabi.h"), *kp = slurp(dir, "../../src/garnet/src/amd/garnet_multidisplay_route_pure.h"), *ka = slurp(dir, "../../src/garnet/src/GarnetNativeABI.h"), *xh = slurp(dir, "rdna4_m6x.h");
      // the state: an array over the instance numbers, one descriptor lookup
      CHECK("glue: the HDMI state is an ARRAY over the kernel instance numbers (RDNA4X.i[inst]); no singleton RDNA4X.sm / .enabled remains", strstr(m, "rdna4xi_t i[3]; } RDNA4XScanoutState;") && strstr(m, "GARNET_INTERNAL RDNA4XScanoutState RDNA4X;") && strstr(m, "#define RDNA4XI(inst) (RDNA4X.i[(inst)])") && !strstr(m, "RDNA4X.sm") && !strstr(m, "RDNA4X.enabled") && !strstr(m, "RDNA4X.xs") && !strstr(m, "RDNA4X.sid") && !strstr(m, "RDNA4X.buf") && !strstr(m, "RDNA4X.mem") && !strstr(m, "RDNA4X_PLAN_MONB ? &RDNA4X"));
      // enable at bind, per instance
      CHECK("glue: each HDMI instance is enabled ONLY through rdna4x_enabled, at bind, from the kernel latches (the third for the monitor A), both exports, ITS kill file - with NO dependence on the DP's plane geometry", strstr(m, "RDNA4XI(xi).enabled = rdna4x_enabled(&xe);") && strstr(m, "radv_darwin_rdna4n_call") && strstr(m, "radv_darwin_rdna4n_bo_handle") && strstr(m, ".killfile = stat(d->killfile, &xs_) == 0") && strstr(m, ".kernel_m6flip = (q.flags & RDNA4X_SCANQ_M6FLIP) != 0, .kernel_m6flip1 = (q.flags & RDNA4X_SCANQ_M6FLIP1) != 0") && !strstr(m, "rdna4x_geom_ok(q.plane_w"));
      // acquire: the driver, no DP operation
      { const char *a = after(m, "static BOOL rdna4x_acquire_locked(int inst) {"); const char *e = a ? strstr(a, "GARNET_INTERNAL int rdna4s_damage_on(void)") : NULL; char *seg = a && e ? strndup(a, (size_t)(e - a)) : strdup("");
        CHECK("glue: the acquire is rdna4x_acquire_run over the real operations, including the geometry cross-check", strstr(seg, "rdna4x_acquire_run(&ops) == 0") && strstr(seg, ".pool_verdict = rdna4x_op_pool") && strstr(seg, ".geom_verdict = rdna4x_op_geom") && strstr(seg, ".off = rdna4x_op_off"));
        CHECK("glue: the acquire body never fails the DP closed and never releases it (only instance 0's own acquire runs first)", strstr(seg, "rdna4s_off_locked(") == NULL && strstr(seg, "RDNA4S.fr(") == NULL && strstr(seg, "rdna4s_do_release_locked(") == NULL && strstr(seg, "RDNA4S.sm.state = RDNA4DF_OFF") == NULL);
        CHECK("glue: the DP must already be ACTIVE (the kernel needs the session to hold instance 0)", strstr(seg, "if (RDNA4S.sm.state != RDNA4DF_ACTIVE) { rdna4x_off_locked(inst,") != NULL); free(seg); }
      { const char *a = after(m, "static int rdna4x_op_pool(void *c) {"); const char *e = a ? strstr(a, "static int rdna4x_op_acquire(") : NULL; char *seg = a && e ? strndup(a, (size_t)(e - a)) : strdup("");
        CHECK("glue (S4): the pool budget is the KERNEL's free visible-VRAM figure (SCANX_ACQUIRE out[3]) with the margin, per instance (the descriptor's slot size) - never RADV's heap size", strstr(seg, ".free_bytes = a->ao[3]") && strstr(seg, ".slot_bytes = d->slot_bytes") && strstr(seg, "rdna4x_pool_verdict(&pool)") && strstr(seg, ".margin_bytes = RDNA4X_MARGIN_BYTES") && !strstr(seg, "memoryHeaps") && !strstr(seg, "RDNA4S.slotBytes")); free(seg); }
      CHECK("glue (item 9): SCANX_ACQUIRE asks for FIVE outputs (the fifth = the geometry), a monitor B may fall back to four, the monitor A never", strstr(m, "const uint64_t in[2] = { (uint64_t)inst, 0 }; uint32_t no = 5;") && strstr(m, "if (rdna4x_acquire_retry4(inst, rc)) { no = 4;") && strstr(m, "const int ok = rdna4x_acq_geom_ok(d, a->nout, a->ao[4]);"));
      CHECK("glue: slot registration uses the instance's tagged-id check, its geometry and the kernel's selector", strstr(m, "if (!rdna4x_id_ok((uint32_t)a->inst, sl))") && strstr(m, "rdna4x_k_register(a->inst, X->mem[i], d->pitch_bytes, d->w, d->h, &sl, &mc)"));
      { const char *a = after(m, "static int rdna4x_op_alloc(void *c, int i) {"); const char *e = a ? strstr(a, "static int rdna4x_op_register(") : NULL; char *seg = a && e ? strndup(a, (size_t)(e - a)) : strdup("");
        CHECK("glue (S8): the HDMI slots are allocated with the DESCRIPTOR'S slot size, never the DP's", strstr(seg, ".size = d->slot_bytes, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT") && !strstr(seg, "RDNA4S.slotBytes")); free(seg); }
      // the selector calls are exactly the five, each with in[0] = the instance
      CHECK("glue: the five selector calls, each with in[0] = the instance", strstr(m, "RDNA4N_SEL_SCANX_ACQUIRE, in, 2") && strstr(m, "RDNA4N_SEL_SCANX_REGISTER, in, 1, &reg, sizeof reg") && strstr(m, "RDNA4N_SEL_SCANX_PRESENT, in, 3") && strstr(m, "RDNA4N_SEL_SCANX_STATUS, in, 1") && strstr(m, "RDNA4N_SEL_SCANX_RELEASE, in, 1") &&
            countof(m, "{ (uint64_t)inst") >= 5 && !strstr(m, "RDNA4X_INSTANCE"));
      CHECK("glue: the HDMI instances are never presented through the DP's fp / fg / fa", ({ const char *a = after(m, "static int rdna4x_k_acquire("); const char *e = a ? strstr(a, "static void rdna4x_do_release_locked(int inst)") : NULL; char *seg = a && e ? strndup(a, (size_t)(e - a)) : strdup(""); int ok = strstr(seg, "RDNA4S.fp(") == NULL && strstr(seg, "RDNA4S.fg(") == NULL && strstr(seg, "RDNA4S.fa(") == NULL; free(seg); ok; }));
      // encode: planned instance reaches the plan and the slot
      CHECK("glue: the encode path plans the instance the gate chose, and records it on the command buffer", strstr(m, "BOOL take = rdna4s_disp_account(dmask, [dt rdna4DispSid], dfns, atomic_fetch_add(&RDNA4S.cbno, 1) + 1, dwr, &pinst, &ptent);") && strstr(m, "int sl = take ? rdna4s_plan(dwr, pinst, ptent, &pl) : -1;") && strstr(m, "_dinst = pl.inst;"));
      CHECK("glue: the completion is told the instance (both the normal and the never-presented paths)", strstr(m, "rdna4s_complete(_dinst, s, r, _dseq, _dsid, rec, _dcls, _tcommit);") && strstr(m, "rdna4s_complete(_dinst, s, VK_ERROR_UNKNOWN, 0, 0, NULL, 0, 0);"));
      { const char *a = after(m, "- (void)rdna4DispSeq {"); const char *e = a ? strstr(a, "\n}") : NULL; char *seg = a && e ? strndup(a, (size_t)(e - a)) : strdup("");
        CHECK("glue: the submit-time bookkeeping goes to the instance's own machine", strstr(seg, "rdna4df_t *const dsm = rdna4x_desc((uint32_t)_dinst) ? &RDNA4XI(_dinst).sm : &RDNA4S.sm;") && strstr(seg, "if (_dsid) rdna4df_submit(dsm, _dslot, _dsid, _dseq);") && strstr(seg, "rdna4df_chain_submit(dsm, _dslot, _dcid, _dbid);")); free(seg); }
      CHECK("glue: the CRC diagnostic is the DP's alone", strstr(m, "if (rdna4s_crc_on() && pinst == RDNA4X_PLAN_DP) { _dcrc = YES; _dtex = dt; }") && strstr(m, "&& _dtex && _dinst == 0) ? rdna4s_crc_check("));
      // THE COPY RECTANGLE (S8): the instance's
      { const char *a = after(m, "GARNET_INTERNAL void rdna4s_record_copy("); const char *e = a ? strstr(a, "// ---- opt-in per-frame CRC diagnostic") : NULL; char *seg = a && e ? strndup(a, (size_t)(e - a)) : strdup("");
        CHECK("glue (S8): the buffer copy of an HDMI plan is the INSTANCE'S frame (pitch x height of its descriptor), into ITS slot buffers", strstr(seg, "const rdna4x_desc_t *const xd = rdna4x_desc((uint32_t)pl->inst);") && strstr(seg, "VkDeviceSize all = xd ? (VkDeviceSize)rdna4x_frame_bytes(xd) : (VkDeviceSize)RDNA4S.pitch * RDNA4S.ph;") && strstr(seg, "VkBuffer *const dstv = xd ? RDNA4XI(pl->inst).buf : RDNA4S.buf;") && strstr(seg, "vkCmdCopyBuffer(cmd, src, dstv[slot], 1, &bc)")); free(seg); }
      { const char *a = after(m, "static void rdna4s_record_copy_img(VkCommandBuffer cmd, RDNA4Texture *t, const rdna4df_plan_t *pl) {"); const char *e = a ? strstr(a, "// Call OUTSIDE a render pass.") : NULL; char *seg = a && e ? strndup(a, (size_t)(e - a)) : strdup("");
        CHECK("glue (S8): the image copy uses the instance's rectangle (width, height, pitch) and slot buffer", strstr(seg, "const uint32_t gw = xd ? xd->w : RDNA4S.pw, gh = xd ? xd->h : RDNA4S.ph, gp = xd ? xd->pitch_bytes : RDNA4S.pitch;") && strstr(seg, "rdna4df_img_region(pl->base >= 0 ? pl->rect : full, gw, gh, gp, 4, &g)") && strstr(seg, "xd ? RDNA4XI(pl->inst).buf[slot] : RDNA4S.buf[slot]")); free(seg); }
      CHECK("glue (S8): the image-source precheck is judged on the PLANNED instance's geometry", strstr(m, "const uint32_t gw_ = pxd ? pxd->w : RDNA4S.pw, gh_ = pxd ? pxd->h : RDNA4S.ph, gp_ = pxd ? pxd->pitch_bytes : RDNA4S.pitch;") && strstr(m, "[dt width] == gw_ && [dt height] == gh_"));
      // the plan
      { const char *a = after(m, "GARNET_INTERNAL int rdna4s_plan(const rdna4df_wr_t *wr, int inst, int tent, rdna4df_plan_t *pl) {"); const char *e = a ? strstr(a, "GARNET_INTERNAL void rdna4s_record_copy(") : NULL; char *seg = a && e ? strndup(a, (size_t)(e - a)) : strdup("");
        const char *dp = strstr(seg, "if (inst == RDNA4X_PLAN_MONA || inst == RDNA4X_PLAN_MONB) {"), *xa = strstr(seg, "rdna4x_acquire_locked(inst)"), *xs = strstr(seg, "rdna4x_status_locked(inst, &X->st)"), *pe = strstr(seg, "rdna4df_plan_ex(&X->sm, fl, RDNA4DF_K_FULL, r, 0, pl)"), *dpa = strstr(seg, "rdna4s_acquire_locked()");
        CHECK("glue: an HDMI plan acquires lazily, reads Status (flags AND the table generation), plans a FULL copy on its own machine - all before the DP's code", dp && xa && xs && pe && dpa && dp < xa && xa < xs && xs < pe && pe < dpa);
        CHECK("glue: the plan's rectangle is the instance's", strstr(seg, "const rdna4df_rect_t r = { 0, 0, (int32_t)d->w, (int32_t)d->h };") != NULL);
        CHECK("glue: the HDMI plan asks for a refresh when the kernel published a newer table (after Status, before the plan)", strstr(seg, "if (RDNA4S.m6.latch && rdna4m6_table_stale(&RDNA4S.m6, X->gen)) rdna4s_m6_refresh_async();") && strstr(seg, "rdna4x_status_locked(inst, &X->st)") < strstr(seg, "rdna4m6_table_stale(&RDNA4S.m6, X->gen)"));
        CHECK("glue: the HDMI plan indexes the kernel's slots by the tagged id minus THE INSTANCE'S tag", strstr(seg, "X->st.slot[X->sid[i] - d->tag].flags") != NULL);
        CHECK("glue (R6): a tentative frame is a FULL copy on the DP's machine and not the chain head", strstr(seg, "k = rdna4x_kind_for(tent, RDNA4DF_K_FULL, k);") && strstr(seg, "rdna4df_plan_ex(&RDNA4S.sm, fl, k, r, tent, pl)")); free(seg); }
      // the gate
      { const char *a = after(m, "GARNET_INTERNAL BOOL rdna4s_disp_account("); const char *e = a ? strstr(a, "static void rdna4s_dirty_summary_locked") : NULL; char *seg = a && e ? strndup(a, (size_t)(e - a)) : strdup("");
        CHECK("glue: an HDMI plan comes ONLY from rdna4x_plan_inst over the enable mask (unknown never reaches an HDMI instance)", strstr(seg, "pi = rdna4x_plan_inst(&RDNA4S.m6, sid, xon, &tent);") && strstr(seg, "const uint32_t xon = rdna4x_onmask_locked();") && strstr(seg, "pi = RDNA4X_PLAN_MONB") == NULL && strstr(seg, "pi = RDNA4X_PLAN_MONA") == NULL);
        CHECK("glue (test 12): the SIZE GATE: an HDMI-planned surface whose size is not the instance's takes no slot and is counted (inst_geom_mismatch)", strstr(seg, "!rdna4x_plan_geom_ok(pi, gw, gh, gb, RDNA4S.pw, RDNA4S.ph, RDNA4S.pitch)) { RDNA4XI(pi).xs.inst_geom_mismatch++;") && strstr(seg, "if (pi != RDNA4X_PLAN_DP) RDNA4XI(pi).plan_count++;") && strstr(seg, "if (pi == RDNA4X_PLAN_DP || pi == RDNA4X_PLAN_MONA || pi == RDNA4X_PLAN_MONB) {") && strstr(seg, "pi = rdna4x_plan_inst(&RDNA4S.m6, sid, xon, &tent);\n        if (pi == RDNA4X_PLAN_DP || pi == RDNA4X_PLAN_MONA") && strstr(seg, "if (tent) RDNA4XI(0).xs.tentative_frames++;"));
        CHECK("glue (R-B1): the gate sits BEFORE the SKIP return and covers the DP's (tentative) plan", ({ const char *g = strstr(seg, "rdna4x_plan_geom_ok(pi, gw"); const char *sk = strstr(seg, "if (pi == RDNA4X_PLAN_SKIP)"); g && sk && g < sk; }));
        CHECK("glue: the frame is accounted on the planned instance's machine, damage accounting is the DP's alone", strstr(seg, "rdna4df_t *const am = apx ? &RDNA4XI(pi).sm : &RDNA4S.sm;") && strstr(seg, "if (wr && !apx) rdna4df_dmg_account(&RDNA4S.sm, c, wr, W, H);")); free(seg); }
      CHECK("glue (classify): a display-surface candidate is the DP's plane OR an enabled HDMI instance's geometry, and its geometry is remembered for the size gate", strstr(m, "rdna4x_classify_mask(RDNA4S.pw, RDNA4S.ph, RDNA4S.pitch, rdna4x_onmask_locked(), (uint32_t)w, (uint32_t)h, (uint32_t)bpr)") && strstr(m, "RDNA4S.sg[k].sid = sid; RDNA4S.sg[k].w = (uint32_t)w;"));
      // the completion
      { const char *a = after(m, "static void rdna4s_complete_run(int inst,"); const char *e = a ? strstr(a, "// Device selectors for mtlprobe dispflip") : NULL; char *seg = a && e ? strndup(a, (size_t)(e - a)) : strdup("");
        const char *cp = strstr(seg, "rdna4x_complete_plan(&RDNA4S.m6, sid, inst, tries, stale, xon)"), *hh = strstr(seg, "int hok = rdna4df_complete_held(sm,"), *pr = strstr(seg, "rdna4x_k_present(inst, sids[slot], o)");
        CHECK("glue: the final instance is judged BEFORE the state machine releases the slot, against the slot's own instance", cp && hh && pr && cp < hh && hh < pr);
        CHECK("glue: a stale table (ANY active HDMI instance's Status generation ahead of the cache) is refreshed async and the frame retried", strstr(seg, "if (RDNA4XI(xi).sm.state == RDNA4DF_ACTIVE && rdna4m6_table_stale(&RDNA4S.m6, RDNA4XI(xi).gen_known ? RDNA4XI(xi).gen : 0u)) stale = 1;") && strstr(seg, "if (stale) cx->xs.stale_retry++;") && strstr(seg, "rdna4s_m6_refresh_async();"));
        CHECK("glue: a mismatch / re-copy is counted (inst_mismatch) and the frame dropped before anything is presented; RECOPY notes the surface on the instance it belongs to", strstr(seg, "if (plan == RDNA4X_C_MISMATCH || plan == RDNA4X_C_RECOPY) cx->xs.inst_mismatch++;") && strstr(seg, "if (plan == RDNA4X_C_RECOPY) rdna4x_recopy_note(&cx->xs, sid, seq);") && strstr(seg, "rdna4xi_t *const cx = xinst ? &RDNA4XI(inst) : (fe && rdna4x_desc(fe->inst) ? &RDNA4XI(fe->inst) : &RDNA4XI(2));"));
        CHECK("glue: a failed HDMI present fails THAT instance closed ONLY (the DP's release is not called for it)", strstr(seg, "if (xinst) rdna4x_do_release_locked(inst); else rdna4s_do_release_locked();") != NULL);
        CHECK("glue: a present of an HDMI display cancels its pending re-copy", strstr(seg, "RDNA4XI(inst).xs.recopy_pending = 0;") != NULL);
        CHECK("glue: the completion never sleeps or reads the registry", strstr(seg, "nanosleep") == NULL && strstr(seg, "rdna4s_m6_read(") == NULL && strstr(seg, "usleep") == NULL); free(seg); }
      // the tick: keep-alive first, per instance, with ITS kill file
      { const char *a = after(m, "static void rdna4s_tick(void) {"); const char *e = a ? strstr(a, "static void rdna4s_atexit(void)") : NULL; char *seg = a && e ? strndup(a, (size_t)(e - a)) : strdup("");
        const char *lp = strstr(seg, "for (int xi = 1; xi <= 2; xi++) {   // bundle 13/15: each HDMI instance's 1 s duties"), *k = strstr(seg, "stat(d->killfile, &kb) == 0"), *tp = strstr(seg, "rdna4x_tick_plan(&tk, &RDNA4S.m6)"), *st = strstr(seg, "if (act & RDNA4X_T_STATUS) {"), *ks = strstr(seg, "(void)rdna4x_status_locked(xi, &X->st);"), *rf = strstr(seg, "if (act & RDNA4X_T_REFRESH) rdna4s_m6_refresh_async();"), *rc = strstr(seg, "if (act & RDNA4X_T_RECOPY) rdna4x_recopy_locked(xi);");
        CHECK("glue: THE KEEP-ALIVE: the 1 s tick calls Status for EVERY active HDMI instance (rdna4x_tick_plan says STATUS while active), then refreshes a stale table, then runs a pending re-copy", lp && k && tp && st && ks && rf && rc && lp < k && k < tp && tp < st && st < ks && ks < rf && rf < rc && strstr(seg, "X->xs.keepalive_calls++;"));
        CHECK("glue (S9): the kill file polled for an instance is THAT instance's own (the descriptor's), and releases only that instance", strstr(seg, "if (kill && X->enabled) { X->enabled = 0;") && strstr(seg, "rdna4x_off_locked(xi, RDNA4DF_R_KILL, \"kill switch file present\")") && !strstr(seg, "RDNA4X_NOFLIP2_FILE") && !strstr(seg, "RDNA4X_NOFLIP1_FILE")); free(seg); }
      CHECK("glue: the size gate refuses an HDMI-planned surface with no recorded geometry; the release-all and the enable mask cover BOTH HDMI instances; the completion's machine is the frame's instance", strstr(m, "if (!have || !rdna4x_plan_geom_ok(pi, gw, gh, gb, RDNA4S.pw, RDNA4S.ph, RDNA4S.pitch))") && strstr(m, "static void rdna4x_release_all_locked(void) { for (int i = 1; i <= 2; i++) rdna4x_do_release_locked(i); }") &&
            strstr(m, "static uint32_t rdna4x_onmask_locked(void) { uint32_t m = 0; for (int i = 1; i <= 2; i++) if (RDNA4XI(i).enabled && RDNA4XI(i).sm.state != RDNA4DF_OFF) m |= 1u << i; return m; }") && strstr(m, "rdna4df_t *const sm = xinst ? &RDNA4XI(inst).sm : &RDNA4S.sm;"));
      { const char *a = after(m, "- (BOOL)rdna4Finish:(NSError **)err {"); const char *e = a ? strstr(a, "- (void)rdna4PoolDone") : NULL; char *seg = a && e ? strndup(a, (size_t)(e - a)) : strdup("");
        CHECK("glue (R-B1): rdna4Finish refuses EVERY frame whose surface is not the plan's width x height (imgsrc or not), and a buffer-sourced one whose bytes-per-row is not the plan's pitch, BEFORE a slot is taken", strstr(seg, "if (take && !((imgsrc ? rdna4df_img_ok(gw_, gh_, gp_, 4) : (BOOL)YES) && [dt bytesPerPixel] == 4 && [dt width] == gw_ && [dt height] == gh_ && [dt rdna4Levels] == 1 && (imgsrc || [dt rdna4IOSBytesPerRow] == gp_))) {") && strstr(seg, "take = NO; }") && ({ const char *c = strstr(seg, "[dt width] == gw_"); const char *pl = strstr(seg, "rdna4s_plan(dwr, pinst, ptent, &pl)"); c && pl && c < pl; }));
        free(seg); }
      CHECK("glue (R-S2): the monitor B's four-word retry is taken only through rdna4x_acquire_retry4 (the -EINVAL shape refusal), never on any error", strstr(m, "if (rdna4x_acquire_retry4(inst, rc)) { no = 4;") && !strstr(m, "if (rc && inst == (int)RDNA4X_INST_MONB)"));
      CHECK("glue (S9): the descriptors carry the kill files in the right rows", strstr(xh, "{ RDNA4X_INST_MONA, 0x10u, 1920u, 1080u, 7680u,  8323072ull,  RDNA4X_NOFLIP1_FILE, \"monitor A\" },") && strstr(xh, "{ RDNA4X_INST_MONB, 0x20u, 2560u, 1440u, 10240u, 14745600ull, RDNA4X_NOFLIP2_FILE, \"monitor B\" } };"));
      CHECK("glue: the DP's release also gives every HDMI instance's A back; the explicit release request does too", strstr(m, "rdna4x_release_all_locked();   // bundle 13/15: the HDMI instances' A go back with the DP's console") && strstr(m, "for (int xi = 1; xi <= 2; xi++) if (RDNA4XI(xi).sm.acquired) { RDNA4XI(xi).sm.state = RDNA4DF_OFF; rdna4x_do_release_locked(xi); }"));
      // R2 in the glue
      CHECK("glue (R2): both refresh paths store through rdna4m6_store; no raw memcpy of the table remains", countof(m, "rdna4m6_store(&RDNA4S.m6, &tmp)") == 2 && countof(m, "memcpy(RDNA4S.m6.e") == 0);
      CHECK("glue: the re-copy is per instance, checks the surface against the INSTANCE'S geometry, drops the lock around the copy and releases the slot without a present when it fails", strstr(m, "static void rdna4x_recopy_locked(int inst) {\n    rdna4xi_t *const X = &RDNA4XI(inst);") && strstr(m, "IOSurfaceGetWidth(ios) == d->w && IOSurfaceGetHeight(ios) == d->h && IOSurfaceGetBytesPerRow(ios) == d->pitch_bytes") && strstr(m, "const int slot = rdna4df_pick(&X->sm, fl);") && strstr(m, "(void)rdna4df_complete_seq(&X->sm, slot, 1, seq);"));
      CHECK("glue (bundle 20, F4): every HDMI instance logs its class / non-final counts, table occupancy, scan_full_refused and surf_untracked every 10 s", strstr(m, "static void rdna4x_rubber_summary_locked(int xi) {") && strstr(m, "scan_full_refused %llu surf_untracked %llu") && strstr(m, "rdna4df_nonscan_used(&X->sm)") && strstr(m, "rdna4df_marked_count(&X->sm)") && strstr(m, "rdna4x_rubber_summary_locked(xi); } }") && strstr(m, "X->sm.cls[i] - pc[xi][i]") && strstr(m, "X->sm.nonfinal[i] - pn[xi][i]"));
      CHECK("glue (bundle 20, F3): the re-copy presents with the NOTED frame's sequence (never a fresh newest one) and the note carries it", strstr(m, "const uint64_t seq = x->recopy_seq;") && !strstr(strstr(m, "static void rdna4x_recopy_locked(int inst) {"), "atomic_fetch_add(&RDNA4S.seq, 1) + 1;\n    pthread_mutex_unlock") && strstr(m, "rdna4x_recopy_note(&cx->xs, sid, seq);"));
      // the mapping (S6) in rdna4_m6x.h
      CHECK("bundle (S6): the plan maps the table's MONA to instance 1 and its MONB to instance 2", strstr(xh, "if (e->inst == RDNA4M6_INST_MONA && rdna4x_on(xon_mask, RDNA4X_PLAN_MONA)) return RDNA4X_PLAN_MONA;") && strstr(xh, "if (e->inst == RDNA4M6_INST_MONB && rdna4x_on(xon_mask, RDNA4X_PLAN_MONB)) return RDNA4X_PLAN_MONB;"));
      // kernel mirrors
      CHECK("kernel: blob version 2, header 12, the generation under the gate", strstr(kp, "kBlobVersion2 = 2u, kBlobHdr2 = 12u") && strstr(kp, "blob_build_v2(SurfTable &t, uint32_t gen,") && strstr(kp, "__atomic_store_n(&g.curGen, seen, __ATOMIC_SEQ_CST);") && strstr(kp, "g.pubGen, seen"));
      CHECK("kernel ABI header carries the bits, the selectors and minor >= 12", strstr(ka, "#define RDNA4N_SCANQ_M6FLIP     (1u << 6)") && strstr(ka, "#define RDNA4N_SCANQ_M6FLIP1    (1u << 7)") && strstr(ka, "RDNA4N_SEL_SCANX_ACQUIRE  = 22") && strstr(ka, "RDNA4N_SEL_SCANX_RELEASE  = 26") && (strstr(ka, "#define RDNA4N_ABI_MINOR     12u") || strstr(ka, "#define RDNA4N_ABI_MINOR     13u")) && strstr(ka, "#define RDNA4N_SCANX_SLOT_TAG_MONA   0x10u"));
      CHECK("bundle's rdna4_scanabi.h mirrors them", strstr(abi, "#define RDNA4N_SCANQ_M6FLIP     (1u << 6)") && strstr(abi, "#define RDNA4N_SCANQ_M6FLIP1    (1u << 7)") && strstr(abi, "#define RDNA4N_SEL_SCANX_ACQUIRE  22u") && strstr(abi, "#define RDNA4N_SEL_SCANX_RELEASE  26u") && strstr(abi, "#define RDNA4N_SCANX_SLOT_TAG   0x20u") && strstr(abi, "#define RDNA4N_SCANX_SLOT_TAG_MONA 0x10u"));
      CHECK("bundle build 43 in Info.plist", strstr(pl, "<key>CFBundleVersion</key>\n\t<string>43</string>") != NULL);
      CHECK("CHANGES.md describes M6 Stage 2 enablement", strstr(ins, "**Stage 2**") && strstr(ins, "rdna4-m6flip1=1"));
      free(m); free(pl); free(ins); free(abi); free(kp); free(ka); free(xh); }
    printf("test-m6x: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
