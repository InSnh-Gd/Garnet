// test-m6route.c: bundle build 11 (M6 Stage 1a): garnet_m6route.h on the host - the kernel's IOSurface ID -> display table, the encode-time gate, the completion-time decision, the property parser - plus
// source pins on the glue in GarnetDevice.m (the ObjC cannot run on the host; the decisions it calls are exercised below, the pins prove it calls them in the right order).
// Build/run on the host Mac:  cc -O1 -D_GNU_SOURCE -Wall -Wextra -Werror -o /tmp/test-m6route test-m6route.c && /tmp/test-m6route .      (the argument is src/garnet-metal)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "garnet_scanabi.h"
#include "garnet_m6route.h"
static int fails, checks;
#define CHECK(name, cond) do { checks++; if (!(cond)) { fails++; printf("FAIL: %s\n", name); } } while (0)
static char *slurp(const char *dir, const char *rel) {
    char path[1024]; snprintf(path, sizeof path, "%s/%s", dir, rel);
    FILE *f = fopen(path, "rb"); if (!f) { printf("FAIL: cannot read %s\n", path); fails++; return strdup(""); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1); size_t r = fread(b, 1, (size_t)n, f); b[r] = 0; fclose(f); return b;
}
static size_t countof(const char *h, const char *n) { size_t c = 0; const char *p = h; while ((p = strstr(p, n))) { c++; p += strlen(n); } return c; }
static void w32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
// a property blob exactly as src/garnet/src/amd/garnet_multidisplay_route_pure.h blob_build lays it out
static size_t blob(uint8_t *out, uint32_t n, const uint32_t (*ents)[3]) {
    w32(out, RDNA4M6_BLOB_VERSION); w32(out + 4, n);
    for (uint32_t i = 0; i < n; i++) { w32(out + 8 + i * 12, ents[i][0]); w32(out + 12 + i * 12, ents[i][1]); w32(out + 16 + i * 12, ents[i][2]); }
    return 8 + (size_t)n * 12;
}
int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    uint8_t b[512]; rdna4m6_t s; memset(&s, 0, sizeof s);
    // ---- the property parser
    { const uint32_t e[4][3] = { { 101, 0, 0 }, { 201, 2, 0 }, { 202, 2, 0 }, { 301, 0, RDNA4M6_ENT_AMBIGUOUS } };
      size_t n = blob(b, 4, e);
      CHECK("parse: a good table", rdna4m6_parse(&s, b, n) == 0 && s.n == 4 && s.have_table && s.e[1].id == 201 && s.e[1].inst == 2 && s.e[3].flags == RDNA4M6_ENT_AMBIGUOUS);
      CHECK("counts per instance", rdna4m6_count_inst(&s, 0) == 2 && rdna4m6_count_inst(&s, 2) == 2 && rdna4m6_count_inst(&s, 1) == 0 && rdna4m6_count_ambig(&s) == 1);
      rdna4m6_t keep = s;
      CHECK("parse: a short blob is refused", rdna4m6_parse(&s, b, 4) == -1);
      CHECK("parse: a trailing byte is refused", rdna4m6_parse(&s, b, n + 1) == -1);
      CHECK("parse: a missing entry is refused", rdna4m6_parse(&s, b, n - 12) == -1);
      { uint8_t c[512]; memcpy(c, b, n); w32(c, 3); CHECK("parse: another version is refused", rdna4m6_parse(&s, c, n) == -1); }
      { uint8_t c[512]; memcpy(c, b, n); w32(c + 4, RDNA4M6_MAX_SURF + 1); CHECK("parse: a count above the table is refused", rdna4m6_parse(&s, c, n) == -1); }
      { uint8_t c[512]; memcpy(c, b, n); w32(c + 8, 0); CHECK("parse: an entry with ID 0 rejects the whole table", rdna4m6_parse(&s, c, n) == -1); }
      { uint8_t c[512]; memcpy(c, b, n); w32(c + 12 + 12, 3); CHECK("parse: an entry on instance 3 rejects the whole table", rdna4m6_parse(&s, c, n) == -1); }
      { uint32_t big[RDNA4M6_MAX_SURF + 1][3]; for (uint32_t i = 0; i < RDNA4M6_MAX_SURF + 1; i++) { big[i][0] = 1000 + i; big[i][1] = 0; big[i][2] = 0; }
        uint8_t c[8 + (RDNA4M6_MAX_SURF + 1) * 12]; size_t m = blob(c, RDNA4M6_MAX_SURF + 1, (const uint32_t (*)[3])big);
        CHECK("parse: a WELL-FORMED blob of 17 entries (length matches) is refused by the count alone", rdna4m6_parse(&s, c, m) == -1);
        size_t m16 = blob(c, RDNA4M6_MAX_SURF, (const uint32_t (*)[3])big); CHECK("parse: 16 entries fit exactly", rdna4m6_parse(&s, c, m16) == 0 && s.n == RDNA4M6_MAX_SURF); rdna4m6_parse(&s, b, n); }
      CHECK("parse: every refusal KEEPS the old table", memcmp(&keep.e, &s.e, sizeof s.e) == 0 && keep.n == s.n);
      CHECK("parse: NULL arguments", rdna4m6_parse(NULL, b, n) == -1 && rdna4m6_parse(&s, NULL, n) == -1); }
    // ---- the decisions with the latch OFF: build 10, whatever the table says
    { rdna4m6_t o; memset(&o, 0, sizeof o); const uint32_t e[2][3] = { { 201, 2, 0 }, { 301, 0, RDNA4M6_ENT_AMBIGUOUS } }; size_t n = blob(b, 2, e); rdna4m6_parse(&o, b, n);
      CHECK("latch OFF: the gate presents a surface the table maps to the monitor B", rdna4m6_gate(&o, 201) == RDNA4M6_PRESENT);
      CHECK("latch OFF: the gate presents an ambiguous surface", rdna4m6_gate(&o, 301) == RDNA4M6_PRESENT);
      CHECK("latch OFF: the gate presents an unknown surface", rdna4m6_gate(&o, 999) == RDNA4M6_PRESENT);
      CHECK("latch OFF: the completion presents all of them", rdna4m6_final(&o, 201) == RDNA4M6_PRESENT && rdna4m6_final(&o, 301) == RDNA4M6_PRESENT && rdna4m6_final(&o, 999) == RDNA4M6_PRESENT);
      CHECK("latch OFF: nothing is counted", o.gate_ok == 0 && o.gate_tentative == 0 && o.fin_ok == 0 && o.fin_unknown == 0); }
    // ---- the decisions with the latch ON
    { rdna4m6_t o; memset(&o, 0, sizeof o); o.latch = 1;
      CHECK("latch ON, no table yet: the gate lets a surface through TENTATIVELY", rdna4m6_gate(&o, 101) == RDNA4M6_TENTATIVE && o.gate_tentative == 1);
      CHECK("latch ON, no table yet: the completion presents NOTHING", rdna4m6_final(&o, 101) == RDNA4M6_SKIP_UNKNOWN && o.fin_unknown == 1);
      const uint32_t e[6][3] = { { 101, 0, 0 }, { 102, 0, 0 }, { 103, 0, 0 }, { 201, 2, 0 }, { 202, 2, 0 }, { 301, 0, RDNA4M6_ENT_AMBIGUOUS } }; size_t n = blob(b, 6, e); rdna4m6_parse(&o, b, n);
      memset(&o.gate_skip, 0, sizeof o.gate_skip); o.gate_ok = o.gate_tentative = 0;
      CHECK("latch ON: the DP's surface passes the gate", rdna4m6_gate(&o, 101) == RDNA4M6_PRESENT && o.gate_ok == 1);
      CHECK("latch ON: a monitor B surface takes NO slot (instance 2)", rdna4m6_gate(&o, 201) == RDNA4M6_SKIP_OTHER && o.gate_skip[2] == 1 && o.gate_skip[1] == 0);
      { const uint32_t a[1][3] = { { 401, 1, 0 } }; uint8_t c[64]; size_t m = blob(c, 1, a); rdna4m6_t p = o; rdna4m6_parse(&p, c, m); CHECK("latch ON: an monitor A surface takes NO slot (instance 1)", rdna4m6_gate(&p, 401) == RDNA4M6_SKIP_OTHER && p.gate_skip[1] == 1); }
      CHECK("latch ON: an AMBIGUOUS surface takes no slot", rdna4m6_gate(&o, 301) == RDNA4M6_SKIP_AMBIG && o.gate_ambig == 1);
      CHECK("latch ON: an unknown surface is let through tentatively", rdna4m6_gate(&o, 777) == RDNA4M6_TENTATIVE && o.gate_tentative == 1);
      CHECK("latch ON: surface ID 0 is unknown", rdna4m6_gate(&o, 0) == RDNA4M6_TENTATIVE && rdna4m6_final_peek(&o, 0) == RDNA4M6_SKIP_UNKNOWN);
      CHECK("latch ON: the completion presents the DP's surface", rdna4m6_final(&o, 102) == RDNA4M6_PRESENT && o.fin_ok == 1);
      CHECK("latch ON: the completion refuses a monitor B surface", rdna4m6_final(&o, 202) == RDNA4M6_SKIP_OTHER && o.fin_skip[2] == 1);
      CHECK("latch ON: the completion refuses an ambiguous surface", rdna4m6_final(&o, 301) == RDNA4M6_SKIP_AMBIG && o.fin_ambig == 1);
      CHECK("latch ON: the completion refuses an unknown surface (after the caller's refresh)", rdna4m6_final(&o, 888) == RDNA4M6_SKIP_UNKNOWN && o.fin_unknown == 2);
      CHECK("peek counts nothing", ({ uint64_t before = o.fin_ok + o.fin_unknown + o.fin_ambig; (void)rdna4m6_final_peek(&o, 101); (void)rdna4m6_final_peek(&o, 888); before == o.fin_ok + o.fin_unknown + o.fin_ambig; }));
      CHECK("only PRESENT may present", rdna4m6_may_present(RDNA4M6_PRESENT) && !rdna4m6_may_present(RDNA4M6_TENTATIVE) && !rdna4m6_may_present(RDNA4M6_SKIP_OTHER) && !rdna4m6_may_present(RDNA4M6_SKIP_AMBIG) && !rdna4m6_may_present(RDNA4M6_SKIP_UNKNOWN));
      // THE misroute this build exists to stop: a 2560x1440 monitor B surface is size-matched to the DP's plane; with the kernel's table it is never presented
      CHECK("a monitor B surface that matches HUBP0's size is never presented, at either stage", rdna4m6_gate(&o, 201) != RDNA4M6_PRESENT && rdna4m6_final(&o, 201) != RDNA4M6_PRESENT && rdna4m6_gate(&o, 202) != RDNA4M6_PRESENT && rdna4m6_final(&o, 202) != RDNA4M6_PRESENT);
      CHECK("a refresh that now knows the surface turns the tentative frame into a present", ({ const uint32_t a2[1][3] = { { 888, 0, 0 } }; uint8_t c[64]; size_t m = blob(c, 1, a2); rdna4m6_parse(&o, c, m); rdna4m6_final(&o, 888) == RDNA4M6_PRESENT; }));
      CHECK("... and a refresh that maps it elsewhere turns it into a skip", ({ const uint32_t a3[1][3] = { { 888, 2, 0 } }; uint8_t c[64]; size_t m = blob(c, 1, a3); rdna4m6_parse(&o, c, m); rdna4m6_final(&o, 888) == RDNA4M6_SKIP_OTHER; })); }
    // ---- bundle 12 (queue 290 S3): the completion plan, decided from the table as it is, with no sleep and no read
    { rdna4m6_t o; memset(&o, 0, sizeof o);
      CHECK("plan: latch OFF presents everything, whatever the table says", rdna4m6_complete_plan(&o, 5, 0) == RDNA4M6_C_PRESENT && rdna4m6_complete_plan(&o, 0, 99) == RDNA4M6_C_PRESENT);
      o.latch = 1;
      const uint32_t e[4][3] = { { 101, 0, 0 }, { 201, 2, 0 }, { 401, 1, 0 }, { 301, 0, RDNA4M6_ENT_AMBIGUOUS } }; size_t n = blob(b, 4, e); rdna4m6_parse(&o, b, n);
      CHECK("plan: instance 0 -> PRESENT", rdna4m6_complete_plan(&o, 101, 0) == RDNA4M6_C_PRESENT && rdna4m6_complete_plan(&o, 101, 7) == RDNA4M6_C_PRESENT);
      CHECK("plan: the monitor B's / the monitor A's / an ambiguous surface -> SKIP (at every try)", rdna4m6_complete_plan(&o, 201, 0) == RDNA4M6_C_SKIP && rdna4m6_complete_plan(&o, 401, 3) == RDNA4M6_C_SKIP && rdna4m6_complete_plan(&o, 301, 0) == RDNA4M6_C_SKIP && rdna4m6_complete_plan(&o, 201, 99) == RDNA4M6_C_SKIP);
      { int ok = 1; for (int t = 0; t < RDNA4M6_RETRIES; t++) ok = ok && rdna4m6_complete_plan(&o, 999, t) == RDNA4M6_C_RETRY; CHECK("plan: an UNKNOWN surface is RETRIED for tries 0..7", ok); }
      CHECK("plan: ... and DROPPED at the last try (8), never presented", rdna4m6_complete_plan(&o, 999, RDNA4M6_RETRIES) == RDNA4M6_C_DROP && rdna4m6_complete_plan(&o, 999, 50) == RDNA4M6_C_DROP && rdna4m6_complete_plan(&o, 0, 0) == RDNA4M6_C_RETRY);
      CHECK("plan: it counts NOTHING (the caller counts once, at the end)", o.fin_ok == 0 && o.fin_unknown == 0 && o.fin_ambig == 0 && o.gate_ok == 0);
      CHECK("plan: a refresh that now knows the ID turns a RETRY into a PRESENT", ({ const uint32_t a2[1][3] = { { 999, 0, 0 } }; uint8_t c[64]; size_t m = blob(c, 1, a2); rdna4m6_parse(&o, c, m); rdna4m6_complete_plan(&o, 999, 3) == RDNA4M6_C_PRESENT; }));
      CHECK("plan: the empty table (just reset) retries too", ({ rdna4m6_t z; memset(&z, 0, sizeof z); z.latch = 1; rdna4m6_complete_plan(&z, 5, 0) == RDNA4M6_C_RETRY; })); }
    // ---- constants: the bundle's copy equals the kernel's
    CHECK("retry plan", RDNA4M6_RETRIES == 8 && RDNA4M6_RETRY_NS == 3000000ull && RDNA4M6_C_PRESENT == 0 && RDNA4M6_C_SKIP == 1 && RDNA4M6_C_RETRY == 2 && RDNA4M6_C_DROP == 3 && RDNA4M6_MAX_SURF == 16u && RDNA4M6_NINST == 3u);
    CHECK("the scan_query flag is the kernel's bit 5", RDNA4M6_SCANQ_FLAG == RDNA4N_SCANQ_M6 && RDNA4N_SCANQ_M6 == (1u << 5) && RDNA4N_SCANQ_M6 != RDNA4N_SCANQ_DTO_VALID);
    { char *kp = slurp(dir, "../../src/garnet/src/amd/garnet_multidisplay_route_pure.h"), *ka = slurp(dir, "../../src/garnet/src/GarnetNativeABI.h");
      CHECK("kernel: blob version / header / entry size / table size", strstr(kp, "kBlobVersion = 1u, kBlobHdr = 8u, kBlobEnt = 12u") && strstr(kp, "constexpr uint32_t kMaxSurf = 16u;") && strstr(kp, "constexpr uint32_t kEntAmbiguous = 1u;"));
      CHECK("kernel: the property names", strstr(kp, "#define RDNA4M6_PROP_SURF    \"" RDNA4M6_PROP_SURF "\"") && strstr(kp, "#define RDNA4M6_PROP_LATCH   \"" RDNA4M6_PROP_LATCH "\""));
      CHECK("kernel: instance numbers (DP 0, monitor A 1, monitor B 2)", strstr(kp, "kInstDp = 0u, kInstMonA = 1u, kInstMonB = 2u") && RDNA4M6_INST_DP == 0 && RDNA4M6_INST_MONA == 1 && RDNA4M6_INST_MONB == 2);
      CHECK("kernel ABI header carries RDNA4N_SCANQ_M6 at bit 5", strstr(ka, "#define RDNA4N_SCANQ_M6         (1u << 5)") != NULL);
      free(kp); free(ka); }
    // ---- the glue in GarnetDevice.m, in the order that makes the decisions reachable
    { char *m = slurp(dir, "GarnetDevice.m"), *pl = slurp(dir, "Info.plist"), *ins = slurp(dir, "../../CHANGES.md");
      const char *ga = strstr(m, "GARNET_INTERNAL BOOL rdna4s_disp_account("); const char *fa = ga ? strstr(ga, "int nf0 = am->nflag_log, ok = rdna4df_account_sid(") : NULL; const char *gg = ga ? strstr(ga, "rdna4x_plan_inst(&RDNA4S.m6, sid, xon, &tent)") : NULL;
      CHECK("glue (bundle 20, F1): the account call passes known = latch on and NOT tentative", ga && strstr(ga, "rdna4df_account_sid(am, mask, sid, RDNA4S.m6.latch && !tent)") != NULL);
      CHECK("glue: the gate runs inside rdna4s_disp_account, under the lock, BEFORE the frame is accounted (before it can take a slot)", ga && fa && gg && gg < fa && strstr(ga, "if (pi == RDNA4X_PLAN_SKIP) { pthread_mutex_unlock(&RDNA4S.mu); return NO; }") && strstr(ga, "if (RDNA4S.m6.latch) {") < gg);
      const char *ca = strstr(m, "static void rdna4s_complete_run(int inst, int slot, VkResult r, uint64_t seq, uint32_t sid, rdna4crc_rec_t *rec, int cls, uint64_t tcommit, int tries) {");
      const char *cp = ca ? strstr(ca, "rdna4x_complete_plan(&RDNA4S.m6, sid, inst, tries, stale, xon)") : NULL, *hh = ca ? strstr(ca, "int hok = rdna4df_complete_held(") : NULL, *fc = ca ? strstr(ca, "rdna4m6_final_count(&RDNA4S.m6, sid, RDNA4M6_PRESENT)") : NULL;
      const char *crc = ca ? strstr(ca, "if (rec) rdna4s_crc_present(rec);") : NULL, *pres = ca ? strstr(ca, "int rc = xinst ? rdna4x_k_present(inst, sids[slot], o) : RDNA4S.fp(RDNA4R.dev, sids[slot], o);") : NULL, *ra = ca ? strstr(ca, "dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)RDNA4M6_RETRY_NS), RDNA4S.pq,") : NULL, *sk = ca ? strstr(ca, "rdna4s_m6_skip_locked(slot, sid, plan, sm);") : NULL;
      const char *ce = ca ? strstr(ca, "\n}\n") : NULL;
      const char *ok = pres && ce && pres < ce ? strstr(pres, "if (rc == 0) {") : NULL;
      const char *okEnd = ok && ok < ce ? strstr(ok, "\n            }") : NULL;
      CHECK("glue (S3): M6 verdict precedes slot completion; CRC precedes present; only a successful present is counted", ca && cp && hh && fc && crc && pres && cp < hh && hh < crc && crc < pres && pres < fc && ok && okEnd && ok < fc && fc < okEnd && okEnd < ce);
      CHECK("glue (S3): an unknown ID is queued again with dispatch_after on the present queue and the table refreshed ASYNCHRONOUSLY, a skip releases through rdna4df_skip_content, both BEFORE complete_held", ra && sk && cp < ra && ra < hh && sk < hh && strstr(ca, "rdna4s_m6_refresh_async();") && strstr(ca, "rdna4s_m6_refresh_async();") < ra && strstr(ca, "tries + 1"));
      { const char *e1 = ca ? strstr(ca, "// Device selectors for mtlprobe dispflip") : NULL; char *seg = ca && e1 ? strndup(ca, (size_t)(e1 - ca)) : strdup("");
        CHECK("glue (S3): the completion path never sleeps and never reads the registry or drops the lock for it", strstr(seg, "nanosleep") == NULL && strstr(seg, "rdna4s_m6_read(") == NULL && strstr(seg, "rdna4s_m6_refresh_locked(") == NULL && strstr(seg, "usleep") == NULL && countof(seg, "pthread_mutex_unlock(&RDNA4S.mu)") >= 3 && countof(seg, "pthread_mutex_lock(&RDNA4S.mu)") == 1 && countof(seg, "rdna4df_complete_held(") == 1 && countof(seg, "rdna4df_skip_content(") == 0 && countof(seg, "rdna4s_crc_free(rec);") == 2); free(seg); }
      CHECK("glue (S3): bundle 11's sleeping final decision is gone", strstr(m, "rdna4s_m6_final_locked") == NULL);
      { const char *ra2 = strstr(m, "static void rdna4s_m6_refresh_async(void) {"); const char *rd = ra2 ? strstr(ra2, "rdna4s_m6_read(&tmp)") : NULL, *lk = ra2 ? strstr(ra2, "pthread_mutex_lock(&RDNA4S.mu);") : NULL;
        CHECK("glue (S3): the async refresh reads the registry with NO lock held, stores under RDNA4S.mu, and runs at most one at a time", ra2 && rd && lk && rd < lk && strstr(ra2, "if (rc == 0) { if (rdna4m6_store(&RDNA4S.m6, &tmp) != 0 && RDNA4S.m6.store_refused <= 3)") && strstr(ra2, "atomic_exchange(&rdna4s_m6_refreshing, 1)") && strstr(ra2, "dispatch_get_global_queue(QOS_CLASS_UTILITY, 0)")); }
      { const char *sl = strstr(m, "static void rdna4s_m6_skip_locked(int slot, uint32_t sid, int plan, rdna4df_t *sm) {");
        CHECK("glue (S3): a skipped frame is counted once, logged (first 12) and its slot released through rdna4df_skip_content", sl && strstr(sl, "rdna4df_skip_content(sm, slot);") && strstr(sl, "rdna4m6_final_count(&RDNA4S.m6, sid,") && strstr(sl, "if (lg < 12)")); }
      const char *rf2 = strstr(m, "static void rdna4s_m6_refresh_locked(void) {"); const char *u1 = rf2 ? strstr(rf2, "pthread_mutex_unlock(&RDNA4S.mu);") : NULL, *rd = rf2 ? strstr(rf2, "rdna4s_m6_read(&tmp)") : NULL, *l1 = rf2 ? strstr(rf2, "pthread_mutex_lock(&RDNA4S.mu);") : NULL;
      CHECK("glue: the report's registry read happens with RDNA4S.mu DROPPED", rf2 && u1 && rd && l1 && u1 < rd && rd < l1);
      CHECK("glue: a successful refresh stores through rdna4m6_store (entries, count, table present, and since build 13 the generation, never an older one over a newer one), in BOTH refresh paths", countof(m, "if (rc == 0) { if (rdna4m6_store(&RDNA4S.m6, &tmp) != 0 && RDNA4S.m6.store_refused <= 3)") == 2 && countof(m, "memcpy(RDNA4S.m6.e") == 0);
      CHECK("glue: the table is the Metal nub's property, read through the accelerator port's parent, parsed by rdna4m6_parse", strstr(m, "CFSTR(RDNA4M6_PROP_SURF)") && strstr(m, "IORegistryEntryGetParentEntry((io_registry_entry_t)port, kIOServicePlane, &parent)") && strstr(m, "rc = rdna4m6_parse(tmp, CFDataGetBytePtr((CFDataRef)v), (size_t)CFDataGetLength((CFDataRef)v));"));
      CHECK("glue: the latch comes from scan_query's flags and from nowhere else", strstr(m, "RDNA4S.m6.latch = (q.flags & RDNA4N_SCANQ_M6) != 0;") && countof(m, "RDNA4S.m6.latch =") == 1);
      CHECK("glue: only WindowServer's admitted device records the accelerator port", strstr(m, "if (self && rdna4_is_ws() && !rdna4_acc_port) rdna4_acc_port = port;") != NULL);
      CHECK("glue: the periodic counter log carries 'surfaces for instance 1/2' and runs from the T1 report", strstr(m, "m6: surfaces for instance 1/2: %u/%u") && strstr(m, "rdna4_m6_report();   // bundle 11"));
      CHECK("glue: rdna4s_classify is unchanged (the size match still classifies; the table decides which display)", strstr(m, "ok = size && pitch && (bt || test);") != NULL);
      CHECK("bundle build 43 in Info.plist", strstr(pl, "<key>CFBundleVersion</key>\n\t<string>43</string>") != NULL);
      CHECK("CHANGES.md describes the M6 stages and enable flags", strstr(ins, "## Multi-display") && strstr(ins, "**Stage 1a**") && strstr(ins, "**Stage 1b**") && strstr(ins, "**Stage 2**") && strstr(ins, "rdna4-m6=1") && strstr(ins, "rdna4-m6flip=1") && strstr(ins, "rdna4-m6flip1=1"));
      free(m); free(pl); free(ins); }
    printf("test-m6route: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
