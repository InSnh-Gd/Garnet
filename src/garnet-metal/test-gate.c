// test-gate.c: rdna4_gate.h (GPU-apps G4, kext 0.0.640: the bundle's load policy outside WindowServer) on the host, plus source pins that tie the REAL GarnetDevice.m to it.
// Build/run on the host Mac (from this directory; argv[1] = the directory holding GarnetDevice.m and rdna4_gate.h, default "."):
//   cc -O1 -Wall -Wextra -Werror -o /tmp/test-gate test-gate.c && /tmp/test-gate .
// tests/../test-gate-plant.sh plants breaks in the REAL rdna4_gate.h and GarnetDevice.m and demands that this suite FAILS on each.
// Covers:
//   G1  process classes (WindowServer / RDNA4M_ALLOW root / other);
//   G2  the kill file: ENOENT / ENOTDIR = absent, EPERM / EACCES / anything else = UNREADABLE; WindowServer keeps today's rule (only a present file declines), everyone else FAILS CLOSED;
//   G3  the nub flags: WindowServer declines only on Ready == 0; everyone else needs Ready present and 1; AutoDisarmed == 1 declines everywhere;
//   G4  the kernel probe: every step must succeed, HUNG declines, only ExclusiveAccess is retried (bounded);
//   G5  the pipeline-cache path: WindowServer / root keep /private/var/tmp (or /private/tmp, or the test directory); an application gets its own file under the user cache dir, none without it;
//   G6  the spvcache-miss fallback: now also for an admitted application, still not for an unadmitted process;
//   G7  constants agree with the kernel ABI header (type, selectors, info size / flags offset);
//   G8  source pins on GarnetDevice.m: reachability and order of the gates, the probe last and OTHER-only, the admitted flag set only after the probe;
//   G9  (0.0.641, G4 review HIGH 2) the probe runs only for euid >= 501: root / daemons are declined by class, first, with 0.0.632's text and no kernel open;
//   G10 (0.0.641, G4 review LOW) the AIR dump directory: mkdir 0700, lstat, a real directory owned by the caller, never a chmod that follows a link.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <errno.h>
#include "rdna4_gate.h"
#include "../../src/garnet/src/GarnetNativeABI.h"

static int fails, runs;
#define CHECK(name, cond) do { int ok_ = (cond) ? 1 : 0; runs++; if (!ok_) { fails++; printf("FAIL: %s\n", name); } } while (0)

static char *slurp(const char *dir, const char *name) {
    char path[1024]; snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1); if (!b) { fclose(f); return NULL; }
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    b[n] = 0; fclose(f); return b;
}
static const char *fn_start(const char *src, const char *sig) { return strstr(src, sig); }
static int before(const char *a, const char *b) { return a && b && a < b; }
static int has(const char *from, const char *to, const char *needle) { const char *q = from ? strstr(from, needle) : NULL; return q != NULL && (to == NULL || q < to); }

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    // ---- G1 ----
    CHECK("G1: WindowServer is class WS even with RDNA4M_ALLOW", rdna4g_class(1, 0) == RDNA4G_CLASS_WS && rdna4g_class(1, 1) == RDNA4G_CLASS_WS);
    CHECK("G1: a root tool with RDNA4M_ALLOW=1 is ROOT", rdna4g_class(0, 1) == RDNA4G_CLASS_ROOT);
    CHECK("G1: anything else is OTHER", rdna4g_class(0, 0) == RDNA4G_CLASS_OTHER);
    // ---- G2 ----
    CHECK("G2: stat ok -> PRESENT", rdna4g_kill_state(0, 0) == RDNA4G_KILL_PRESENT && rdna4g_kill_state(0, EPERM) == RDNA4G_KILL_PRESENT);
    CHECK("G2: ENOENT / ENOTDIR -> ABSENT", rdna4g_kill_state(-1, ENOENT) == RDNA4G_KILL_ABSENT && rdna4g_kill_state(-1, ENOTDIR) == RDNA4G_KILL_ABSENT);
    CHECK("G2: EPERM (the sandbox) -> UNREADABLE", rdna4g_kill_state(-1, EPERM) == RDNA4G_KILL_UNREADABLE);
    CHECK("G2: EACCES / EIO / ELOOP -> UNREADABLE", rdna4g_kill_state(-1, EACCES) == RDNA4G_KILL_UNREADABLE && rdna4g_kill_state(-1, EIO) == RDNA4G_KILL_UNREADABLE && rdna4g_kill_state(-1, ELOOP) == RDNA4G_KILL_UNREADABLE);
    for (int ws = 0; ws <= 1; ws++) {
        CHECK("G2: a PRESENT kill file declines everyone", rdna4g_kill_declines(ws, RDNA4G_KILL_PRESENT, 0));
        CHECK("G2: a PRESENT kill file with the root test bypass does not decline", !rdna4g_kill_declines(ws, RDNA4G_KILL_PRESENT, 1));
        CHECK("G2: an ABSENT kill file declines nobody", !rdna4g_kill_declines(ws, RDNA4G_KILL_ABSENT, 0));
    }
    CHECK("G2: UNREADABLE does NOT decline WindowServer (today's rule, bit for bit)", !rdna4g_kill_declines(1, RDNA4G_KILL_UNREADABLE, 0));
    CHECK("G2: UNREADABLE declines every other process (fail CLOSED)", rdna4g_kill_declines(0, RDNA4G_KILL_UNREADABLE, 0));
    CHECK("G2: the bypass does not excuse an UNREADABLE file", rdna4g_kill_declines(0, RDNA4G_KILL_UNREADABLE, 1));
    // ---- G3 ----
    CHECK("G3: WindowServer declines only on Ready == 0", rdna4g_ready_declines(1, 0) && !rdna4g_ready_declines(1, 1) && !rdna4g_ready_declines(1, RDNA4G_FLAG_ABSENT) && !rdna4g_ready_declines(1, RDNA4G_FLAG_NONUB));
    CHECK("G3: another process needs Ready present and 1", !rdna4g_ready_declines(0, 1) && rdna4g_ready_declines(0, 0) && rdna4g_ready_declines(0, RDNA4G_FLAG_ABSENT) && rdna4g_ready_declines(0, RDNA4G_FLAG_NONUB));
    CHECK("G3: AutoDisarmed == 1 declines everyone, absent / 0 declines nobody", rdna4g_autodisarm_declines(0, 1) && rdna4g_autodisarm_declines(1, 1) && !rdna4g_autodisarm_declines(0, 0) && !rdna4g_autodisarm_declines(0, RDNA4G_FLAG_ABSENT) && !rdna4g_autodisarm_declines(1, RDNA4G_FLAG_NONUB));
    // ---- G4 ----
    CHECK("G4: all steps ok, not HUNG -> OK", rdna4g_probe_verdict(1, 0, 0, 0, 0, 2u) == RDNA4G_PROBE_OK);
    CHECK("G4: no service -> NOSVC", rdna4g_probe_verdict(0, 0, 0, 0, 0, 2u) == RDNA4G_PROBE_NOSVC);
    CHECK("G4: a refused open -> OPEN (any code)", rdna4g_probe_verdict(1, 0xe00002c1u, 0, 0, 0, 2u) == RDNA4G_PROBE_OPEN && rdna4g_probe_verdict(1, RDNA4G_KR_EXCLUSIVE, 0, 0, 0, 2u) == RDNA4G_PROBE_OPEN);
    CHECK("G4: a failed Hello -> HELLO", rdna4g_probe_verdict(1, 0, 0xe00002c7u, 0, 0, 2u) == RDNA4G_PROBE_HELLO);
    CHECK("G4: an unreadable QueryInfo -> INFO", rdna4g_probe_verdict(1, 0, 0, 0xe00002c0u, 0, 2u) == RDNA4G_PROBE_INFO);
    CHECK("G4: HUNG -> HUNG, other flag bits are fine", rdna4g_probe_verdict(1, 0, 0, 0, RDNA4G_INFO_HUNG, 2u) == RDNA4G_PROBE_HUNG && rdna4g_probe_verdict(1, 0, 0, 0, 6u, 3u) == RDNA4G_PROBE_OK);
    CHECK("G4: only ExclusiveAccess is retried", rdna4g_probe_retry(RDNA4G_KR_EXCLUSIVE, 0) && !rdna4g_probe_retry(0xe00002c1u, 0) && !rdna4g_probe_retry(0u, 0));
    CHECK("G4: the retry is bounded", rdna4g_probe_retry(RDNA4G_KR_EXCLUSIVE, RDNA4G_PROBE_RETRIES - 1) && !rdna4g_probe_retry(RDNA4G_KR_EXCLUSIVE, RDNA4G_PROBE_RETRIES));
    CHECK("G4: a root session (valid budget flags WITHOUT the APP bit, or none) is not an application", rdna4g_probe_verdict(1, 0, 0, 0, 0, 0u) == RDNA4G_PROBE_NOTAPP && rdna4g_probe_verdict(1, 0, 0, 0, 0, 1u) == RDNA4G_PROBE_NOTAPP);
    CHECK("G4: HUNG is judged before the role", rdna4g_probe_verdict(1, 0, 0, 0, RDNA4G_INFO_HUNG, 0u) == RDNA4G_PROBE_HUNG);
    { int ok = 1; for (int v = 0; v <= 6; v++) ok &= strcmp(rdna4g_probe_text(v), "?") != 0; CHECK("G4: every verdict has a text", ok); }
    // ---- G5 ----
    { char p[300];
      CHECK("G5: WindowServer: /private/var/tmp when writable", rdna4g_pcache_path(p, sizeof p, RDNA4G_CLASS_WS, NULL, 1, "/var/folders/xx/C/", "WindowServer") && !strcmp(p, "/private/var/tmp/rdna4m-pipecache.bin"));
      CHECK("G5: WindowServer: /private/tmp when /private/var/tmp is not writable", rdna4g_pcache_path(p, sizeof p, RDNA4G_CLASS_WS, NULL, 0, "/x/", "WindowServer") && !strcmp(p, "/private/tmp/rdna4m-pipecache.bin"));
      CHECK("G5: a root tool honours the test directory", rdna4g_pcache_path(p, sizeof p, RDNA4G_CLASS_ROOT, "/tmp/t", 1, "/x/", "mtlprobe") && !strcmp(p, "/tmp/t/rdna4m-pipecache.bin"));
      CHECK("G5: an application gets its OWN file in the user cache dir", rdna4g_pcache_path(p, sizeof p, RDNA4G_CLASS_OTHER, NULL, 1, "/var/folders/xx/C/", "mpv") && !strcmp(p, "/var/folders/xx/C/rdna4m-pipecache.mpv.bin"));
      CHECK("G5: ... even if /private/var/tmp is writable and a test dir is offered", rdna4g_pcache_path(p, sizeof p, RDNA4G_CLASS_OTHER, "/tmp/t", 1, "/c/", "app") && !strcmp(p, "/c/rdna4m-pipecache.app.bin"));
      CHECK("G5: the cache dir without a trailing slash still works", rdna4g_pcache_path(p, sizeof p, RDNA4G_CLASS_OTHER, NULL, 0, "/c", "app") && !strcmp(p, "/c/rdna4m-pipecache.app.bin"));
      CHECK("G5: an application with no user cache dir has NO cache (never /private/var/tmp)", rdna4g_pcache_path(p, sizeof p, RDNA4G_CLASS_OTHER, NULL, 1, "", "mpv") == 0 && p[0] == 0 && rdna4g_pcache_path(p, sizeof p, RDNA4G_CLASS_OTHER, NULL, 1, NULL, "mpv") == 0);
      CHECK("G5: path separators and odd characters in the program name are neutralised", rdna4g_pcache_path(p, sizeof p, RDNA4G_CLASS_OTHER, NULL, 1, "/c/", "../a b/c") && !strcmp(p, "/c/rdna4m-pipecache..._a_b_c.bin") && strchr(p + 3, '/') == NULL);
      CHECK("G5: an empty program name becomes app", rdna4g_pcache_path(p, sizeof p, RDNA4G_CLASS_OTHER, NULL, 1, "/c/", "") && !strcmp(p, "/c/rdna4m-pipecache.app.bin"));
      CHECK("G5: a long program name is cut to 40 characters", rdna4g_pcache_path(p, sizeof p, RDNA4G_CLASS_OTHER, NULL, 1, "/c/", "0123456789012345678901234567890123456789ZZZZ") && !strcmp(p, "/c/rdna4m-pipecache.0123456789012345678901234567890123456789.bin"));
      CHECK("G5: a buffer too small -> no cache, empty path", rdna4g_pcache_path(p, 20, RDNA4G_CLASS_WS, NULL, 1, "", "x") == 0 && p[0] == 0);
      CHECK("G5: who saves: WindowServer yes, root only with the switch, an application yes", rdna4g_pcache_save(RDNA4G_CLASS_WS, 0) == 1 && rdna4g_pcache_save(RDNA4G_CLASS_ROOT, 0) == 0 && rdna4g_pcache_save(RDNA4G_CLASS_ROOT, 1) == 1 && rdna4g_pcache_save(RDNA4G_CLASS_OTHER, 0) == 1);
    }
    // ---- G6 ----
    CHECK("G6: explicit root test switches allow placeholders", rdna4g_fallback_ok(0, 1, 0, 0) && rdna4g_fallback_ok(0, 0, 1, 0));
    CHECK("G6: admitted applications and WindowServer get temporary placeholders", rdna4g_fallback_ok(0, 0, 0, 1) && rdna4g_fallback_ok(1, 0, 0, 0) && rdna4g_fallback_ok(1, 0, 0, 1));
    CHECK("G6: an unadmitted process still gets the NSError (no fallback)", !rdna4g_fallback_ok(0, 0, 0, 0));
    // ---- G7 ----
    CHECK("G7: the open type is 'GRNT'", RDNA4G_UC_TYPE == RDNA4N_UC_TYPE);
    CHECK("G7: Hello / QueryInfo selectors", RDNA4G_SEL_HELLO == (unsigned)RDNA4N_SEL_HELLO && RDNA4G_SEL_QUERYINFO == (unsigned)RDNA4N_SEL_QUERYINFO);
    CHECK("G7: Hello's minor flag", RDNA4G_HELLO_F_MINOR == (unsigned long long)RDNA4N_HELLO_F_MINOR);
    CHECK("G7: the info size and the flags offset", RDNA4G_INFO_SIZE == sizeof(struct rdna4n_info) && RDNA4G_INFO_FLAGS_OFF == offsetof(struct rdna4n_info, flags));
    CHECK("G7: the HUNG bit", RDNA4G_INFO_HUNG == RDNA4N_INFO_HUNG);
    CHECK("G7: the budget flags word is info.reserved[3] and the APP bit is the ABI's", RDNA4G_INFO_BUDGET_OFF == offsetof(struct rdna4n_info, reserved) + 4u * RDNA4N_INFO_R_BUDGET_FLAGS && RDNA4G_BUDGET_APP == RDNA4N_BUDGET_APP);
    CHECK("G7: ExclusiveAccess is the IOReturn value", RDNA4G_KR_EXCLUSIVE == 0xe00002c5u);
    CHECK("G7: ABI minor is 12", RDNA4N_ABI_MINOR == 12u);
    // ---- G8 ----
    char *m = slurp(dir, "GarnetDevice.m");
    CHECK("G8: GarnetDevice.m is readable", m != NULL);
    if (m) {
        const char *adm = fn_start(m, "static const char *rdna4_admit(uint32_t port) {");
        const char *adm_end = adm ? strstr(adm, "\nstatic BOOL rdna4_headless(void)") : NULL;
        CHECK("G8: rdna4_admit is found", adm && adm_end);
        if (adm && adm_end) {
            const char *kill = strstr(adm, "rdna4g_kill_declines(isWs, ks, killIgnored)"), *rdy = strstr(adm, "rdna4g_ready_declines(isWs, rdy)"), *ad = strstr(adm, "rdna4g_autodisarm_declines(isWs"),
                       *cnt = strstr(adm, "rdna4_counter_tripped(&count)"), *probe = strstr(adm, "rdna4_app_probe(port)"), *flag = strstr(adm, "atomic_store(&rdna4_app_admitted, 1)"), *cls = strstr(adm, "if (cls == RDNA4G_CLASS_OTHER) {");
            CHECK("G8: every gate is in rdna4_admit", kill && rdy && ad && cnt && probe && flag && cls);
            CHECK("G8: order: kill file, Ready, AutoDisarmed, crash counter, then the kernel probe", before(kill, rdy) && before(rdy, ad) && before(ad, cnt) && before(cnt, probe));
            const char *whyc = strstr(adm, "if (why) return why;");
            CHECK("G8: the probe runs only for class OTHER, a refusal returns before the admitted flag, and the flag is set after the probe", whyc && before(cls, probe) && before(probe, whyc) && before(whyc, flag));
            CHECK("G8: the admitted flag is stored in exactly one place", strstr(m, "atomic_store(&rdna4_app_admitted, 1)") == strstr(adm, "atomic_store(&rdna4_app_admitted, 1)") && !strstr(strstr(m, "atomic_store(&rdna4_app_admitted, 1)") + 10, "atomic_store(&rdna4_app_admitted, 1)"));
            CHECK("G8: the old name-based refusal text is only reachable through the by-class decline (0.0.641: euid < 501)", !has(adm, adm_end, "process is not WindowServer and RDNA4M_ALLOW=1 (as root) is not set") && has(adm, adm_end, "return RDNA4G_DECLINE_BY_CLASS_TEXT;"));
            CHECK("G8: the plain stat()==0 test is gone (the kill file is read through rdna4g_kill_state)", !has(adm, adm_end, "if (stat(RDNA4_KILL_FILE, &st) == 0)") && has(adm, adm_end, "rdna4g_kill_state(srcRc, srcErr)"));
            CHECK("G8: the Ready test is the gate function, not a bare == 0", !has(adm, adm_end, "rdna4_nub_ready(port) == 0") && !has(adm, adm_end, "rdna4_nub_autodisarmed(port) == 1"));
        }
        const char *probe = fn_start(m, "static const char *rdna4_app_probe(uint32_t port) {");
        const char *probe_end = probe ? strstr(probe, "\n// NULL = admit, else the reason") : NULL;
        CHECK("G8: rdna4_app_probe is found", probe && probe_end);
        if (probe && probe_end) {
            const char *o = strstr(probe, "IOServiceOpen(svc, mach_task_self(), RDNA4G_UC_TYPE, &conn)"), *h = strstr(probe, "RDNA4G_SEL_HELLO"), *q = strstr(probe, "RDNA4G_SEL_QUERYINFO"), *c = strstr(probe, "IOServiceClose(conn)"), *v = strstr(probe, "rdna4g_probe_verdict(");
            CHECK("G8: the probe opens, says Hello, reads QueryInfo, closes, then judges", o && h && q && c && v && before(o, h) && before(h, q) && before(q, c) && before(c, v));
            CHECK("G8: the probe reads the APP role from the kernel's QueryInfo (never assumes it)", has(probe, probe_end, "memcpy(&budgetFlags, info + RDNA4G_INFO_BUDGET_OFF, sizeof budgetFlags);") && !has(probe, probe_end, "budgetFlags = 2") && !has(probe, probe_end, "budgetFlags |="));
            CHECK("G8: the probe never calls Vulkan or the lazy RADV open", !has(probe, probe_end, "rdna4_radv_open") && !has(probe, probe_end, "vkCreate"));
            CHECK("G8: the verdict comes from the pure function, the probe returns NULL only on OK", strstr(probe, "return v == RDNA4G_PROBE_OK ? NULL : rdna4g_probe_text(v);") != NULL);
        }
        CHECK("G8: rdna4_fallback_ok asks the pure function with the admitted flag", strstr(m, "rdna4g_fallback_ok(rdna4_is_ws(), rdna4_force_fallback(), rdna4_test_fb_as_ws(), atomic_load(&rdna4_app_admitted))") != NULL);
        CHECK("G8: native code container capabilities are explicitly declined", strstr(m, "- (BOOL)supportsDynamicLibraries { return NO; }") && strstr(m, "- (BOOL)supportsRenderDynamicLibraries { return NO; }") && strstr(m, "- (BOOL)supportsBinaryArchives { return NO; }"));
        CHECK("G8: process wait budget uses a lock and pure strikes policy", strstr(m, "pthread_mutex_lock(&RDNA4SYNC.mu)") && strstr(m, "rdna4g_sync_allowed(&RDNA4SYNC.state)") && strstr(m, "rdna4g_sync_result(&RDNA4SYNC.state"));
        CHECK("G8: no obsolete 60-second wait constant remains", !strstr(m, "RDNA4G_SYNC_WAIT_MS"));
        const char *pc = fn_start(m, "static void rdna4_pc_open(const VkPhysicalDeviceProperties *pp) {");
        const char *pc_end = pc ? strstr(pc, "static void rdna4_pc_save(") : NULL;
        CHECK("G8: rdna4_pc_open is found", pc && pc_end);
        if (pc && pc_end) {
            const char *conf = strstr(pc, "confstr(_CS_DARWIN_USER_CACHE_DIR"), *path = strstr(pc, "rdna4g_pcache_path(RDNA4PC.path, sizeof RDNA4PC.path, pcls, envdir, vtw, ucd, getprogname())"), *sv = strstr(pc, "rdna4g_pcache_save(pcls");
            CHECK("G8: the cache path comes from the pure function after reading the user cache dir", conf && path && sv && before(conf, path) && before(path, sv));
            CHECK("G8: the cache class is the process class", has(pc, pc_end, "const int pcls = rdna4g_class(rdna4_is_ws(), rdna4_allow());"));
            CHECK("G8: the cache directory is no longer hard-coded in rdna4_pc_open", !has(pc, pc_end, "snprintf(RDNA4PC.path, sizeof RDNA4PC.path, \"%s/rdna4m-pipecache.bin\", dir)"));
        }
        CHECK("G8: rdna4_nub_flag tells an unreachable nub (-2) from an absent property (-1)", strstr(m, "kIOServicePlane, &parent) != KERN_SUCCESS || !parent) return RDNA4G_FLAG_NONUB;") != NULL && strstr(m, "if (!v) return RDNA4G_FLAG_ABSENT;") != NULL);
        free(m);
    }
    // ---- G9 ----
    CHECK("G9: the by-class text is 0.0.632's refusal, word for word", !strcmp(RDNA4G_DECLINE_BY_CLASS_TEXT, "process is not WindowServer and RDNA4M_ALLOW=1 (as root) is not set"));
    CHECK("G9: the bound is 501, the kernel's kAppMinUid", RDNA4G_APP_MIN_EUID == 501u);
    { static const uint32_t euids[] = { 0, 1, 88, 89, 200, 499, 500, 501, 502, 1000, 65534, 0xFFFFFFFFu };
      int declined = 0, probed = 0;
      for (unsigned i = 0; i < sizeof euids / sizeof euids[0]; i++) for (int ws = 0; ws <= 1; ws++) for (int allow = 0; allow <= 1; allow++) {
          const int cls = rdna4g_class(ws, allow), d = rdna4g_declined_by_class(cls, euids[i]);
          const int probe = (cls == RDNA4G_CLASS_OTHER) && !d;   // the flow of rdna4_admit: the probe is reached only for class OTHER that was not declined first
          if (cls != RDNA4G_CLASS_OTHER) CHECK("G9: WindowServer and RDNA4M_ALLOW root tools are never declined by class, whatever their euid", !d && !probe);
          else if (euids[i] < 501u) { declined++; CHECK("G9: class OTHER with euid < 501 (root, daemons) is declined by class and never probed", d && !probe); }
          else { probed++; CHECK("G9: class OTHER with euid >= 501 is not declined by class and IS probed (reachable)", !d && probe); }
      }
      CHECK("G9: both branches were reached", declined > 0 && probed > 0); }
    char *m2 = slurp(dir, "GarnetDevice.m");
    CHECK("G9: GarnetDevice.m is readable", m2 != NULL);
    if (m2) {
        const char *adm = fn_start(m2, "static const char *rdna4_admit(uint32_t port) {");
        const char *adm_end = adm ? strstr(adm, "\nstatic BOOL rdna4_headless(void)") : NULL;
        if (adm && adm_end) {
            const char *dec = strstr(adm, "if (rdna4g_declined_by_class(cls, (uint32_t)geteuid())) return RDNA4G_DECLINE_BY_CLASS_TEXT;"), *stt = strstr(adm, "stat(RDNA4_KILL_FILE, &st)"), *pr = strstr(adm, "rdna4_app_probe(port)"), *gate = strstr(adm, "if (cls == RDNA4G_CLASS_OTHER) {");
            CHECK("G9: rdna4_admit declines by class FIRST: before the kill file, before any nub read, before the probe", dec && stt && pr && gate && before(dec, stt) && before(dec, pr) && before(dec, gate));
            CHECK("G9: the euid used is geteuid(), not getuid()", dec && !has(adm, adm_end, "getuid()"));
        }
    }
    // ---- G10 (0.0.641 rules, kept; build 6: per-uid subdirectories) ----
    CHECK("G10: a real directory owned by the caller is usable", rdna4g_dumpdir_ok(0, 1, 0, 501, 501) && rdna4g_dumpdir_ok(0, 1, 0, 0, 0) && rdna4g_dumpdir_ok(0, 1, 0, 88, 88));
    CHECK("G10: a symlink is refused (even to a directory of ours)", !rdna4g_dumpdir_ok(0, 0, 1, 501, 501) && !rdna4g_dumpdir_ok(0, 1, 1, 501, 501));
    CHECK("G10: a directory owned by someone else is refused (a planted one)", !rdna4g_dumpdir_ok(0, 1, 0, 88, 501) && !rdna4g_dumpdir_ok(0, 1, 0, 501, 0) && !rdna4g_dumpdir_ok(0, 1, 0, 0, 501));
    CHECK("G10: a failed lstat or a plain file is refused", !rdna4g_dumpdir_ok(-1, 1, 0, 501, 501) && !rdna4g_dumpdir_ok(0, 0, 0, 501, 501));
    CHECK("G10: any group / other bit is narrowed, 0700 is left alone", rdna4g_dumpdir_needs_narrow(0777) && rdna4g_dumpdir_needs_narrow(0770) && rdna4g_dumpdir_needs_narrow(0707) && rdna4g_dumpdir_needs_narrow(0755) && rdna4g_dumpdir_needs_narrow(01777) && !rdna4g_dumpdir_needs_narrow(0700) && !rdna4g_dumpdir_needs_narrow(0500));
    CHECK("G10: the parent is /tmp/rdna4m", !strcmp(RDNA4G_DUMP_DIR, "/tmp/rdna4m"));
    // ---- G12 (build 6, C1 finding A): the parent (sticky 01777) and the per-uid subdirectory ----
    CHECK("G12: parent: a real sticky-1777 directory owned by root, by the caller or by the daemon user is usable", rdna4g_dumproot_ok(0, 1, 0, 0, 041777, 501) && rdna4g_dumproot_ok(0, 1, 0, 501, 041777, 501) && rdna4g_dumproot_ok(0, 1, 0, 88, 01777, 88) && rdna4g_dumproot_ok(0, 1, 0, RDNA4G_DAEMON_UID, 01777, 501));
    CHECK("G12: parent: a symlink, a failed lstat or a plain file is refused", !rdna4g_dumproot_ok(0, 0, 1, 0, 01777, 501) && !rdna4g_dumproot_ok(-1, 1, 0, 0, 01777, 501) && !rdna4g_dumproot_ok(0, 0, 0, 0, 01777, 501));
    CHECK("G12: parent: owned by a THIRD user is refused even at 1777 (a planted one)", !rdna4g_dumproot_ok(0, 1, 0, 502, 01777, 501) && !rdna4g_dumproot_ok(0, 1, 0, 88, 01777, 501));
    CHECK("G12: parent: without the sticky bit or world-write it is refused (0700 from a pre-build-6 WindowServer, 0777 without sticky, 0755)", !rdna4g_dumproot_ok(0, 1, 0, 88, 0700, 88) && !rdna4g_dumproot_ok(0, 1, 0, 0, 0777, 501) && !rdna4g_dumproot_ok(0, 1, 0, 0, 0755, 501) && !rdna4g_dumproot_ok(0, 1, 0, 0, 01755, 501));
    CHECK("G12: parent: extra setuid / setgid bits are refused", !rdna4g_dumproot_ok(0, 1, 0, 0, 03777, 501) && !rdna4g_dumproot_ok(0, 1, 0, 0, 05777, 501));
    CHECK("G12: parent repair: only a parent that is OURS and not 1777 is repaired", rdna4g_dumproot_needs_fix(88, 88, 0700) && rdna4g_dumproot_needs_fix(501, 501, 01755) && !rdna4g_dumproot_needs_fix(501, 501, 01777) && !rdna4g_dumproot_needs_fix(0, 501, 0700) && !rdna4g_dumproot_needs_fix(88, 501, 0700));
    { char sp[64];
      CHECK("G12: the subdirectory is /tmp/rdna4m/<euid>", rdna4g_dumpsub_path(sp, sizeof sp, 501) && !strcmp(sp, "/tmp/rdna4m/501"));
      CHECK("G12: WindowServer (88) keeps ITS OWN subdirectory", rdna4g_dumpsub_path(sp, sizeof sp, 88) && !strcmp(sp, "/tmp/rdna4m/88"));
      CHECK("G12: distinct uids give distinct subdirectories, uid 0 included, the largest uid fits", rdna4g_dumpsub_path(sp, sizeof sp, 0) && !strcmp(sp, "/tmp/rdna4m/0") && rdna4g_dumpsub_path(sp, sizeof sp, 4294967294u) && !strcmp(sp, "/tmp/rdna4m/4294967294"));
      CHECK("G12: a too-small buffer yields 0 and an empty string", rdna4g_dumpsub_path(sp, 8, 501) == 0 && sp[0] == 0); }
    CHECK("G12: the daemon user is nobody (uid -2)", RDNA4G_DAEMON_UID == 4294967294u);
    if (m2) {
        const char *rd = fn_start(m2, "static int rdna4_dump_dir_ready(char *sub, size_t subn) {");
        const char *rd_end = rd ? strstr(rd, "\n// A dump file appears under its final name only when complete") : NULL;
        CHECK("G12: rdna4_dump_dir_ready is found", rd && rd_end);
        if (rd && rd_end) {
            const char *mkr = strstr(rd, "mkdir(RDNA4G_DUMP_DIR, RDNA4G_DUMP_ROOT_MODE)"), *lr = strstr(rd, "lstat(RDNA4G_DUMP_DIR, &rs)"), *fx = strstr(rd, "rdna4g_dumproot_needs_fix("), *fcr = strstr(rd, "fchmodat(AT_FDCWD, RDNA4G_DUMP_DIR, RDNA4G_DUMP_ROOT_MODE, AT_SYMLINK_NOFOLLOW)"),
                       *okr = strstr(rd, "if (!rdna4g_dumproot_ok(rrc, "), *sp = strstr(rd, "rdna4g_dumpsub_path(sub, subn, (uint32_t)geteuid())"), *mks = strstr(rd, "mkdir(sub, 0700)"), *ls = strstr(rd, "lstat(sub, &st)"),
                       *ok = strstr(rd, "if (!rdna4g_dumpdir_ok(rc, S_ISDIR(st.st_mode), S_ISLNK(st.st_mode), (uint32_t)st.st_uid, (uint32_t)geteuid())"), *op = strstr(rd, "open(sub, O_RDONLY | O_DIRECTORY | O_NOFOLLOW)"),
                       *nr = strstr(rd, "rdna4g_dumpdir_needs_narrow((uint32_t)fs.st_mode)"), *fm = strstr(rd, "fchmod(fd, 0700)"), *ac = strstr(rd, "rdna4da_grant_list(fd, (uid_t)RDNA4G_DAEMON_UID)");
            CHECK("G12: parent: mkdir, lstat, repair (no-follow), re-lstat verdict, THEN the subdirectory", mkr && lr && fx && fcr && okr && sp && before(mkr, lr) && before(lr, fx) && before(fx, fcr) && before(fcr, okr) && before(okr, sp));
            CHECK("G12: subdirectory: mkdir 0700, lstat, the ownership / symlink verdict, an O_NOFOLLOW open, narrowing and the ACL grant on that descriptor (in this order)", mks && ls && ok && op && nr && fm && ac && before(sp, mks) && before(mks, ls) && before(ls, ok) && before(ok, op) && before(op, nr) && before(nr, fm) && before(fm, ac));
            CHECK("G12: both refusals return 0 BEFORE any narrowing, ACL or write", has(rd, rd_end, "return 0;") && before(strstr(rd, "return 0;"), op) && before(okr, strstr(okr, "return 0;")) );
            CHECK("G12: the descriptor must be the directory lstat described (inode, device, owner)", has(rd, rd_end, "fs.st_ino != st.st_ino") && has(rd, rd_end, "fs.st_dev != st.st_dev") && has(rd, rd_end, "(uint32_t)fs.st_uid != (uint32_t)geteuid()"));
            CHECK("G12: no stat() (it follows links), no plain chmod, and no path-based ACL call in the helper", !has(rd, rd_end, " stat(RDNA4G_DUMP_DIR") && !has(rd, rd_end, " stat(sub") && !has(rd, rd_end, " chmod(") && !has(rd, rd_end, "acl_set_file") && !has(rd, rd_end, "acl_set_link_np"));
            CHECK("G12: the 0.0.641 narrowing of the parent is gone (the parent MUST stay sticky 1777)", !has(rd, rd_end, "fchmodat(AT_FDCWD, RDNA4G_DUMP_DIR, 0700"));
        }
        const char *df = fn_start(m2, "static void rdna4_dump_function(");
        const char *df_end = df ? strstr(df, "\nstatic NSError *rdna4_dump_pipeline(") : NULL;
        CHECK("G10: rdna4_dump_function is found", df && df_end);
        if (df && df_end) {
            const char *rdy = strstr(df, "if (!rdna4_dump_dir_ready(sub, sizeof sub)) {"), *sd = strstr(df, "if (!rdna4_dump_write(side.UTF8String, js)) {"), *wr = strstr(df, "if (!rdna4_dump_write(air.UTF8String, bc)) {");
            CHECK("G10: REACHABLE: both dump files are written only after the directory check passed", rdy && sd && wr && before(rdy, sd) && has(df, df_end, "return; }"));
            CHECK("G12: the sidecar is written BEFORE the .air (the daemon starts on the .air)", sd && wr && before(sd, wr));
            const char *sd_end = sd ? strstr(sd, "if (!rdna4_dump_write(air.UTF8String, bc)) {") : NULL;
            const char *wr_end = wr ? strstr(wr, "[paths addObject:") : NULL;
            CHECK("G12: a sidecar write failure reports the sidecar path and errno", sd_end && has(sd, sd_end, "write to sidecar %s failed (errno %d: %s)") && has(sd, sd_end, "strerror(savedErrno)"));
            CHECK("G12: an AIR write failure reports the AIR path and errno", wr_end && has(wr, wr_end, "write to AIR %s failed (errno %d: %s)") && has(wr, wr_end, "strerror(savedErrno)"));
            CHECK("G12: dump paths are under the per-uid subdirectory, not the flat parent", has(df, df_end, "@\"%s/%@.air\", sub, sha") && has(df, df_end, "@\"%s/%@.%s.json\", sub, sha, role") && !has(df, df_end, "/tmp/rdna4m/%@"));
        }
        const char *dw = fn_start(m2, "static int rdna4_dump_write(const char *path, NSData *data) {");
        const char *dw_end = dw ? strstr(dw, "\n// Dumps one function's bitcodeData") : NULL;
        CHECK("G12: rdna4_dump_write is found", dw && dw_end);
        if (dw && dw_end) {
            const char *op = strstr(dw, "mkstemp(tmp)"), *fc = strstr(dw, "fchmod(fd, 0644)"), *rn = strstr(dw, "rename(tmp, path)");
            CHECK("G12: a file appears under its final name only complete and 0644: exclusive temp, fchmod 0644, rename", op && fc && rn && before(op, fc) && before(fc, rn));
            CHECK("G12: each writer gets a unique temp name outside the daemon's .air / .json scan", has(dw, dw_end, "\"%s.tmpXXXXXX\""));
            CHECK("G12: a failed rename preserves errno through temp-file cleanup", has(dw, dw_end, "const int savedErrno = failure ? failure : errno;") && has(dw, dw_end, "errno = savedErrno;"));
        }
        CHECK("G10: the 0777 mkdir and chmod are gone from the whole bundle", !strstr(m2, "mkdir(\"/tmp/rdna4m\", 0777)") && !strstr(m2, "chmod(\"/tmp/rdna4m\", 0777)"));
        CHECK("G10: no plain chmod of the dump directories anywhere", !strstr(m2, "chmod(\"/tmp/rdna4m") && !strstr(m2, "chmod(sub"));
    }
    // Actual delayed compiler/output checks live in test-cold-pipelines.sh.
    CHECK("G13: admitted applications and WindowServer wait for real translations", rdna4g_syncwait_applies(0, 1, 0, 0) && rdna4g_syncwait_applies(1, 0, 0, 0) && rdna4g_syncwait_applies(1, 1, 0, 0));
    CHECK("G13: unadmitted processes and explicit placeholder tests do not wait", !rdna4g_syncwait_applies(0, 0, 0, 0) && !rdna4g_syncwait_applies(0, 1, 1, 0) && !rdna4g_syncwait_applies(1, 1, 0, 1));
    CHECK("G13: WS waits at most 250 ms, apps at most 3000 ms", RDNA4G_SYNC_WAIT_WS_MS == 250 && RDNA4G_SYNC_WAIT_APP_MS == 3000 && rdna4g_sync_wait_ms(1) == 250 && rdna4g_sync_wait_ms(0) == 3000);
    { rdna4g_sync s = { 0 };
      CHECK("G13: fresh process may wait, strikes budget is three", rdna4g_sync_allowed(&s) && RDNA4G_SYNC_STRIKES == 3);
      for (int i = 1; i <= 3; i++) {
          rdna4g_sync_result(&s, 0);
          CHECK("G13: each consecutive timeout spends one strike", s.strikes == i && rdna4g_sync_allowed(&s) == (i < 3));
      }
      for (int i = 0; i < 10000; i++) rdna4g_sync_result(&s, 0);
      CHECK("G13: strikes saturate without overflow", s.strikes == 3 && !rdna4g_sync_allowed(&s));
      rdna4g_sync_result(&s, 1);
      CHECK("G13: success resets and rearms waits", s.strikes == 0 && rdna4g_sync_allowed(&s));
      rdna4g_sync_result(&s, 0); rdna4g_sync_result(&s, 1); rdna4g_sync_result(&s, 0);
      CHECK("G13: only consecutive timeouts count", s.strikes == 1 && rdna4g_sync_allowed(&s));
    }
    if (m2) {
        const char *tf = fn_start(m2, "static BOOL rdna4x_translate_fn_with_error(id fn, const char *role, uint64_t deadlineNs, NSError **error) {");
        const char *tf_end = tf ? strstr(tf, "\n}\n") : NULL;
        CHECK("G13: in-process waits use the app cap and shared strikes budget", tf && tf_end && has(tf, tf_end, "RDNA4G_SYNC_WAIT_APP_MS") && has(tf, tf_end, "rdna4_sync_begin(!rdna4_is_ws())") && has(tf, tf_end, "if (!waiting) waitMs = 0;") && has(tf, tf_end, "rdna4_sync_end(rc == RDNA4X_OK, rc == RDNA4X_E_TIMEOUT)"));
        CHECK("G13: hot-swap searches the translator user directory", strstr(m2, "NSString *u = rdna4x_user_dir();") && strstr(m2, "[m addObject:u]") && strstr(m2, "for (NSString *dir in rdna4_spv_dirs())"));
        const char *sw = fn_start(m2, "static BOOL rdna4_sync_wait(const char *what, NSArray<NSString *> *shas, BOOL needMeta) {");
        const char *sw_end = sw ? strstr(sw, "\n// ---------------------------------------------------------------------------------------------------------------\n// RDNA4RenderPipelineState (10d/11e)") : NULL;
        CHECK("G13: rdna4_sync_wait is found", sw && sw_end);
        if (sw && sw_end) {
            CHECK("G13: the wait is gated by the admission rule", has(sw, sw_end, "rdna4g_syncwait_applies(rdna4_is_ws(), atomic_load(&rdna4_app_admitted), rdna4_force_fallback(), rdna4_test_fb_as_ws())"));
            CHECK("G13: publication uses bounded directory events", has(sw, sw_end, "EVFILT_VNODE") && has(sw, sw_end, "kevent(queue, NULL") && has(sw, sw_end, "rdna4g_sync_wait_ms(rdna4_is_ws())") && !has(sw, sw_end, "usleep("));
            CHECK("G13: daemon waits acquire and release the process budget", has(sw, sw_end, "rdna4_sync_begin(NO)") && has(sw, sw_end, "rdna4_sync_end(found, !found && rdna4_now() >= deadline)"));
            CHECK("G13: the wait uses a paired cache observation with the SPIR-V sanity check", has(sw, sw_end, "rdna4hs_stat_pair(sha, needMeta)") && has(sw, sw_end, "!pair.spv.present || (needMeta && !pair.meta.present)"));
        }
        const char *ri = fn_start(m2, "- (instancetype)initWithDevice:(id)dev descriptor:(MTLRenderPipelineDescriptor *)d noFallback:(BOOL)nofb error:(NSError **)err {");
        const char *ri_end = ri ? strstr(ri, "RDNA4sRefl *rv = NULL, *rf = NULL;") : NULL;
        CHECK("G13: the render miss branch is found", ri && ri_end);
        if (ri && ri_end) {
            const char *dm = strstr(ri, "(void)rdna4_dump_pipeline(d);"), *wt = strstr(ri, "rdna4_sync_wait(\"render\", want, NO)"), *fbk = strstr(ri, "fb = YES; _fallback = YES;"), *rl = strstr(ri, "vsd = rdna4_spv_lookup(d.vertexFunction, \"vertex\", &e1, &vname, &mv_);\n                    fsd");
            CHECK("G13: render: dump FIRST, then the wait, then (only if still missing) the fallback", dm && wt && fbk && before(dm, wt) && before(wt, fbk));
            CHECK("G13: render: after a successful wait both stages are looked up again before deciding", rl && before(wt, rl) && before(rl, fbk));
            CHECK("G13: render: unadmitted processes keep the NSError", has(ri, ri_end, "if (!rdna4_fallback_ok()) { if (err) *err = e; return nil; }"));
        }
        const char *cp = fn_start(m2, "- (id)rdna4NewComputePipeline:(id)fn error:(NSError **)error {");
        const char *cp_end = cp ? strstr(cp, "- (id)newComputePipelineStateWithFunction:(id)fn error:") : NULL;
        CHECK("G13: the compute miss branch is found", cp && cp_end);
        if (cp && cp_end) {
            const char *gd = strstr(cp, "if (!inproc && e.code == 41 && rdna4g_syncwait_applies("), *dm = strstr(cp, "rdna4_dump_function(fn, \"kernel\", dp, dn);"), *wt = strstr(cp, "rdna4_sync_wait(\"kernel\", @[ ksha ], YES)"), *rt = strstr(cp, "initWithDevice:self function:fn error:&e2"), *ph = strstr(cp, "initPlaceholderWithDevice:self function:fn");
            CHECK("G13: compute: dump, wait (spv + meta), retry the real pipeline, THEN the no-op placeholder", gd && dm && wt && rt && ph && before(gd, dm) && before(dm, wt) && before(wt, rt) && before(rt, ph));
        }
    }
    // ---- G14 (build 6, C1 finding B): the GPU family claim ----
    CHECK("G14: Metal3 (5001) is NOT claimed for apps, kept for WindowServer", !rdna4g_supports_family(RDNA4G_FAMILY_METAL3, 0) && rdna4g_supports_family(RDNA4G_FAMILY_METAL3, 1) && RDNA4G_FAMILY_METAL3 == 5001);
    CHECK("G14: Mac2 and Common1..3 are kept", rdna4g_supports_family(2002, 0) && rdna4g_supports_family(3001, 0) && rdna4g_supports_family(3002, 0) && rdna4g_supports_family(3003, 0) && rdna4g_supports_family(3001, 1));
    { int any = 0; static const long no[] = { 1001, 1002, 1003, 1004, 1005, 1006, 1007, 1008, 1009, 2001, 4001, 5001, 5002, 6001, 0, -1, 99999 };
      for (unsigned i = 0; i < sizeof no / sizeof no[0]; i++) any |= rdna4g_supports_family(no[i], 0) | (no[i] != RDNA4G_FAMILY_METAL3 ? rdna4g_supports_family(no[i], 1) : 0);
      CHECK("G14: Apple1..9, Mac1, MacCatalyst, Metal3, Metal4 and unknown values are all NO", !any); }
    CHECK("G14: argument buffers report the lowest tier", RDNA4G_ARGBUF_TIER1 == 0);
    if (m2) {
        const char *sf = fn_start(m2, "- (BOOL)supportsFamily:(NSInteger)family {");
        const char *sf_end = sf ? strstr(sf, "- (NSUInteger)argumentBuffersSupport") : NULL;
        CHECK("G14: supportsFamily asks the pure table (no case list in the .m)", sf && sf_end && has(sf, sf_end, "return rdna4g_supports_family((long)family, rdna4_is_ws() ? 1 : 0) ? YES : NO;") && !has(sf, sf_end, "case "));
        CHECK("G14: it logs once when Metal3 is asked for", sf && sf_end && has(sf, sf_end, "RDNA4_ONCE(\"supportsFamily(Metal3) -> NO"));
        CHECK("G14: Metal3 is logged only for applications (WindowServer keeps YES)", sf && sf_end && has(sf, sf_end, "if (family == RDNA4G_FAMILY_METAL3 && !rdna4_is_ws())"));
        CHECK("G14: WindowServer gets the base class's argumentBuffersSupport", strstr(m2, "return ((NSUInteger (*)(struct objc_super *, SEL))objc_msgSendSuper)(&sup, @selector(argumentBuffersSupport));") != NULL && strstr(m2, "    if (rdna4_is_ws()) {   // the base class") != NULL);
        CHECK("G14: argumentBuffersSupport is overridden and returns the Tier1 constant", strstr(m2, "- (NSUInteger)argumentBuffersSupport {") != NULL && strstr(m2, "return (NSUInteger)RDNA4G_ARGBUF_TIER1; }") != NULL);
        CHECK("G14: the old Metal3 constant is gone from the .m", !strstr(m2, "RDNA4_Metal3"));
    }
    free(m2);
    // ---- G15 (bundle 8, G6): sandboxed applications open RDNA4N on the accelerator port the bundle holds ----
    CHECK("G15: WindowServer keeps Mesa's default service (no setter call)", rdna4g_mesa_service_for_class(RDNA4G_CLASS_WS) == NULL);
    CHECK("G15: an RDNA4M_ALLOW root tool keeps Mesa's default service", rdna4g_mesa_service_for_class(RDNA4G_CLASS_ROOT) == NULL);
    CHECK("G15: an application (CLASS_OTHER) opens on GarnetAccelerator", rdna4g_mesa_service_for_class(RDNA4G_CLASS_OTHER) && !strcmp(rdna4g_mesa_service_for_class(RDNA4G_CLASS_OTHER), "GarnetAccelerator"));
    CHECK("G15: no other class value selects the accelerator", rdna4g_mesa_service_for_class(-1) == NULL && rdna4g_mesa_service_for_class(3) == NULL && rdna4g_mesa_service_for_class(99) == NULL);
    CHECK("G15: the setter's export name", !strcmp(RDNA4G_MESA_SETTER, "radv_darwin_set_service_class"));
    { char *m5 = slurp(dir, "GarnetDevice.m");
      CHECK("G15: GarnetDevice.m is readable", m5 != NULL);
      if (m5) {
        const char *probe = fn_start(m5, "static const char *rdna4_app_probe(uint32_t port) {");
        const char *probe_end = probe ? strstr(probe, "\n// NULL = admit, else the reason") : NULL;
        CHECK("G15: the probe is found", probe && probe_end);
        if (probe && probe_end) {
            CHECK("G15: the probe opens on the port it was given (the accelerator), cast to io_service_t", has(probe, probe_end, "io_service_t svc = (io_service_t)port;"));
            CHECK("G15: the probe does not look a service up by name (no GarnetDriver, no matching)", !has(probe, probe_end, "IOServiceMatching") && !has(probe, probe_end, "IOServiceGetMatchingService") && !has(probe, probe_end, "GarnetDriver"));
            CHECK("G15: the probe does not release the caller's port", !has(probe, probe_end, "IOObjectRelease"));
            CHECK("G15: no port = no service for the pure verdict", has(probe, probe_end, "rdna4g_probe_verdict(svc != 0,"));
            CHECK("G15: the probe still opens type RDNA4N", has(probe, probe_end, "IOServiceOpen(svc, mach_task_self(), RDNA4G_UC_TYPE, &conn)"));
        }
        const char *ro = fn_start(m5, "static BOOL rdna4_radv_open_once(NSError **err) {");
        const char *ro_end = ro ? strstr(ro, "VkResult r = rdna4_vkCreateInstance(&ici, NULL, &RDNA4R.inst);") : NULL;
        CHECK("G15: the RADV open (up to vkCreateInstance) is found", ro && ro_end);
        if (ro && ro_end) {
            const char *gp = strstr(ro, "rdna4_gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, \"vk_icdGetInstanceProcAddr\");"), *sel = strstr(ro, "rdna4g_mesa_service_for_class(rdna4g_class(rdna4_is_ws(), rdna4_allow()))"), *gd = strstr(ro, "if (svcCls) {"),
                       *ds = strstr(ro, "dlsym(h, RDNA4G_MESA_SETTER)"), *nos = strstr(ro, "if (!setSvc) {"), *call = strstr(ro, "setSvc(svcCls)"), *bad = strstr(ro, "if (src != 0) {");
            CHECK("G15: the route is chosen per class, after dlopen, and BEFORE vkCreateInstance", gp && sel && gd && ds && call && before(gp, sel) && before(sel, gd) && before(gd, ds) && before(ds, call));
            CHECK("G15: a missing setter or a refused name fails closed (error 7, goto fail) before any instance exists", nos && bad && before(nos, call) && before(call, bad) && has(nos, bad, "rdna4_err(7,") && has(bad, ro_end, "rdna4_err(7,") && has(nos, call, "goto fail;"));
            CHECK("G15: the setter is called in exactly one place", strstr(ro, "setSvc(svcCls)") == strstr(m5, "setSvc(svcCls)") && !strstr(strstr(m5, "setSvc(svcCls)") + 10, "setSvc("));
            CHECK("G15: WindowServer / root never reach the setter (the call is under `if (svcCls)`)", gd && call && before(gd, call) && !has(ro, gd, "if (rdna4_is_ws()") );
        }
        free(m5); } }
    // ---- G11 (0.0.641): the bundle build ----
    { char *pl = slurp(dir, "Info.plist");
      CHECK("G11: Info.plist: bundle version 0.1.1, build 43", pl && strstr(pl, "<key>CFBundleShortVersionString</key>\n\t<string>0.1.1</string>") && strstr(pl, "<key>CFBundleVersion</key>\n\t<string>43</string>"));
      free(pl); }
    printf("test-gate: %d checks, %d failed\n", runs, fails);
    return fails ? 1 : 0;
}
