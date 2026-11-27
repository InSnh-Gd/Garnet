// garnet_gate.h - the pure half of the bundle's load policy outside WindowServer (GPU-apps stage G4, kext 0.0.640; sandbox/app load policy).
// No Metal, no IOKit, no I/O: host test test-gate.c compiles this very header and drives the very functions GarnetDevice.m calls (rdna4_admit, rdna4_pc_open, rdna4_fallback_ok).
//
// Three kinds of process load the bundle (every Metal-enumerating process does):
//   WS    WindowServer (getprogname() == "WindowServer"). Today's rules, bit for bit: the safety gates only decline on a POSITIVE reading.
//   ROOT  a root tool with RDNA4M_ALLOW=1 (mtlprobe and friends). The same gates, but they now FAIL CLOSED (below).
//   OTHER any other process (an ordinary user application). Admitted only if the KERNEL admits it: the bundle opens (and at once closes) a probe connection to GarnetDriver and
//         reads QueryInfo; the kernel's open policy (uid >= 501 + rdna4-apps=1 + rdna4-multisession=1 + the allow-list) is the only decision, never the process name. The kernel must
//         also report the APP role (ABI 1.10 budget flags): a root process the kernel admits as root stays declined unless it set RDNA4M_ALLOW=1, as before. 0.0.641: and a process with
//         euid < 501 (root, daemons) is not probed at all (rdna4g_declined_by_class).
//
// FAIL CLOSED outside WindowServer. The G2 review found the three safety gates fail OPEN in a sandbox: stat("/private/tmp/garnet-off") is denied (EPERM, not ENOENT) and was read as
// "no kill file"; the nub properties Garnet,Ready / Garnet,AutoDisarmed are filtered by the sandbox's iokit-get-properties allow-list, so a denied read came back as "absent" and was
// read as "proceed". Outside WindowServer an answer that is not a positive "all clear" now declines:
//   * kill file: only ENOENT / ENOTDIR (a real "no such file") means absent; any other stat failure is UNREADABLE and declines.
//   * Garnet,Ready must be PRESENT and 1 (the kext publishes it on every nub since 0.0.612). Absent / unreadable / 0 declines.
//   * Garnet,AutoDisarmed: 1 declines. (The kext does not publish this property today, so an absent or denied read cannot be told apart from "not published": it cannot fail closed.
//     The display-pipe state is WindowServer's concern; an APP session never reaches the scanout selectors. Recorded as an open doubt.)
//   * the probe's QueryInfo: unreadable, RDNA4N_INFO_HUNG set, or the APP role not reported, declines.
#ifndef RDNA4_GATE_H
#define RDNA4_GATE_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#define RDNA4G_CLASS_WS    0
#define RDNA4G_CLASS_ROOT  1
#define RDNA4G_CLASS_OTHER 2
static inline int rdna4g_class(int isWs, int allowRoot) { return isWs ? RDNA4G_CLASS_WS : allowRoot ? RDNA4G_CLASS_ROOT : RDNA4G_CLASS_OTHER; }

// 0.0.641 (G4 review HIGH 2): only a process whose EFFECTIVE uid is an ordinary user's (>= 501, the same bound as the kernel's kAppMinUid) is ever probed. A root process without RDNA4M_ALLOW=1 and a
// system daemon (euid < 501, _windowserver's 88 included when it is not named WindowServer) is declined BY CLASS, exactly as in 0.0.632, with no kernel open at all: the probe would otherwise open a real
// session as root for every Metal-enumerating daemon (stalling WindowServer's exclusive open with the switches off, resetting the seqno as first opener).
#define RDNA4G_APP_MIN_EUID 501u
static inline int rdna4g_declined_by_class(int cls, uint32_t euid) { return cls == RDNA4G_CLASS_OTHER && euid < RDNA4G_APP_MIN_EUID; }
#define RDNA4G_DECLINE_BY_CLASS_TEXT "process is not WindowServer and GARNET_ALLOW=1 (as root) is not set"

// ---- environment variable fallback (1-release compatibility: GARNET_* primary, RDNA4M_* fallback) ----
static inline const char *rdna4g_getenv_pair(const char *newVal, const char *legacyVal, int *usedLegacyOut) {
    if (usedLegacyOut) *usedLegacyOut = 0;
    if (newVal != (const char *)0) return newVal;
    if (legacyVal != (const char *)0) {
        if (usedLegacyOut) *usedLegacyOut = 1;
        return legacyVal;
    }
    return (const char *)0;
}

// ---- persistent dual-key application denylist (bundle:<CFBundleIdentifier> & proc:<name>) ----
#define RDNA4G_DENYLIST_PATH        "/Library/Application Support/Garnet/native/app-denylist.txt"
#define RDNA4G_LEGACY_DENYLIST_PATH "/Library/Application Support/RDNA4/native/app-denylist.txt"
#define RDNA4G_DENY_NONE        0
#define RDNA4G_DENY_BUILTIN     1
#define RDNA4G_DENY_BUNDLE      2
#define RDNA4G_DENY_PROC        3
static inline int rdna4g_denylist_applies(int cls) { return cls == RDNA4G_CLASS_OTHER; }
// Strict sandboxed apps get EPERM/EACCES under /Library/Application Support, and clean installs may omit the file (ENOENT).
// Never fail closed here: fall back to the built-in table and let kernel `appdeny` enforce process-level blocks.
static inline int rdna4g_denylist_open_fallback(int err) { return err == ENOENT || err == ENOTDIR || err == EPERM || err == EACCES; }
static inline const char *rdna4g_denylist_builtin_match(const char *bundleId, const char *procName) {
    static const char *const kBuiltinBundles[] = { (const char *)0 };
    static const char *const kBuiltinProcs[] = { (const char *)0 };
    if (bundleId && bundleId[0]) {
        for (size_t i = 0; kBuiltinBundles[i]; i++) if (strcmp(bundleId, kBuiltinBundles[i]) == 0) return kBuiltinBundles[i];
    }
    if (procName && procName[0]) {
        for (size_t i = 0; kBuiltinProcs[i]; i++) if (strcmp(procName, kBuiltinProcs[i]) == 0) return kBuiltinProcs[i];
    }
    return (const char *)0;
}
static inline int rdna4g_denylist_line_matches(const char *line, const char *bundleId, const char *procName) {
    if (!line) return RDNA4G_DENY_NONE;
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == 0 || *p == '\n' || *p == '\r' || *p == '#') return RDNA4G_DENY_NONE;
    size_t len = strlen(p);
    while (len > 0 && (p[len - 1] == '\n' || p[len - 1] == '\r' || p[len - 1] == ' ' || p[len - 1] == '\t')) len--;
    if (len > 7 && strncmp(p, "bundle:", 7) == 0) {
        const char *val = p + 7;
        size_t vlen = len - 7;
        while (vlen > 0 && (*val == ' ' || *val == '\t')) { val++; vlen--; }
        if (vlen > 0 && bundleId && bundleId[0] && strlen(bundleId) == vlen && strncmp(bundleId, val, vlen) == 0)
            return RDNA4G_DENY_BUNDLE;
    } else if (len > 5 && strncmp(p, "proc:", 5) == 0) {
        const char *val = p + 5;
        size_t vlen = len - 5;
        while (vlen > 0 && (*val == ' ' || *val == '\t')) { val++; vlen--; }
        if (vlen > 0 && procName && procName[0] && strlen(procName) == vlen && strncmp(procName, val, vlen) == 0)
            return RDNA4G_DENY_PROC;
    }
    return RDNA4G_DENY_NONE;
}
static inline int rdna4g_denylist_check_stream(FILE *fp, const char *bundleId, const char *procName) {
    if (!fp) return RDNA4G_DENY_NONE;
    char buf[512];
    while (fgets(buf, sizeof buf, fp) != NULL) {
        const size_t blen = strlen(buf);
        if (blen > 0 && buf[blen - 1] != '\n') { int ch; while ((ch = fgetc(fp)) != EOF && ch != '\n') {} }
        const int m = rdna4g_denylist_line_matches(buf, bundleId, procName);
        if (m != RDNA4G_DENY_NONE) return m;
    }
    return RDNA4G_DENY_NONE;
}
static inline int rdna4g_denylist_eval(int cls, const char *path, const char *bundleId, const char *procName, int *openErrOut) {
    if (openErrOut) *openErrOut = 0;
    if (!rdna4g_denylist_applies(cls)) return RDNA4G_DENY_NONE;
    if (rdna4g_denylist_builtin_match(bundleId, procName) != (const char *)0) return RDNA4G_DENY_BUILTIN;
    if (!path || !path[0]) return RDNA4G_DENY_NONE;
    FILE *fp = fopen(path, "r");
    if (!fp) {
        const int err = errno;
        if (openErrOut) *openErrOut = err;
        (void)rdna4g_denylist_open_fallback(err);
        return RDNA4G_DENY_NONE;
    }
    const int m = rdna4g_denylist_check_stream(fp, bundleId, procName);
    fclose(fp);
    return m;
}
static inline int rdna4g_denylist_eval_with_fallback(int cls, const char *primaryPath, const char *legacyPath, const char *bundleId, const char *procName, int *openErrOut, int *usedLegacyOut) {
    if (usedLegacyOut) *usedLegacyOut = 0;
    int primaryErr = 0;
    const int m = rdna4g_denylist_eval(cls, primaryPath, bundleId, procName, &primaryErr);
    if (m != RDNA4G_DENY_NONE) {
        if (openErrOut) *openErrOut = 0;
        return m;
    }
    if ((primaryErr == ENOENT || primaryErr == ENOTDIR) && legacyPath && legacyPath[0]) {
        int legacyErr = 0;
        const int lm = rdna4g_denylist_eval(cls, legacyPath, bundleId, procName, &legacyErr);
        if (legacyErr == 0 && usedLegacyOut) *usedLegacyOut = 1;
        if (openErrOut) *openErrOut = (legacyErr == ENOENT || legacyErr == ENOTDIR) ? primaryErr : legacyErr;
        return lm;
    }
    if (openErrOut) *openErrOut = primaryErr;
    return RDNA4G_DENY_NONE;
}

// ---- the kill file ----
#define RDNA4G_KILL_ABSENT     0
#define RDNA4G_KILL_PRESENT    1
#define RDNA4G_KILL_UNREADABLE 2
static inline int rdna4g_kill_state(int statRc, int err) {
    if (statRc == 0) return RDNA4G_KILL_PRESENT;
    return (err == ENOENT || err == ENOTDIR) ? RDNA4G_KILL_ABSENT : RDNA4G_KILL_UNREADABLE;
}
static inline int rdna4g_kill_state_pair(int newRc, int newErr, int legacyRc, int legacyErr, int *usedLegacyOut) {
    if (usedLegacyOut) *usedLegacyOut = 0;
    const int ns = rdna4g_kill_state(newRc, newErr);
    if (ns != RDNA4G_KILL_ABSENT) return ns;
    const int ls = rdna4g_kill_state(legacyRc, legacyErr);
    if (ls == RDNA4G_KILL_PRESENT && usedLegacyOut) *usedLegacyOut = 1;
    return ls;
}
// ignored: the root test bypass (RDNA4M_TEST_IGNORE_KILL) applied to a PRESENT file. WindowServer: only a present file declines (today). Everyone else: anything but a clean "absent" declines.
static inline int rdna4g_kill_declines(int isWs, int state, int ignored) {
    if (state == RDNA4G_KILL_PRESENT) return !ignored;
    return isWs ? 0 : (state != RDNA4G_KILL_ABSENT);
}

// ---- the nub flags: -1 = property absent (or filtered: the registry cannot tell), -2 = the nub (parent entry) could not be reached, 0 / 1 = the value ----
#define RDNA4G_FLAG_ABSENT   (-1)
#define RDNA4G_FLAG_NONUB    (-2)
static inline int rdna4g_ready_declines(int isWs, int flag) { return isWs ? (flag == 0) : (flag != 1); }
static inline int rdna4g_autodisarm_declines(int isWs, int flag) { (void)isWs; return flag == 1; }

// ---- which service the RDNA4N connection is opened on (G6, bundle 8) ----
// A sandboxed application's profile admits user clients only on a service that conforms to IOAccelerator (IOAccelerator user-client routing). The accelerator port this bundle was initialised with IS such a
// service, and the aux kext's GarnetAccelerator hands out the same native client on it. So the APP class (CLASS_OTHER: not WindowServer, not an RDNA4M_ALLOW root tool) probes on THAT port and tells Mesa to open
// "GarnetAccelerator" (radv_darwin_set_service_class); WindowServer and root tools keep Mesa's default, GarnetDriver, untouched (no setter call at all).
#define RDNA4G_SVC_ACCEL        "GarnetAccelerator"
static inline const char *rdna4g_mesa_service_for_class(int cls) { return cls == RDNA4G_CLASS_OTHER ? RDNA4G_SVC_ACCEL : (const char *)0; }   // NULL = leave Mesa's default
#define RDNA4G_MESA_SETTER      "radv_darwin_set_service_class"

// ---- the kernel probe ----
#define RDNA4G_UC_TYPE          0x47524E54u   // 'GRNT'
#define RDNA4G_SEL_HELLO        0u
#define RDNA4G_SEL_QUERYINFO    1u
#define RDNA4G_HELLO_F_MINOR    (1ull << 8)
#define RDNA4G_INFO_SIZE        192u
#define RDNA4G_INFO_FLAGS_OFF   8u            // rdna4n_info.flags
#define RDNA4G_INFO_HUNG        1u
#define RDNA4G_INFO_BUDGET_OFF  172u          // rdna4n_info.reserved[3] (ABI 1.10): the budget flags word
#define RDNA4G_BUDGET_APP       2u            // RDNA4N_BUDGET_APP: the kernel admitted this session as an allow-listed APPLICATION (not as root)
#define RDNA4G_KR_EXCLUSIVE     0xe00002c5u   // kIOReturnExclusiveAccess: every session slot is busy, or a previous client is still closing
#define RDNA4G_PROBE_RETRIES    50            // x 10 ms: the same courtesy Mesa's winsys extends to a client that is still closing
#define RDNA4G_PROBE_OK     0
#define RDNA4G_PROBE_NOSVC  1
#define RDNA4G_PROBE_OPEN   2
#define RDNA4G_PROBE_HELLO  3
#define RDNA4G_PROBE_INFO   4
#define RDNA4G_PROBE_HUNG   5
#define RDNA4G_PROBE_NOTAPP 6
static inline int rdna4g_probe_retry(uint32_t openRc, int tries) { return openRc == RDNA4G_KR_EXCLUSIVE && tries < RDNA4G_PROBE_RETRIES; }
// budgetFlags: rdna4n_info.reserved[3]. A root process is admitted by the kernel too, as root - without RDNA4M_ALLOW=1 that must stay declined, so the verdict
// needs the kernel to say APP.
static inline int rdna4g_probe_verdict(int haveSvc, uint32_t openRc, uint32_t helloRc, uint32_t infoRc, uint32_t infoFlags, uint32_t budgetFlags) {
    if (!haveSvc) return RDNA4G_PROBE_NOSVC;
    if (openRc != 0u) return RDNA4G_PROBE_OPEN;
    if (helloRc != 0u) return RDNA4G_PROBE_HELLO;
    if (infoRc != 0u) return RDNA4G_PROBE_INFO;
    if ((infoFlags & RDNA4G_INFO_HUNG) != 0u) return RDNA4G_PROBE_HUNG;
    if ((budgetFlags & RDNA4G_BUDGET_APP) == 0u) return RDNA4G_PROBE_NOTAPP;
    return RDNA4G_PROBE_OK;
}
static inline const char *rdna4g_probe_text(int v) {
    switch (v) {
    case RDNA4G_PROBE_OK:    return "the kernel admitted this process";
    case RDNA4G_PROBE_NOSVC: return "process is not WindowServer or a GARNET_ALLOW=1 root tool, and the GarnetDriver service was not found (or the sandbox denied the lookup)";
    case RDNA4G_PROBE_OPEN:  return "process is not WindowServer or a GARNET_ALLOW=1 root tool, and the kernel refused its RDNA4N open (not allow-listed, or the user-application role is off)";
    case RDNA4G_PROBE_HELLO: return "the kernel probe's Hello failed";
    case RDNA4G_PROBE_INFO:  return "the kernel probe's QueryInfo could not be read";
    case RDNA4G_PROBE_HUNG:  return "the kernel reports the GPU HUNG";
    case RDNA4G_PROBE_NOTAPP: return "the kernel admitted this process as root, not as an allow-listed application (a root tool needs GARNET_ALLOW=1)";
    default:               return "?";
    }
}

// ---- the pipeline cache file (T2) ----
// WS and ROOT: as before (the test-hook directory when allowed, else /private/var/tmp when writable, else /private/tmp; one shared file name). OTHER: a file of its OWN in the user's cache
// directory (confstr _CS_DARWIN_USER_CACHE_DIR, which ends with '/'), named after the program; with no such directory there is NO cache (never /private/var/tmp: a user's file there would
// sit in the sticky directory beside WindowServer's and could block its rename). Returns the path length, 0 = no cache.
static inline size_t rdna4g_pcache_path(char *out, size_t n, int cls, const char *envDirAllowed, int varTmpWritable, const char *userCacheDir, const char *prog) {
    if (n == 0) return 0;
    out[0] = 0;
    int w;
    if (cls == RDNA4G_CLASS_OTHER) {
        if (!userCacheDir || !userCacheDir[0]) return 0;
        char nm[48]; size_t k = 0;
        for (const char *p = prog; p && *p && k < 40; p++) {
            const char c = *p;
            nm[k++] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_') ? c : '_';
        }
        if (k == 0) { memcpy(nm, "app", 3); k = 3; }
        nm[k] = 0;
        const size_t dl = strlen(userCacheDir);
        w = snprintf(out, n, "%s%sgarnet-pipecache.%s.bin", userCacheDir, (userCacheDir[dl - 1] == '/') ? "" : "/", nm);
    } else {
        const char *dir = (envDirAllowed && envDirAllowed[0]) ? envDirAllowed : (varTmpWritable ? "/private/var/tmp" : "/private/tmp");
        w = snprintf(out, n, "%s/garnet-pipecache.bin", dir);
    }
    if (w <= 0 || (size_t)w >= n) { out[0] = 0; return 0; }
    return (size_t)w;
}
// Who writes the file: WindowServer; an RDNA4M_ALLOW root tool only with RDNA4M_PCACHE_SAVE; an OTHER process (an application) its own file.
static inline int rdna4g_pcache_save(int cls, int rootSaveEnv) { return cls == RDNA4G_CLASS_WS ? 1 : cls == RDNA4G_CLASS_ROOT ? rootSaveEnv : 1; }

// ---- a spvcache miss ----
// Temporary placeholders keep WindowServer and admitted apps alive on a miss;
// hot-swap replaces them when the real translation appears. Others keep the NSError.
static inline int rdna4g_fallback_ok(int isWs, int force, int testAsWs, int appAdmitted) {
    return isWs || force || testAsWs || appAdmitted;
}

// ---- the AIR dump directories (0.0.641 safety rules, 0.0.652 / bundle build 6: ONE SUBDIRECTORY PER UID; C1 finding A) ----
// 0.0.641: /tmp/rdna4m was created 0700 and used only when owned by the caller. That made it per-BOOT, not per-user: the first uid to create it (WindowServer, uid 88) locked every uid-501 app out of
// dumping, so an admitted app's spvcache misses stayed magenta for good (sandbox shader-cache compatibility).
// Now:  PARENT  /tmp/rdna4m            a real directory (lstat, never a symlink), mode 01777 (sticky, world-writable: every uid can make its own subdirectory, none can remove another's),
//                                    owned by root, by the translate daemon's user (RDNA4G_DAEMON_UID) or by the caller. A parent that is OURS (owner == caller) with the wrong mode is repaired to 01777
//                                    without following links (fchmodat AT_SYMLINK_NOFOLLOW); a parent owned by anyone else with the wrong mode (or a symlink) means: skip dumping.
//       SUBDIR  /private/tmp/garnet-xlate/<euid>     created by that uid, 0700, lstat-checked: a real directory OWNED BY THE CALLER (a planted subdir of someone else, or a symlink, skips the dump).
//                                    The daemon's user is granted list+search on it by an ACL entry (garnet_dumpacl.h), so the mode stays 0700 for everyone else.
// The daemon scans every numeric subdirectory and trusts a file only when the directory AND the file are owned by the uid in the directory's name (pc-translate.sh).
#ifndef RDNA4G_DUMP_DIR
#define RDNA4G_DUMP_DIR "/private/tmp/garnet-xlate"   // tests (test-dumpdir.sh) compile the real helper with -DRDNA4G_DUMP_DIR pointing into a scratch directory
#endif
#define RDNA4G_DAEMON_UID 4294967294u   // `nobody` (uid -2): the unprivileged account org.garnet.translate runs as (INSTALL.md "translate daemon")
static inline int rdna4g_dumpdir_ok(int lstatRc, int isDir, int isLnk, uint32_t ownerUid, uint32_t callerUid) { return lstatRc == 0 && isDir && !isLnk && ownerUid == callerUid; }
static inline int rdna4g_dumpdir_needs_narrow(uint32_t mode) { return (mode & 0077u) != 0u; }
#define RDNA4G_DUMP_ROOT_MODE 01777u
static inline int rdna4g_dumproot_ok(int lstatRc, int isDir, int isLnk, uint32_t ownerUid, uint32_t mode, uint32_t callerUid) {
    return lstatRc == 0 && isDir && !isLnk && (ownerUid == 0u || ownerUid == callerUid || ownerUid == RDNA4G_DAEMON_UID) && (mode & 07777u) == RDNA4G_DUMP_ROOT_MODE;
}
static inline int rdna4g_dumproot_needs_fix(uint32_t ownerUid, uint32_t callerUid, uint32_t mode) { return ownerUid == callerUid && (mode & 07777u) != RDNA4G_DUMP_ROOT_MODE; }
// "<parent>/<uid>" for the calling process's own subdirectory; returns the length, 0 on overflow.
static inline size_t rdna4g_dumpsub_path(char *out, size_t n, uint32_t uid) {
    const int w = snprintf(out, n, "%s/%u", RDNA4G_DUMP_DIR, (unsigned)uid);
    if (w <= 0 || (size_t)w >= n) { if (n) out[0] = 0; return 0; }
    return (size_t)w;
}

// WindowServer's render thread must not block for seconds. Apps get a longer
// bounded wait, then both use a temporary placeholder and background hot-swap.
// Three consecutive timeouts disable waiting; a successful wait resets the budget.
#define RDNA4G_SYNC_WAIT_WS_MS 250
#define RDNA4G_SYNC_WAIT_APP_MS 3000
#define RDNA4G_SYNC_STRIKES 3
typedef struct { int strikes; } rdna4g_sync;
static inline int rdna4g_sync_wait_ms(int isWs) { return isWs ? RDNA4G_SYNC_WAIT_WS_MS : RDNA4G_SYNC_WAIT_APP_MS; }
static inline int rdna4g_syncwait_applies(int isWs, int appAdmitted, int force, int testAsWs) { return (isWs || appAdmitted) && !force && !testAsWs; }
static inline int rdna4g_sync_allowed(const rdna4g_sync *s) { return s->strikes < RDNA4G_SYNC_STRIKES; }
static inline void rdna4g_sync_result(rdna4g_sync *s, int found) {
    if (found) s->strikes = 0;
    else if (s->strikes < RDNA4G_SYNC_STRIKES) s->strikes++;
}

// ---- the GPU family claim (bundle build 6, C1 finding B; Metal 3 parity Task 8.2) ----
// Mac1..2 (Mac2 is a strict superset of Mac1 in Metal's hierarchy, required by MPS),
// Common1..3, and Metal3 (AMD 6000-series parity) are advertised for both
// applications and WindowServer. Apple Silicon families (Apple1..10, explicitly
// including Apple6 despite GFX12 Sparse/PRT), MacCatalyst, Metal4, and
// unknown values remain strictly NO.
#define RDNA4G_FAMILY_MAC1     2001
#define RDNA4G_FAMILY_MAC2     2002
#define RDNA4G_FAMILY_COMMON1  3001
#define RDNA4G_FAMILY_COMMON2  3002
#define RDNA4G_FAMILY_COMMON3  3003
#define RDNA4G_FAMILY_METAL3   5001
#define RDNA4G_ARGBUF_TIER1    0
#define RDNA4G_ARGBUF_TIER2    1
static inline int rdna4g_supports_family(long family, int isWs) {
    (void)isWs;
    switch (family) {
    case RDNA4G_FAMILY_MAC1: case RDNA4G_FAMILY_MAC2:
    case RDNA4G_FAMILY_COMMON1: case RDNA4G_FAMILY_COMMON2: case RDNA4G_FAMILY_COMMON3:
    case RDNA4G_FAMILY_METAL3: return 1;
    default: return 0;    // Apple1..10 (incl. Apple6), MacCatalyst, Metal4, unknown
    }
}

#define RDNA4G_FS_MACOS_GPUFAMILY1_V1 10000ul
#define RDNA4G_FS_MACOS_GPUFAMILY1_V2 10001ul
#define RDNA4G_FS_MACOS_RW_TEXTURE_T2 10002ul
#define RDNA4G_FS_MACOS_GPUFAMILY1_V3 10003ul
#define RDNA4G_FS_MACOS_GPUFAMILY1_V4 10004ul
#define RDNA4G_FS_MACOS_GPUFAMILY2_V1 10005ul

static inline int rdna4g_supports_feature_set(unsigned long fs) {
    return fs >= RDNA4G_FS_MACOS_GPUFAMILY1_V1 && fs <= RDNA4G_FS_MACOS_GPUFAMILY2_V1;
}

#endif /* RDNA4_GATE_H */
