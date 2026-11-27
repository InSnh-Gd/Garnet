// test-gate.c: garnet_gate.h (GPU-apps G4, kext 0.0.640: the bundle's load policy outside WindowServer) on the host, plus source pins that tie the REAL GarnetDevice.m to it.
// Build/run on the host Mac (from this directory; argv[1] = the directory holding GarnetDevice.m and garnet_gate.h, default "."):
//   cc -O1 -Wall -Wextra -Werror -o /tmp/test-gate test-gate.c && /tmp/test-gate .
// tests/../test-gate-plant.sh plants breaks in the REAL garnet_gate.h and GarnetDevice.m and demands that this suite FAILS on each.
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
#include <sys/stat.h>
#include <unistd.h>
#include "garnet_gate.h"
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

static int test_sb_probe_ret = 0;
static int test_sb_probe_calls = 0;
static int test_sb_probe_fn(void *ctx) {
    (void)ctx;
    test_sb_probe_calls++;
    return test_sb_probe_ret;
}

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
      CHECK("G5: WindowServer: /private/var/tmp when writable", rdna4g_pcache_path(p, sizeof p, RDNA4G_CLASS_WS, NULL, 1, "/var/folders/xx/C/", "WindowServer") && !strcmp(p, "/private/var/tmp/garnet-pipecache.bin"));
      CHECK("G5: WindowServer: /private/tmp when /private/var/tmp is not writable", rdna4g_pcache_path(p, sizeof p, RDNA4G_CLASS_WS, NULL, 0, "/x/", "WindowServer") && !strcmp(p, "/private/tmp/garnet-pipecache.bin"));
      CHECK("G5: a root tool honours the test directory", rdna4g_pcache_path(p, sizeof p, RDNA4G_CLASS_ROOT, "/tmp/t", 1, "/x/", "mtlprobe") && !strcmp(p, "/tmp/t/garnet-pipecache.bin"));
      CHECK("G5: an application gets its OWN file in the user cache dir", rdna4g_pcache_path(p, sizeof p, RDNA4G_CLASS_OTHER, NULL, 1, "/var/folders/xx/C/", "mpv") && !strcmp(p, "/var/folders/xx/C/garnet-pipecache.mpv.bin"));
      CHECK("G5: ... even if /private/var/tmp is writable and a test dir is offered", rdna4g_pcache_path(p, sizeof p, RDNA4G_CLASS_OTHER, "/tmp/t", 1, "/c/", "app") && !strcmp(p, "/c/garnet-pipecache.app.bin"));
      CHECK("G5: the cache dir without a trailing slash still works", rdna4g_pcache_path(p, sizeof p, RDNA4G_CLASS_OTHER, NULL, 0, "/c", "app") && !strcmp(p, "/c/garnet-pipecache.app.bin"));
      CHECK("G5: an application with no user cache dir has NO cache (never /private/var/tmp)", rdna4g_pcache_path(p, sizeof p, RDNA4G_CLASS_OTHER, NULL, 1, "", "mpv") == 0 && p[0] == 0 && rdna4g_pcache_path(p, sizeof p, RDNA4G_CLASS_OTHER, NULL, 1, NULL, "mpv") == 0);
      CHECK("G5: path separators and odd characters in the program name are neutralised", rdna4g_pcache_path(p, sizeof p, RDNA4G_CLASS_OTHER, NULL, 1, "/c/", "../a b/c") && !strcmp(p, "/c/garnet-pipecache..._a_b_c.bin") && strchr(p + 3, '/') == NULL);
      CHECK("G5: an empty program name becomes app", rdna4g_pcache_path(p, sizeof p, RDNA4G_CLASS_OTHER, NULL, 1, "/c/", "") && !strcmp(p, "/c/garnet-pipecache.app.bin"));
      CHECK("G5: a long program name is cut to 40 characters", rdna4g_pcache_path(p, sizeof p, RDNA4G_CLASS_OTHER, NULL, 1, "/c/", "0123456789012345678901234567890123456789ZZZZ") && !strcmp(p, "/c/garnet-pipecache.0123456789012345678901234567890123456789.bin"));
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
    CHECK("G7: ABI minor is 14", RDNA4N_ABI_MINOR == 14u);
    CHECK("G7: ABI 1.14 info flags and minor offset match GarnetNativeABI.h",
          RDNA4G_INFO_READY == RDNA4N_INFO_READY &&
          RDNA4G_INFO_DISPLAY_ACTIVE == RDNA4N_INFO_DISPLAY_ACTIVE &&
          RDNA4G_INFO_KILL_SWITCH == RDNA4N_INFO_KILL_SWITCH &&
          RDNA4G_INFO_MINOR_OFF == offsetof(struct rdna4n_info, reserved) + 8u);
    // ---- G9 ----
    CHECK("G9: the by-class text names GARNET_ALLOW=1", !strcmp(RDNA4G_DECLINE_BY_CLASS_TEXT, "process is not WindowServer and GARNET_ALLOW=1 (as root) is not set"));
    // ---- G9b (1D migration): GARNET_* priority + 1-release RDNA4M_* fallback & kill-file pair ----
    { int leg = -1;
      const char *v = rdna4g_getenv_pair("1", "0", &leg);
      CHECK("G9b: GARNET_* takes priority over RDNA4M_*", v && !strcmp(v, "1") && leg == 0);
      v = rdna4g_getenv_pair("0", "1", &leg);
      CHECK("G9b: explicit GARNET_*=0 overrides legacy RDNA4M_*=1", v && !strcmp(v, "0") && leg == 0);
      v = rdna4g_getenv_pair(NULL, "1", &leg);
      CHECK("G9b: unset GARNET_* falls back to RDNA4M_* and flags deprecation", v && !strcmp(v, "1") && leg == 1);
      v = rdna4g_getenv_pair(NULL, NULL, &leg);
      CHECK("G9b: both unset returns NULL", v == NULL && leg == 0);
      CHECK("G9b: new kill file present wins without legacy flag", rdna4g_kill_state_pair(0, 0, -1, ENOENT, &leg) == RDNA4G_KILL_PRESENT && leg == 0);
      CHECK("G9b: legacy kill file present when new absent triggers kill + legacy flag", rdna4g_kill_state_pair(-1, ENOENT, 0, 0, &leg) == RDNA4G_KILL_PRESENT && leg == 1);
      CHECK("G9b: new kill file unreadable fails closed immediately", rdna4g_kill_state_pair(-1, EPERM, -1, ENOENT, &leg) == RDNA4G_KILL_UNREADABLE && leg == 0);
      CHECK("G9b: legacy kill file unreadable when new absent fails closed", rdna4g_kill_state_pair(-1, ENOENT, -1, EPERM, &leg) == RDNA4G_KILL_UNREADABLE && leg == 0);
      CHECK("G9b: both kill files absent -> ABSENT", rdna4g_kill_state_pair(-1, ENOENT, -1, ENOENT, &leg) == RDNA4G_KILL_ABSENT && leg == 0);
    }
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
    // ---- G10 (0.0.641 rules, kept; build 6: per-uid subdirectories) ----
    CHECK("G10: a real directory owned by the caller is usable", rdna4g_dumpdir_ok(0, 1, 0, 501, 501) && rdna4g_dumpdir_ok(0, 1, 0, 0, 0) && rdna4g_dumpdir_ok(0, 1, 0, 88, 88));
    CHECK("G10: a symlink is refused (even to a directory of ours)", !rdna4g_dumpdir_ok(0, 0, 1, 501, 501) && !rdna4g_dumpdir_ok(0, 1, 1, 501, 501));
    CHECK("G10: a directory owned by someone else is refused (a planted one)", !rdna4g_dumpdir_ok(0, 1, 0, 88, 501) && !rdna4g_dumpdir_ok(0, 1, 0, 501, 0) && !rdna4g_dumpdir_ok(0, 1, 0, 0, 501));
    CHECK("G10: a failed lstat or a plain file is refused", !rdna4g_dumpdir_ok(-1, 1, 0, 501, 501) && !rdna4g_dumpdir_ok(0, 0, 0, 501, 501));
    CHECK("G10: any group / other bit is narrowed, 0700 is left alone", rdna4g_dumpdir_needs_narrow(0777) && rdna4g_dumpdir_needs_narrow(0770) && rdna4g_dumpdir_needs_narrow(0707) && rdna4g_dumpdir_needs_narrow(0755) && rdna4g_dumpdir_needs_narrow(01777) && !rdna4g_dumpdir_needs_narrow(0700) && !rdna4g_dumpdir_needs_narrow(0500));
    CHECK("G10: the parent is /private/tmp/garnet-xlate", !strcmp(RDNA4G_DUMP_DIR, "/private/tmp/garnet-xlate"));
    // ---- G12 (build 6, C1 finding A): the parent (sticky 01777) and the per-uid subdirectory ----
    CHECK("G12: parent: a real sticky-1777 directory owned by root, by the caller or by the daemon user is usable", rdna4g_dumproot_ok(0, 1, 0, 0, 041777, 501) && rdna4g_dumproot_ok(0, 1, 0, 501, 041777, 501) && rdna4g_dumproot_ok(0, 1, 0, 88, 01777, 88) && rdna4g_dumproot_ok(0, 1, 0, RDNA4G_DAEMON_UID, 01777, 501));
    CHECK("G12: parent: a symlink, a failed lstat or a plain file is refused", !rdna4g_dumproot_ok(0, 0, 1, 0, 01777, 501) && !rdna4g_dumproot_ok(-1, 1, 0, 0, 01777, 501) && !rdna4g_dumproot_ok(0, 0, 0, 0, 01777, 501));
    CHECK("G12: parent: owned by a THIRD user is refused even at 1777 (a planted one)", !rdna4g_dumproot_ok(0, 1, 0, 502, 01777, 501) && !rdna4g_dumproot_ok(0, 1, 0, 88, 01777, 501));
    CHECK("G12: parent: without the sticky bit or world-write it is refused (0700 from a pre-build-6 WindowServer, 0777 without sticky, 0755)", !rdna4g_dumproot_ok(0, 1, 0, 88, 0700, 88) && !rdna4g_dumproot_ok(0, 1, 0, 0, 0777, 501) && !rdna4g_dumproot_ok(0, 1, 0, 0, 0755, 501) && !rdna4g_dumproot_ok(0, 1, 0, 0, 01755, 501));
    CHECK("G12: parent: extra setuid / setgid bits are refused", !rdna4g_dumproot_ok(0, 1, 0, 0, 03777, 501) && !rdna4g_dumproot_ok(0, 1, 0, 0, 05777, 501));
    CHECK("G12: parent repair: only a parent that is OURS and not 1777 is repaired", rdna4g_dumproot_needs_fix(88, 88, 0700) && rdna4g_dumproot_needs_fix(501, 501, 01755) && !rdna4g_dumproot_needs_fix(501, 501, 01777) && !rdna4g_dumproot_needs_fix(0, 501, 0700) && !rdna4g_dumproot_needs_fix(88, 501, 0700));
    { char sp[64];
      CHECK("G12: the subdirectory is /private/tmp/garnet-xlate/<euid>", rdna4g_dumpsub_path(sp, sizeof sp, 501) && !strcmp(sp, "/private/tmp/garnet-xlate/501"));
      CHECK("G12: WindowServer (88) keeps ITS OWN subdirectory", rdna4g_dumpsub_path(sp, sizeof sp, 88) && !strcmp(sp, "/private/tmp/garnet-xlate/88"));
      CHECK("G12: distinct uids give distinct subdirectories, uid 0 included, the largest uid fits", rdna4g_dumpsub_path(sp, sizeof sp, 0) && !strcmp(sp, "/private/tmp/garnet-xlate/0") && rdna4g_dumpsub_path(sp, sizeof sp, 4294967294u) && !strcmp(sp, "/private/tmp/garnet-xlate/4294967294"));
      CHECK("G12: a too-small buffer yields 0 and an empty string", rdna4g_dumpsub_path(sp, 8, 501) == 0 && sp[0] == 0); }
    CHECK("G12: the daemon user is nobody (uid -2)", RDNA4G_DAEMON_UID == 4294967294u);
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
    // ---- G14 (build 6, C1 finding B; Metal 3 parity Task 8.2): the GPU family claim ----
    CHECK("G14: Metal3 (5001) is claimed for apps and WindowServer", rdna4g_supports_family(RDNA4G_FAMILY_METAL3, 0) && rdna4g_supports_family(RDNA4G_FAMILY_METAL3, 1) && RDNA4G_FAMILY_METAL3 == 5001);
    CHECK("G14: Mac1..2 and Common1..3 are kept", rdna4g_supports_family(RDNA4G_FAMILY_MAC1, 0) && rdna4g_supports_family(RDNA4G_FAMILY_MAC1, 1) && RDNA4G_FAMILY_MAC1 == 2001 && rdna4g_supports_family(2002, 0) && rdna4g_supports_family(3001, 0) && rdna4g_supports_family(3002, 0) && rdna4g_supports_family(3003, 0) && rdna4g_supports_family(3001, 1));
    { int any = 0; static const long no[] = { 1001, 1002, 1003, 1004, 1005, 1006, 1007, 1008, 1009, 1010, 4001, 4002, 5002, 6001, 0, -1, 99999 };
      for (unsigned i = 0; i < sizeof no / sizeof no[0]; i++) any |= rdna4g_supports_family(no[i], 0) | rdna4g_supports_family(no[i], 1);
      CHECK("G14: Apple1..10 (incl. Apple6), MacCatalyst1..2, Metal4 and unknown values are all NO", !any); }
    CHECK("G14: argument buffer tier constants", RDNA4G_ARGBUF_TIER1 == 0 && RDNA4G_ARGBUF_TIER2 == 1);
    { static const unsigned long yes_fs[] = {
          RDNA4G_FS_MACOS_GPUFAMILY1_V1, RDNA4G_FS_MACOS_GPUFAMILY1_V2,
          RDNA4G_FS_MACOS_RW_TEXTURE_T2, RDNA4G_FS_MACOS_GPUFAMILY1_V3,
          RDNA4G_FS_MACOS_GPUFAMILY1_V4, RDNA4G_FS_MACOS_GPUFAMILY2_V1
      };
      int all_fs = 1;
      for (unsigned i = 0; i < sizeof yes_fs / sizeof yes_fs[0]; i++)
          all_fs &= rdna4g_supports_feature_set(yes_fs[i]);
      CHECK("G14: macOS feature sets 10000..10005 are YES", all_fs);
      static const unsigned long no_fs[] = { 0, 1, 9999, 10006, 20000, 30000, 99999 };
      int any_bad_fs = 0;
      for (unsigned i = 0; i < sizeof no_fs / sizeof no_fs[0]; i++)
          any_bad_fs |= rdna4g_supports_feature_set(no_fs[i]);
      CHECK("G14: iOS/tvOS/out-of-range feature sets are NO", !any_bad_fs); }
    // ---- G15 (bundle 8, G6): sandboxed applications open RDNA4N on the accelerator port the bundle holds ----
    CHECK("G15: WindowServer keeps Mesa's default service (no setter call)", rdna4g_mesa_service_for_class(RDNA4G_CLASS_WS) == NULL);
    CHECK("G15: an RDNA4M_ALLOW root tool keeps Mesa's default service", rdna4g_mesa_service_for_class(RDNA4G_CLASS_ROOT) == NULL);
    CHECK("G15: an application (CLASS_OTHER) opens on GarnetAccelerator", rdna4g_mesa_service_for_class(RDNA4G_CLASS_OTHER) && !strcmp(rdna4g_mesa_service_for_class(RDNA4G_CLASS_OTHER), "GarnetAccelerator"));
    CHECK("G15: no other class value selects the accelerator", rdna4g_mesa_service_for_class(-1) == NULL && rdna4g_mesa_service_for_class(3) == NULL && rdna4g_mesa_service_for_class(99) == NULL);
    CHECK("G15: the setter's export name", !strcmp(RDNA4G_MESA_SETTER, "radv_darwin_set_service_class"));
    // ---- G11 (0.0.641): the bundle build ----
    { char *pl = slurp(dir, "Info.plist");
      CHECK("G11: Info.plist: bundle version 0.1.1, build 44", pl && strstr(pl, "<key>CFBundleShortVersionString</key>\n\t<string>0.1.1</string>") && strstr(pl, "<key>CFBundleVersion</key>\n\t<string>44</string>"));
      free(pl); }
    { char *gm = slurp(dir, "GarnetDevice.m");
      const int assembled = gm && strstr(gm, "@implementation RDNA4RenderPipelineState") != NULL;
      char *gd = assembled ? NULL : slurp(dir, "garnet_device.m");
      char *gh = assembled ? NULL : slurp(dir, "garnet_metal_internal.h");
      char *rp = assembled ? NULL : slurp(dir, "garnet_pipelines.m");
      char *cp = assembled ? NULL : slurp(dir, "garnet_pipelines.m");
      char *re = assembled ? NULL : slurp(dir, "garnet_commands.m");
      char *ce = assembled ? NULL : slurp(dir, "garnet_commands.m");
      char *bf = assembled ? NULL : slurp(dir, "garnet_resources.m");
      char *rs = assembled ? NULL : slurp(dir, "garnet_resources.m");
      char *cb = assembled ? NULL : slurp(dir, "garnet_commands.m");
      char *sr = assembled ? NULL : slurp(dir, "garnet_shaders.m");
      char *bk = assembled ? NULL : slurp(dir, "garnet_backend.m");
      char *sb = assembled ? NULL : slurp(dir, "garnet_pipelines.m");
      char *ag = assembled ? NULL : slurp(dir, "garnet_arguments.m");
      const char *devSrc = assembled ? gm : gd;
      const char *hdrSrc = assembled ? gm : gh;
      const char *rpSrc = assembled ? gm : rp;
      const char *cpSrc = assembled ? gm : cp;
      const char *reSrc = assembled ? gm : re;
      const char *ceSrc = assembled ? gm : ce;
      const char *bfSrc = assembled ? gm : bf;
      const char *rsSrc = assembled ? gm : rs;
      const char *cbSrc = assembled ? gm : cb;
      const char *srSrc = assembled ? gm : sr;
      const char *bkSrc = assembled ? gm : bk;
      const char *sbSrc = assembled ? gm : sb;
      CHECK("G14: GarnetDevice.m exposes Metal2/Metal3 capability and out-of-scope selectors",
            devSrc && strstr(devSrc, "- (BOOL)areRasterOrderGroupsSupported")
               && strstr(devSrc, "- (BOOL)supportsBCTextureCompression")
               && strstr(devSrc, "- (BOOL)supports32BitFloatFiltering")
               && strstr(devSrc, "- (BOOL)supports32BitMSAA")
               && strstr(devSrc, "- (BOOL)supportsVertexAmplificationCount:(NSUInteger)count { return count == 1; }")
               && strstr(devSrc, "- (BOOL)supportsRaytracing { return rdna4_rt_runtime_supports_raytracing(); }")
               && strstr(devSrc, "- (BOOL)supportsRaytracingFromRender { return rdna4_rt_runtime_supports_raytracing_from_render(); }")
               && strstr(devSrc, "- (BOOL)supportsPrimitiveMotionBlur { return rdna4_rt_runtime_supports_primitive_motion_blur(); }")
               && strstr(devSrc, "- (BOOL)supportsDynamicLibraries { return rdna4_dynlib_vft_translator_ready(); }")
               && strstr(devSrc, "- (BOOL)supportsRenderDynamicLibraries { return rdna4_dynlib_vft_translator_ready(); }")
               && strstr(devSrc, "- (BOOL)supportsFunctionPointers { return rdna4_dynlib_vft_translator_ready(); }")
               && strstr(devSrc, "- (BOOL)supportsFunctionPointersFromRender { return rdna4_dynlib_vft_translator_ready(); }")
               && strstr(devSrc, "- (BOOL)supportsBinaryArchives { return rdna4x_active(); }")
               && strstr(devSrc, "- (BOOL)supportsPullModelInterpolation")
               && strstr(devSrc, "rdna4dc_metal3_pull_model_ok(&m3)")
               && strstr(devSrc, "- (BOOL)supportsShaderBarycentricCoordinates")
               && strstr(devSrc, "rdna4dc_metal3_barycentric_ok(&m3)")
               && strstr(devSrc, "- (NSUInteger)minimumLinearTextureAlignmentForPixelFormat:(MTLPixelFormat)pf")
               && strstr(devSrc, "- (BOOL)supportsFeatureSet:(NSUInteger)featureSet")
               && strstr(devSrc, "- (BOOL)deviceSupportsFeatureSet:(NSUInteger)featureSet"));
      CHECK("G14: GarnetDevice.m wires Wave 2A counters, binary archive, IO queue, and residency set factories",
            gm && devSrc && rpSrc
               && (assembled || (strstr(gm, "#include \"garnet_shaders.m\"")
                                 && strstr(gm, "#include \"garnet_commands.m\"")
                                 && strstr(gm, "#include \"garnet_resources.m\"")
                                 && strstr(gm, "#include \"garnet_sparse_io.m\"")))
               && strstr(devSrc, "(id<MTLBinaryArchive>)[[RDNA4BinaryArchive alloc] initWithDevice:self descriptor:descriptor error:error]")
               && strstr(rpSrc, "rdna4_ba_lookup_render(")
               && strstr(rpSrc, "rdna4_ba_vkcache()")
               && strstr(devSrc, "rdna4_device_counter_sets()")
               && strstr(devSrc, "rdna4_device_supports_counter_sampling(")
               && strstr(devSrc, "rdna4_device_new_counter_sample_buffer(")
               && strstr(devSrc, "rdna4_device_sample_timestamps(")
               && strstr(devSrc, "[[RDNA4IOFileHandle alloc] initWithDevice:self")
               && strstr(devSrc, "[[RDNA4IOCommandQueue alloc] initWithDevice:self descriptor:descriptor error:error]")
               && strstr(devSrc, "[[RDNA4ResidencySet alloc] initWithDevice:self descriptor:descriptor error:error]"));
      CHECK("G14: GarnetDevice.m wires Wave 3A sparse/PRT and ray-tracing acceleration structure modules",
            gm && devSrc && cbSrc
               && (assembled ? (strstr(gm, "@implementation RDNA4ResourceStateEncoder")
                                && strstr(gm, "@implementation GarnetAccelerationStructureCommandEncoder"))
                             : (strstr(gm, "#include \"garnet_sparse_io.m\"")
                                && strstr(gm, "#include \"garnet_rt_runtime.m\"")
                                && strstr(gm, "#include \"garnet_rt_accel.m\"")
                                && strstr(gm, "#include \"garnet_rt_plan.m\"")
                                && strstr(gm, "#include \"garnet_rt_encoder.m\"")
                                && strstr(gm, "#include \"garnet_mesh.m\"")))
               && strstr(devSrc, "rdna4_sparse_heap_new(self, d, NULL)")
               && strstr(devSrc, "rdna4_device_sparse_tile_size_in_bytes()")
               && strstr(devSrc, "rdna4_device_sparse_tile_size_for_page_size(")
               && strstr(devSrc, "rdna4_device_sparse_tile_size(")
               && strstr(devSrc, "rdna4_device_sparse_tile_size_with_page_size(")
               && strstr(devSrc, "rdna4_device_convert_sparse_pixel_regions(")
               && strstr(devSrc, "rdna4_device_convert_sparse_tile_regions(")
               && strstr(devSrc, "rdna4_cb_new_resource_state_encoder(self, nil)")
               && strstr(devSrc, "rdna4_cb_new_resource_state_encoder(self, d)")
               && strstr(devSrc, "rdna4_rt_device_sizes(self, d)")
               && strstr(devSrc, "rdna4_rt_device_new_with_size(self, size)")
               && strstr(devSrc, "rdna4_rt_device_new_with_descriptor(self, d)")
               && strstr(devSrc, "[[GarnetAccelerationStructureCommandEncoder alloc] initWithCommandBuffer:self descriptor:nil]")
               && strstr(devSrc, "[[GarnetAccelerationStructureCommandEncoder alloc] initWithCommandBuffer:self descriptor:d]")
               && strstr(devSrc, "\"MTLResourceStateCommandEncoder\"")
               && strstr(devSrc, "\"MTLAccelerationStructure\"")
               && strstr(devSrc, "\"MTLAccelerationStructureCommandEncoder\"")
               && strstr(cbSrc, "objc_getClass(\"GarnetAccelerationStructure\")")
               && strstr(cbSrc, "rdna4CurrentHandle")
               && strstr(cbSrc, "objc_getClass(\"RDNA4SparseHeap\")"));
      CHECK("G14: GarnetDevice.m wires Wave 3B dynlib/VFT, AS buffer build-input bit, and sparse residency",
            gm && devSrc && rpSrc && cpSrc && reSrc && ceSrc && bfSrc && rsSrc && srSrc
               && (assembled ? (strstr(gm, "@implementation RDNA4DynamicLibrary")
                                && strstr(gm, "@implementation RDNA4FunctionHandle")
                                && strstr(gm, "@implementation RDNA4VisibleFunctionTable"))
                             : (strstr(gm, "#include \"garnet_shaders.m\"") != NULL))
               && strstr(devSrc, "(id<MTLDynamicLibrary>)[[RDNA4DynamicLibrary alloc] initWithDevice:self library:library error:error]")
               && strstr(devSrc, "(id<MTLDynamicLibrary>)[[RDNA4DynamicLibrary alloc] initWithDevice:self url:url error:error]")
               && strstr(devSrc, "\"MTLDynamicLibrary\"")
               && strstr(devSrc, "\"MTLFunctionHandle\"")
               && strstr(devSrc, "\"MTLVisibleFunctionTable\"")
               && strstr(cpSrc, "[(id<MTLComputePipelineState>)obj supportIndirectCommandBuffers]")
               && strstr(cpSrc, "rdna4_lk_error(fn)")
               && strstr(cpSrc, "rdna4x_parse_vft_from_meta(")
               && strstr(cpSrc, "- (id<MTLFunctionHandle>)functionHandleWithFunction:(id<MTLFunction>)function")
               && strstr(cpSrc, "- (id<MTLVisibleFunctionTable>)newVisibleFunctionTableWithDescriptor:(MTLVisibleFunctionTableDescriptor *)descriptor")
               && strstr(srSrc, "const BOOL requireStage2 = (dynlibs.count > 0 || visFns.count > 0 || deps.count > 0 || f || bf || pf || gg || pl || addBin);")
               && strstr(devSrc, "!noStage2Fallback && rdna4_fallback_ok()")
               && strstr(rpSrc, "rdna4_vft_validate_render_footnote10(")
               && strstr(rpSrc, "rdna4x_parse_vft_from_meta(")
               && strstr(rpSrc, "stage2Only")
               && strstr(rpSrc, "noStage2Fallback || !rdna4_fallback_ok()")
               && strstr(rpSrc, "- (id<MTLFunctionHandle>)functionHandleWithFunction:(id<MTLFunction>)function stage:(MTLRenderStages)stage")
               && strstr(rpSrc, "- (id<MTLVisibleFunctionTable>)newVisibleFunctionTableWithDescriptor:(MTLVisibleFunctionTableDescriptor *)descriptor stage:(MTLRenderStages)stage")
               && strstr(ceSrc, "- (void)setVisibleFunctionTable:(id<MTLVisibleFunctionTable>)t atBufferIndex:(NSUInteger)i")
               && strstr(ceSrc, "- (void)setVisibleFunctionTables:(const id<MTLVisibleFunctionTable> __unsafe_unretained *)t withBufferRange:(NSRange)r")
               && strstr(reSrc, "- (void)setVertexVisibleFunctionTable:(id<MTLVisibleFunctionTable>)t atBufferIndex:(NSUInteger)i")
               && strstr(reSrc, "- (void)setVertexVisibleFunctionTables:(const id<MTLVisibleFunctionTable> __unsafe_unretained *)t withBufferRange:(NSRange)r")
               && strstr(reSrc, "- (void)setFragmentVisibleFunctionTable:(id<MTLVisibleFunctionTable>)t atBufferIndex:(NSUInteger)i")
               && strstr(reSrc, "- (void)setFragmentVisibleFunctionTables:(const id<MTLVisibleFunctionTable> __unsafe_unretained *)t withBufferRange:(NSRange)r")
               && strstr(reSrc, "setFragmentVisibleFunctionTable:atBufferIndex: ignored") == NULL
               && strstr(bfSrc, "VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR")
               && strstr(rsSrc, "objc_getClass(\"RDNA4SparseHeap\")")
               && strstr(rsSrc, "sparse textures/heaps are not supported") == NULL);
      CHECK("G14: GarnetDevice.m wires Wave 3E mesh shaders and ray-tracing runtime/IFT",
            gm && devSrc && hdrSrc && rpSrc && cpSrc && reSrc && ceSrc && cbSrc && srSrc && bkSrc && sbSrc
               && (assembled ? (strstr(gm, "@implementation RDNA4IntersectionFunctionTable")
                                && strstr(gm, "@implementation RDNA4MeshRenderPipelineState")
                                && strstr(gm, "rdna4_rt_arg_is_rt_data_type(") != NULL)
                             : (strstr(gm, "#include \"garnet_rt_runtime.m\"") != NULL
                                && strstr(gm, "#include \"garnet_mesh.m\"") != NULL
                                && ag != NULL
                                && strstr(ag, "rdna4_rt_arg_is_rt_data_type(") != NULL
                                && strstr(ag, "- (void)setAccelerationStructure:(id<MTLAccelerationStructure>)accelerationStructure atIndex:(NSUInteger)index") != NULL
                                && strstr(ag, "- (void)setIntersectionFunctionTable:(id<MTLIntersectionFunctionTable>)intersectionFunctionTable atIndex:(NSUInteger)index") != NULL
                                && strstr(ag, "- (void)setIntersectionFunctionTables:(const id<MTLIntersectionFunctionTable> __unsafe_unretained *)intersectionFunctionTables withRange:(NSRange)range") != NULL))
               && strstr(hdrSrc, "#define RDNA4_HAS_MULTI_DRAW_INDIRECT 1")
               && strstr(bkSrc, ".multiDrawIndirect = pf.multiDrawIndirect")
               && strstr(bkSrc, "RDNA4R.multiDrawIndirect = ef.multiDrawIndirect")
               && strstr(srSrc, "garnetx_detect_stage")
               && strstr(srSrc, "garnetx_translate_stage3")
               && strstr(srSrc, "rdna4rtr_infer_ift_kind_from_ir(")
               && strstr(devSrc, "\"MTLIntersectionFunctionTable\"")
               && strstr(devSrc, "RDNA4MeshRenderPipelineState")
               && strstr(devSrc, "- (id<MTLRenderPipelineState>)newRenderPipelineStateWithMeshDescriptor:")
               && strstr(devSrc, "rdna4_mesh_device_new_pipeline(self, d, opts, refl, error)")
               && strstr(devSrc, "rdna4_mesh_device_new_pipeline_async(self, d, opts, h)")
               && strstr(hdrSrc, "RDNA4RTStageSlots rt;")
               && strstr(cbSrc, "rdna4_rt_descriptor_pool_append(")
               && strstr(cbSrc, "VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:")
               && strstr(cbSrc, "rdna4_rt_fill_accel_descriptor(")
               && strstr(cpSrc, "rdna4_rt_parse_meta(")
               && strstr(cpSrc, "rdna4_rt_pipeline_function_handle(")
               && strstr(cpSrc, "rdna4_rt_pipeline_new_ift(")
               && strstr(hdrSrc, "#define RDNA4_HAS_MESH_PIPELINE_QUERY 1")
               && strstr(rpSrc, "rdna4_rt_parse_meta(")
               && strstr(rpSrc, "rdna4rtr_refl_uses_raytracing(")
               && strstr(rpSrc, "rdna4_rt_pipeline_function_handle(")
               && strstr(rpSrc, "rdna4_rt_pipeline_new_ift(")
               && strstr(ceSrc, "- (void)setAccelerationStructure:(id<MTLAccelerationStructure>)accel atBufferIndex:(NSUInteger)i")
               && strstr(ceSrc, "- (void)setIntersectionFunctionTable:(id<MTLIntersectionFunctionTable>)t atBufferIndex:(NSUInteger)i")
               && strstr(ceSrc, "- (void)setIntersectionFunctionTables:(const id<MTLIntersectionFunctionTable> __unsafe_unretained *)t withBufferRange:(NSRange)r")
               && strstr(ceSrc, "rdna4_rt_retain_argbuf_resources(")
               && strstr(reSrc, "- (void)setVertexAccelerationStructure:(id<MTLAccelerationStructure>)accel atBufferIndex:(NSUInteger)i")
               && strstr(reSrc, "- (void)setFragmentAccelerationStructure:(id<MTLAccelerationStructure>)accel atBufferIndex:(NSUInteger)i")
               && strstr(reSrc, "- (void)setVertexIntersectionFunctionTable:(id<MTLIntersectionFunctionTable>)t atBufferIndex:(NSUInteger)i")
               && strstr(reSrc, "- (void)setFragmentIntersectionFunctionTable:(id<MTLIntersectionFunctionTable>)t atBufferIndex:(NSUInteger)i")
               && strstr(reSrc, "rdna4_rt_dc_push_accel_sig(")
               && strstr(reSrc, "if (_psoFast.isMesh)")
               && strstr(rpSrc, "out->isMesh = [self rdna4IsMeshPipeline]")
               && strstr(reSrc, "rdna4_rt_retain_argbuf_resources("));
      free(gd);
      free(gh);
      free(ag);
      free(sb);
      free(bk);
      free(sr);
      free(cb);
      free(rs);
      free(bf);
      free(ce);
      free(re);
      free(cp);
      free(rp);
      free(gm); }
    // ---- G16 (Release 1 Task 3.3): dual-key denylist & sandbox fallback ----
    CHECK("G16: denylist applies only to class OTHER (WindowServer & RDNA4M_ALLOW=1 root immune)",
          !rdna4g_denylist_applies(RDNA4G_CLASS_WS) && !rdna4g_denylist_applies(RDNA4G_CLASS_ROOT) && rdna4g_denylist_applies(RDNA4G_CLASS_OTHER));
    CHECK("G16: sandbox fallback covers ENOENT/ENOTDIR/EPERM/EACCES",
          rdna4g_denylist_open_fallback(ENOENT) && rdna4g_denylist_open_fallback(ENOTDIR) &&
          rdna4g_denylist_open_fallback(EPERM) && rdna4g_denylist_open_fallback(EACCES));
    CHECK("G16: exact bundle: and proc: matching ignores comments, blanks, partial/16-char prefix overlaps",
          rdna4g_denylist_line_matches("# bundle:com.example.App\n", "com.example.App", "App") == RDNA4G_DENY_NONE &&
          rdna4g_denylist_line_matches("   \r\n", "com.example.App", "App") == RDNA4G_DENY_NONE &&
          rdna4g_denylist_line_matches("  bundle: com.example.VeryLongNameMainApp\r\n", "com.example.VeryLongNameMainApp", "Helper") == RDNA4G_DENY_BUNDLE &&
          rdna4g_denylist_line_matches("bundle:com.example.VeryLongNameMainApp\n", "com.example.VeryLongNameHelperApp", "Helper") == RDNA4G_DENY_NONE &&
          rdna4g_denylist_line_matches("proc:BadRendererTool  \n", "com.example.Other", "BadRendererTool") == RDNA4G_DENY_PROC &&
          rdna4g_denylist_line_matches("proc:BadRenderer\n", "com.example.Other", "BadRendererTool") == RDNA4G_DENY_NONE);
    {
        const char *tmpd = getenv("TMPDIR");
        if (!tmpd || !tmpd[0]) tmpd = "/tmp";
        char dpath[512];
        snprintf(dpath, sizeof dpath, "%s/test-gate-denylist.%d.txt", tmpd, (int)getpid());
        FILE *fp = fopen(dpath, "w");
        CHECK("G16: create temp denylist fixture", fp != NULL);
        if (fp) {
            fprintf(fp, "# comment\n\nbundle:com.bad.LongBundleIdentifierOne\nproc:BadHelperExact\n");
            fclose(fp);
            int openErr = 0;
            CHECK("G16: bundle match denies class OTHER",
                  rdna4g_denylist_eval(RDNA4G_CLASS_OTHER, dpath, "com.bad.LongBundleIdentifierOne", "GoodName", &openErr) == RDNA4G_DENY_BUNDLE && openErr == 0);
            CHECK("G16: distinct long bundle sharing 16-char prefix is NOT falsely denied",
                  rdna4g_denylist_eval(RDNA4G_CLASS_OTHER, dpath, "com.bad.LongBundleIdentifierTwo", "GoodName", &openErr) == RDNA4G_DENY_NONE);
            CHECK("G16: proc match denies class OTHER",
                  rdna4g_denylist_eval(RDNA4G_CLASS_OTHER, dpath, "com.good.App", "BadHelperExact", &openErr) == RDNA4G_DENY_PROC);
            CHECK("G16: WindowServer and ROOT remain immune even on matching denylist",
                  rdna4g_denylist_eval(RDNA4G_CLASS_WS, dpath, "com.bad.LongBundleIdentifierOne", "BadHelperExact", &openErr) == RDNA4G_DENY_NONE &&
                  rdna4g_denylist_eval(RDNA4G_CLASS_ROOT, dpath, "com.bad.LongBundleIdentifierOne", "BadHelperExact", &openErr) == RDNA4G_DENY_NONE);
            if (geteuid() != 0 && chmod(dpath, 0000) == 0) {
                int epermErr = 0;
                const int rc = rdna4g_denylist_eval(RDNA4G_CLASS_OTHER, dpath, "com.good.App", "GoodName", &epermErr);
                CHECK("G16: unreadable denylist (EACCES) safely falls back to NONE for normal apps",
                      rc == RDNA4G_DENY_NONE && rdna4g_denylist_open_fallback(epermErr));
                (void)chmod(dpath, 0600);
            }
            unlink(dpath);
        }
        int missErr = 0, usedLeg = 0;
        CHECK("G16: missing denylist file (ENOENT) safely falls back to NONE",
              rdna4g_denylist_eval(RDNA4G_CLASS_OTHER, dpath, "com.good.App", "GoodName", &missErr) == RDNA4G_DENY_NONE && missErr == ENOENT);
        char lpath[512];
        snprintf(lpath, sizeof lpath, "%s/test-gate-denylist-legacy.%d.txt", tmpd, (int)getpid());
        FILE *lfp = fopen(lpath, "w");
        if (lfp) {
            fprintf(lfp, "proc:LegacyDeniedProc\n");
            fclose(lfp);
            CHECK("G16: missing primary denylist falls back to legacy denylist and flags deprecation",
                  rdna4g_denylist_eval_with_fallback(RDNA4G_CLASS_OTHER, dpath, lpath, "com.good.App", "LegacyDeniedProc", &missErr, &usedLeg) == RDNA4G_DENY_PROC && usedLeg == 1);
            FILE *pfp = fopen(dpath, "w");
            if (pfp) { fprintf(pfp, "# empty primary\n"); fclose(pfp); }
            CHECK("G16: existing primary denylist takes precedence and does NOT read legacy denylist",
                  rdna4g_denylist_eval_with_fallback(RDNA4G_CLASS_OTHER, dpath, lpath, "com.good.App", "LegacyDeniedProc", &missErr, &usedLeg) == RDNA4G_DENY_NONE && usedLeg == 0);
            unlink(dpath);
            unlink(lpath);
        }
    }
    // ---- G17: one-way Seatbelt sandbox latch ----
    {
        int latched = 0, next = -1;
        test_sb_probe_calls = 0; test_sb_probe_ret = 0;
        CHECK("G17: unsandboxed returns 0 without latching",
              rdna4g_sb_check_latch(latched, test_sb_probe_fn, NULL, &next) == 0 && next == 0 && test_sb_probe_calls == 1);
        latched = next;
        CHECK("G17: negative error from sandbox_check does not falsely latch",
              (test_sb_probe_ret = -1, rdna4g_sb_check_latch(latched, test_sb_probe_fn, NULL, &next) == 0) && next == 0 && test_sb_probe_calls == 2);
        latched = next;
        test_sb_probe_ret = 1; // late Seatbelt entry after framework init
        CHECK("G17: late Seatbelt entry is observed and latched to 1",
              rdna4g_sb_check_latch(latched, test_sb_probe_fn, NULL, &next) == 1 && next == 1 && test_sb_probe_calls == 3);
        latched = next;
        test_sb_probe_ret = 0;
        CHECK("G17: once latched to 1, probe_fn is skipped and stays 1",
              rdna4g_sb_check_latch(latched, test_sb_probe_fn, NULL, &next) == 1 && next == 1 && test_sb_probe_calls == 3);
        char *bk = slurp(dir, "garnet_backend.m");
        CHECK("G17: garnet_backend.m uses rdna4g_sb_check_latch one-way latch",
              bk && strstr(bk, "rdna4g_sb_check_latch(") != NULL &&
              strstr(bk, "sandboxed = (fn && fn(getpid(), NULL, 0) != 0) ? 1 : 0;") == NULL);
        // ---- G18: ABI 1.14 kernel-authoritative sandbox gate & headless ----
        CHECK("G18: CLASS_OTHER defers UNREADABLE kill file while ROOT still fails closed and PRESENT always declines",
              !rdna4g_kill_declines_for_class(RDNA4G_CLASS_OTHER, RDNA4G_KILL_UNREADABLE, 0) &&
              rdna4g_kill_declines_for_class(RDNA4G_CLASS_ROOT, RDNA4G_KILL_UNREADABLE, 0) &&
              !rdna4g_kill_declines_for_class(RDNA4G_CLASS_WS, RDNA4G_KILL_UNREADABLE, 0) &&
              rdna4g_kill_declines_for_class(RDNA4G_CLASS_OTHER, RDNA4G_KILL_PRESENT, 0));
        CHECK("G18: CLASS_OTHER defers filtered Garnet,Ready while ROOT fails closed and explicit 0 always declines",
              !rdna4g_ready_declines_for_class(RDNA4G_CLASS_OTHER, RDNA4G_FLAG_ABSENT) &&
              !rdna4g_ready_declines_for_class(RDNA4G_CLASS_OTHER, RDNA4G_FLAG_NONUB) &&
              rdna4g_ready_declines_for_class(RDNA4G_CLASS_OTHER, 0) &&
              rdna4g_ready_declines_for_class(RDNA4G_CLASS_ROOT, RDNA4G_FLAG_ABSENT) &&
              !rdna4g_ready_declines_for_class(RDNA4G_CLASS_WS, RDNA4G_FLAG_ABSENT));
        CHECK("G18: ABI 1.14 probe admits deferred sandbox app when READY=1 and KILL_SWITCH=0",
              rdna4g_probe_verdict_v2(1, 0, 0, 0, RDNA4G_INFO_READY | RDNA4G_INFO_DISPLAY_ACTIVE, RDNA4G_BUDGET_APP, 14u, 1, 1) == RDNA4G_PROBE_OK);
        CHECK("G18: ABI 1.14 probe declines when KILL_SWITCH=1 or READY=0",
              rdna4g_probe_verdict_v2(1, 0, 0, 0, RDNA4G_INFO_READY | RDNA4G_INFO_KILL_SWITCH, RDNA4G_BUDGET_APP, 14u, 0, 0) == RDNA4G_PROBE_KILLED &&
              rdna4g_probe_verdict_v2(1, 0, 0, 0, 0u, RDNA4G_BUDGET_APP, 14u, 0, 0) == RDNA4G_PROBE_NOTREADY);
        CHECK("G18: ABI <= 1.13 fails closed on deferred unreadable kill/ready gates, admits clean readable",
              rdna4g_probe_verdict_v2(1, 0, 0, 0, 0u, RDNA4G_BUDGET_APP, 13u, 1, 0) == RDNA4G_PROBE_KILLED &&
              rdna4g_probe_verdict_v2(1, 0, 0, 0, 0u, RDNA4G_BUDGET_APP, 13u, 0, 1) == RDNA4G_PROBE_NOTREADY &&
              rdna4g_probe_verdict_v2(1, 0, 0, 0, 0u, RDNA4G_BUDGET_APP, 13u, 0, 0) == RDNA4G_PROBE_OK);
        CHECK("G18: headless uses DISPLAY_ACTIVE when marker stat gets EPERM/EACCES on ABI >= 1.14",
              rdna4g_headless_from_probe(0, 0, 0, 0u, 0u) == 0 &&
              rdna4g_headless_from_probe(-1, EPERM, 1, 14u, RDNA4G_INFO_DISPLAY_ACTIVE) == 0 &&
              rdna4g_headless_from_probe(-1, EACCES, 1, 14u, RDNA4G_INFO_DISPLAY_ACTIVE) == 0 &&
              rdna4g_headless_from_probe(-1, EPERM, 1, 14u, 0u) == 1 &&
              rdna4g_headless_from_probe(-1, ENOENT, 1, 14u, RDNA4G_INFO_DISPLAY_ACTIVE) == 1 &&
              rdna4g_headless_from_probe(-1, EPERM, 1, 13u, RDNA4G_INFO_DISPLAY_ACTIVE) == 1);
        CHECK("G18: all probe verdicts 0..8 have descriptive text",
              strcmp(rdna4g_probe_text(RDNA4G_PROBE_NOTREADY), "?") != 0 &&
              strcmp(rdna4g_probe_text(RDNA4G_PROBE_KILLED), "?") != 0);
        CHECK("G18: garnet_backend.m wires kernel-authoritative sandbox admission and headless",
              bk && strstr(bk, "rdna4g_kill_declines_for_class(") != NULL &&
              strstr(bk, "rdna4g_ready_declines_for_class(") != NULL &&
              strstr(bk, "rdna4g_probe_verdict_v2(") != NULL &&
              strstr(bk, "rdna4g_headless_from_probe(") != NULL);
        free(bk);
    }
    printf("test-gate: %d checks, %d failed\n", runs, fails);
    return fails ? 1 : 0;
}
