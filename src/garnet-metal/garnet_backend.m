// garnet_backend.m - GarnetMetal modular compilation unit.
#if !defined(GARNET_INTERNAL) && !defined(GARNET_METAL_MODULAR_BUILD)
#define GARNET_INTERNAL static
#endif
#if !defined(GARNET_ARGUMENT_LIMIT_SNAPSHOT_ONLY)
#ifdef GARNET_METAL_MODULAR_BUILD
#include "garnet_metal_internal.h"
#endif
#endif

#if !defined(GARNET_ARGUMENT_LIMIT_SNAPSHOT_ONLY)

GARNET_INTERNAL void rdna4_log(const char *fmt, ...) {
    char buf[2048]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    os_log(OS_LOG_DEFAULT, "GarnetMetal: %{public}s", buf);
    fprintf(stderr, "GarnetMetal: %s\n", buf);
}
GARNET_INTERNAL const char *garnet_getenv(const char *newName, const char *legacyName) {
    int usedLegacy = 0;
    const char *nv = newName ? getenv(newName) : NULL;
    const char *lv = (!nv && legacyName) ? getenv(legacyName) : NULL;
    const char *v = rdna4g_getenv_pair(nv, lv, &usedLegacy);
    if (usedLegacy && legacyName && newName) {
        static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
        static char seen[32][48];
        static unsigned nseen = 0;
        int shouldLog = 0;
        pthread_mutex_lock(&mu);
        int found = 0;
        for (unsigned i = 0; i < nseen; i++) {
            if (strcmp(seen[i], legacyName) == 0) { found = 1; break; }
        }
        if (!found) {
            if (nseen < 32) {
                snprintf(seen[nseen], sizeof seen[nseen], "%s", legacyName);
                nseen++;
            }
            shouldLog = 1;
        }
        pthread_mutex_unlock(&mu);
        if (shouldLog)
            RDNA4LOG("DEPRECATED env %s used; migrate to %s (legacy fallback will be removed in a future release)", legacyName, newName);
    }
    return v;
}
// #12 R5: per-command-buffer / per-encoder lines are rate limited per call site: the first 8, then every 1024th (errors and WARN lines stay RDNA4LOG).

// ---------------------------------------------------------------------------------------------------------------
// Load policy (NATIVE-S4-M11 11b, L1-L3). Every Metal-enumerating process loads this bundle; only WindowServer, a root
// tool that opts in with GARNET_ALLOW=1 (or legacy RDNA4M_ALLOW=1), or (GPU-apps G4, kext 0.0.640) a process the KERNEL admits as an allow-listed application (decided by a
// successful RDNA4N open + QueryInfo, never by name; garnet_gate.h) may get a device. All test hooks are honoured only with GARNET_ALLOW=1 as root.
// Outside WindowServer the safety gates below FAIL CLOSED (a gate that cannot be read declines): see garnet_gate.h.
//   /private/tmp/garnet-off              exists -> decline (L1 kill file; legacy /private/tmp/rdna4m-off also checked); outside WindowServer also when stat is denied
//   /private/tmp/garnet-starts           one epoch-seconds line per WindowServer init; >= 3 within 300 s -> decline (L2)
//   nub property "Garnet,Ready" = 0    -> decline (L3); WindowServer: absent (old kexts) -> proceed; elsewhere it must be present and 1
//   nub property "Garnet,AutoDisarmed" = 1 -> decline (#12); absent -> ignored (the kext does not publish it today)
//   /private/tmp/garnet-headless-no      exists -> isHeadless NO (default YES); read once per process (legacy /private/tmp/rdna4m-headless-no fallback)
//   /private/tmp/garnet-noplanes         exists -> P2: multi-plane IOSurfaces refused as before (default: plane p of a 2/3-plane surface is accepted); read once per process
//   /private/tmp/garnet-noflip           exists -> S5.2a D-copy present OFF for the process (the kernel v1 copy continues); read once per process
//   GARNET_TEST_IGNORE_KILL=1            (root + GARNET_ALLOW=1, NOT WindowServer) -> garnet-off is IGNORED in that process only (S5.2b test bypass)
//   GARNET_TEST_FALLBACK_AS_WS=1         (root + GARNET_ALLOW=1) -> a spvcache miss gives a FALLBACK pipeline as in WindowServer (hot-swap oracle: mtlprobe hotswap)
//   GARNET_TEST_HIDE_BUNDLE_SPV=1        (root + GARNET_ALLOW=1) -> the bundle's Resources/spvcache is NOT consulted (only the side directory)
//   GARNET_TEST_SIDE_DIR=<dir>           (root + GARNET_ALLOW=1) -> replaces the side spvcache directory /private/var/tmp/garnet-spv
//   GARNET_TEST_DISPFLIP=1               (root + GARNET_ALLOW=1) -> a surface may be accepted as a display surface without the CoreDisplay backtrace signal (mtlprobe dispflip sets it)
// ---------------------------------------------------------------------------------------------------------------

GARNET_INTERNAL BOOL rdna4_is_ws(void) {
    static int v = -1;
    if (v < 0) { const char *p = getprogname(); v = (p && !strcmp(p, "WindowServer")) ? 1 : 0; }
    return v == 1;
}
GARNET_INTERNAL BOOL rdna4_allow(void) {
    const char *e = garnet_getenv("GARNET_ALLOW", "RDNA4M_ALLOW");
    return e && !strcmp(e, "1") && geteuid() == 0;
}
GARNET_INTERNAL BOOL rdna4_force_fallback(void) {
    const char *e = garnet_getenv("GARNET_FORCE_FALLBACK", "RDNA4M_FORCE_FALLBACK");
    return rdna4_allow() && e && !strcmp(e, "1");
}
// Test hook (root + GARNET_ALLOW=1): this process takes the WindowServer branch of the fallback rule (a spvcache miss yields a fallback pipeline,
// not an error) while the lookup stays real, so the hot-swap can be exercised from mtlprobe.
GARNET_INTERNAL BOOL rdna4_test_fb_as_ws(void) {
    const char *e = garnet_getenv("GARNET_TEST_FALLBACK_AS_WS", "RDNA4M_TEST_FALLBACK_AS_WS");
    return rdna4_allow() && e && !strcmp(e, "1");
}
// GPU-apps G4: 1 once the kernel admitted this (non-WindowServer, non-GARNET_ALLOW) process as an allow-listed application (rdna4_admit's probe succeeded).
GARNET_INTERNAL _Atomic int rdna4_app_admitted;
GARNET_INTERNAL BOOL rdna4_fallback_ok(void) { return rdna4g_fallback_ok(rdna4_is_ws(), rdna4_force_fallback(), rdna4_test_fb_as_ws(), atomic_load(&rdna4_app_admitted)) ? YES : NO; }

// Crash counter (#12 R3): counts only ABNORMAL ends. One line per instance start: "<epoch> <pid>". An instance that committed a
// first successful command buffer appends "<pid> <start epoch> <now>" to RDNA4_OK_FILE (rdna4_mark_clean) and no longer counts: normal
// logouts, logins and restarts do not trip the rule; >= 3 instances within 300 s that never got clean still decline.
// Old-format lines (no pid) count as before. Paths are RDNA4_STARTS_FILE / RDNA4_OK_FILE, or (test hook, needs GARNET_ALLOW)
// GARNET_TEST_COUNTER=<path> (ok file <path>.ok), which also enables the counter outside WindowServer.
static char rdna4_ok_path[1100]; static long long rdna4_start_epoch; static _Atomic int rdna4_counter_on, rdna4_clean_done;
static BOOL rdna4_counter_tripped(int *countOut) {
    const char *path = RDNA4_STARTS_FILE;
    const char *tp = garnet_getenv("GARNET_TEST_COUNTER", "RDNA4M_TEST_COUNTER");
    if (rdna4_allow() && tp && tp[0]) { path = tp; snprintf(rdna4_ok_path, sizeof rdna4_ok_path, "%s.ok", tp); }
    else if (!rdna4_is_ws()) return NO;
    else snprintf(rdna4_ok_path, sizeof rdna4_ok_path, "%s", RDNA4_OK_FILE);
    time_t now = time(NULL); int count = 1, cleaned = 0;
    NSMutableSet *clean = [NSMutableSet set];
    FILE *f = fopen(rdna4_ok_path, "r");
    if (f) { char line[96]; while (fgets(line, sizeof line, f)) { long long pid = 0, st = 0; if (sscanf(line, "%lld %lld", &pid, &st) == 2) [clean addObject:[NSString stringWithFormat:@"%lld/%lld", pid, st]]; } fclose(f); }
    f = fopen(path, "r");
    if (f) { char line[64]; while (fgets(line, sizeof line, f)) {
        long long t = 0, pid = 0; int n = sscanf(line, "%lld %lld", &t, &pid);
        if (n < 1 || t <= 0 || now - t < 0 || now - t > 300) continue;
        if (n == 2 && [clean containsObject:[NSString stringWithFormat:@"%lld/%lld", pid, t]]) { cleaned++; continue; }
        count++; }
        fclose(f); }
    rdna4_start_epoch = (long long)now;
    int fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd >= 0) { char b[48]; int n = snprintf(b, sizeof b, "%lld %d\n", (long long)now, (int)getpid()); (void)!write(fd, b, (size_t)n); close(fd); }
    else RDNA4LOG("counter: cannot append to %s (errno %d)", path, errno);
    atomic_store(&rdna4_counter_on, 1);
    RDNA4LOG("counter: %d abnormal start(s) in the last 300 s including this one (%d earlier instance(s) reached clean and are not counted)", count, cleaned);
    if (countOut) *countOut = count;
    return count >= 3;
}
// Called after the first successfully completed command buffer of an admitted instance that is counted.
GARNET_INTERNAL void rdna4_mark_clean(void) {
    if (!atomic_load(&rdna4_counter_on) || atomic_exchange(&rdna4_clean_done, 1)) return;
    int fd = open(rdna4_ok_path, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd < 0) { RDNA4LOG("clean marker: cannot append to %s (errno %d)", rdna4_ok_path, errno); return; }
    char b[96]; int n = snprintf(b, sizeof b, "%d %lld %lld\n", (int)getpid(), rdna4_start_epoch, (long long)time(NULL)); (void)!write(fd, b, (size_t)n); close(fd);
    RDNA4LOG("clean marker written to %s (first completed command buffer)", rdna4_ok_path);
}

// RDNA4G_FLAG_ABSENT (-1) = property absent (or filtered by a sandbox: the registry cannot tell), RDNA4G_FLAG_NONUB (-2) = the nub could not be reached, 0 = false/zero, 1 = true.
// Reads a numeric/boolean property from the accelerator's parent (the nub). WindowServer treats -1 and -2 alike (proceed); everything else declines on them (garnet_gate.h).
static int rdna4_nub_flag(uint32_t port, CFStringRef name) {
    io_registry_entry_t parent = 0;
    if (IORegistryEntryGetParentEntry((io_registry_entry_t)port, kIOServicePlane, &parent) != KERN_SUCCESS || !parent) return RDNA4G_FLAG_NONUB;
    CFTypeRef v = IORegistryEntryCreateCFProperty(parent, name, kCFAllocatorDefault, 0);
    IOObjectRelease(parent);
    if (!v) return RDNA4G_FLAG_ABSENT;
    int r = RDNA4G_FLAG_ABSENT;
    if (CFGetTypeID(v) == CFNumberGetTypeID()) { int x = 0; CFNumberGetValue((CFNumberRef)v, kCFNumberIntType, &x); r = x ? 1 : 0; }
    else if (CFGetTypeID(v) == CFBooleanGetTypeID()) r = CFBooleanGetValue((CFBooleanRef)v) ? 1 : 0;
    CFRelease(v);
    return r;
}

GARNET_INTERNAL NSString *rdna4_model_name(uint32_t port) {
    id value = CFBridgingRelease(IORegistryEntrySearchCFProperty(port, kIOServicePlane, CFSTR("model"),
        kCFAllocatorDefault, kIORegistryIterateRecursively | kIORegistryIterateParents));
    NSString *model = nil;
    if ([value isKindOfClass:NSString.class]) model = value;
    else if ([value isKindOfClass:NSData.class] && [value length] > 0 && [value length] <= 1024) {
        NSData *data = value;
        const char *end = memchr(data.bytes, 0, data.length);
        model = [[NSString alloc] initWithBytes:data.bytes length:end ? (NSUInteger)(end - (const char *)data.bytes) : data.length
            encoding:NSUTF8StringEncoding];
    }
    model = [model stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    return model.length ? [model copy] : [NSString stringWithUTF8String:rdna4dc_fallback().name];
}

GARNET_INTERNAL BOOL rdna4_number_u64(CFTypeRef field, uint64_t *out) {
    int64_t number = 0;
    if (!field || CFGetTypeID(field) != CFNumberGetTypeID() || CFNumberIsFloatType((CFNumberRef)field) ||
        !CFNumberGetValue((CFNumberRef)field, kCFNumberSInt64Type, &number) || number < 0) return NO;
    *out = (uint64_t)number;
    return YES;
}
GARNET_INTERNAL rdna4dc_native_memory rdna4_nub_memory(uint32_t port) {
    rdna4dc_native_memory m = {0};
    CFTypeRef value = IORegistryEntrySearchCFProperty(port, kIOServicePlane, CFSTR(RDNA4DC_MEMORY_CAPS_PROPERTY),
        kCFAllocatorDefault, kIORegistryIterateRecursively | kIORegistryIterateParents);
    if (!value) return m;
    if (CFGetTypeID(value) == CFDictionaryGetTypeID()) {
        const CFStringRef keys[] = {CFSTR("version"), CFSTR("physical"), CFSTR("pool"),
            CFSTR("visible"), CFSTR("applicationBudget"), CFSTR("maxBuffer")};
        uint64_t fields[6] = {0};
        BOOL valid = YES;
        for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
            CFTypeRef field = CFDictionaryGetValue((CFDictionaryRef)value, keys[i]);
            if (!rdna4_number_u64(field, &fields[i])) {
                valid = NO; break;
            }
        }
        if (valid) {
            m = (rdna4dc_native_memory){.version = fields[0], .physical = fields[1], .pool = fields[2],
                .visible = fields[3], .application_budget = fields[4], .max_buffer = fields[5]};
            CFTypeRef composite = CFDictionaryGetValue((CFDictionaryRef)value, CFSTR("compositeMaxBuffer"));
            (void)rdna4_number_u64(composite, &m.composite_max_buffer);
        }
    }
    CFRelease(value);
    return m;
}
// "Garnet,Ready": absent (old kexts) -> proceed. GARNET_TEST_READY overrides.
static int rdna4_nub_ready(uint32_t port) {
    const char *th = garnet_getenv("GARNET_TEST_READY", "RDNA4M_TEST_READY");
    if (rdna4_allow() && th && th[0]) return atoi(th) ? 1 : 0;
    return rdna4_nub_flag(port, CFSTR("Garnet,Ready"));
}
// #12 R3: "Garnet,AutoDisarmed" = 1 (kernel disarmed the display pipe after a fault) -> decline; absent -> ignore. GARNET_TEST_AUTODISARMED overrides.
static int rdna4_nub_autodisarmed(uint32_t port) {
    const char *th = garnet_getenv("GARNET_TEST_AUTODISARMED", "RDNA4M_TEST_AUTODISARMED");
    if (rdna4_allow() && th && th[0]) return atoi(th) ? 1 : 0;
    return rdna4_nub_flag(port, CFSTR("Garnet,AutoDisarmed"));
}

// The kernel probe (GPU-apps G4): for a process that is neither WindowServer nor an RDNA4M_ALLOW root tool the ONLY way in is the kernel's own decision. Open an RDNA4N connection exactly
// as Mesa's winsys does (IOServiceOpen type 'GRNT'), say Hello, read QueryInfo (HUNG), and close it again at once; the real, lazy RADV connection is opened later by the same process.
// G6 (bundle 8): the connection is opened on the ACCELERATOR PORT this bundle holds (the GarnetAccelerator the aux kext published), not on a GarnetDriver looked up by name: a sandboxed application's profile
// admits user clients only on an IOAccelerator service, and the accelerator's newUserClient hands out the same native client. The port is the caller's (Metal's) object: it is NOT released here.
// A successful open IS the allow-list verdict (uid >= 501, rdna4-apps=1, rdna4-multisession=1, the process on the kernel's list); any failure - including a sandbox that denies the service
// lookup or the open - declines. Returns NULL = admitted.
static const char *rdna4_app_probe(uint32_t port) {
    io_service_t svc = (io_service_t)port;   // G6: the accelerator port (0 = none); never released here
    io_connect_t conn = 0; kern_return_t kr = KERN_FAILURE, hk = KERN_FAILURE, ik = KERN_FAILURE; uint32_t infoFlags = 0, budgetFlags = 0;
    if (svc) {
        for (int tries = 0;; tries++) {
            kr = IOServiceOpen(svc, mach_task_self(), RDNA4G_UC_TYPE, &conn);
            if (!rdna4g_probe_retry((uint32_t)kr, tries)) break;
            usleep(10000);
        }
        if (kr == KERN_SUCCESS) {
            uint64_t in[2] = { 1ull, RDNA4G_HELLO_F_MINOR }, out[4] = { 0 }; uint32_t outCnt = 4;
            hk = IOConnectCallScalarMethod(conn, RDNA4G_SEL_HELLO, in, 2, out, &outCnt);
            if (hk == KERN_SUCCESS) {
                uint8_t info[RDNA4G_INFO_SIZE]; size_t isz = sizeof info; memset(info, 0, sizeof info);
                ik = IOConnectCallStructMethod(conn, RDNA4G_SEL_QUERYINFO, NULL, 0, info, &isz);
                if (ik == KERN_SUCCESS && isz == sizeof info) { memcpy(&infoFlags, info + RDNA4G_INFO_FLAGS_OFF, sizeof infoFlags); memcpy(&budgetFlags, info + RDNA4G_INFO_BUDGET_OFF, sizeof budgetFlags); } else if (ik == KERN_SUCCESS) ik = KERN_FAILURE;
            }
            IOServiceClose(conn);   // the session is released at once; Mesa opens its own later
        }
    }
    const int v = rdna4g_probe_verdict(svc != 0, (uint32_t)kr, (uint32_t)hk, (uint32_t)ik, infoFlags, budgetFlags);
    if (v != RDNA4G_PROBE_OK) RDNA4LOG("APP probe: declined (%s): open 0x%x hello 0x%x info 0x%x flags 0x%x budget 0x%x", rdna4g_probe_text(v), (unsigned)kr, (unsigned)hk, (unsigned)ik, infoFlags, budgetFlags);
    else RDNA4LOG("APP probe: the kernel admitted this process (allow-listed application): GPU path enabled for %s", getprogname());
    return v == RDNA4G_PROBE_OK ? NULL : rdna4g_probe_text(v);
}

static const char *rdna4_denylist_decline(int cls) {
    if (!rdna4g_denylist_applies(cls)) return NULL;
    NSString *bidStr = [[NSBundle mainBundle] bundleIdentifier];
    const char *bid = bidStr ? [bidStr UTF8String] : NULL;
    const char *proc = getprogname();
    const char *envPath = rdna4_allow() ? garnet_getenv("GARNET_TEST_DENYLIST", "RDNA4M_TEST_DENYLIST") : NULL;
    int openErr = 0, usedLegacy = 0;
    const int hit = (envPath && envPath[0])
        ? rdna4g_denylist_eval(cls, envPath, bid, proc, &openErr)
        : rdna4g_denylist_eval_with_fallback(cls, RDNA4G_DENYLIST_PATH, RDNA4G_LEGACY_DENYLIST_PATH, bid, proc, &openErr, &usedLegacy);
    if (usedLegacy)
        RDNA4_ONCE("DEPRECATED denylist path %s read; migrate to %s", RDNA4G_LEGACY_DENYLIST_PATH, RDNA4G_DENYLIST_PATH);
    if (hit != RDNA4G_DENY_NONE) {
        RDNA4LOG("admit: declined by denylist (%s) bundle=%s proc=%s",
                 hit == RDNA4G_DENY_BUILTIN ? "builtin" : hit == RDNA4G_DENY_BUNDLE ? "bundle" : "proc",
                 bid ? bid : "-", proc ? proc : "-");
        return "application is on the Garnet denylist";
    }
    if (openErr != 0 && !rdna4g_denylist_open_fallback(openErr))
        RDNA4LOG("admit: denylist read errno=%d (%s); falling back to kernel appdeny", openErr, strerror(openErr));
    return NULL;
}

// NULL = admit, else the reason for declining (static string).
GARNET_INTERNAL const char *rdna4_admit(uint32_t port) {
    const int cls = rdna4g_class(rdna4_is_ws(), rdna4_allow());
    const int isWs = cls == RDNA4G_CLASS_WS;
    // 0.0.641 (G4 review HIGH 2): root and daemons (euid < 501) that are neither WindowServer nor a GARNET_ALLOW tool are declined BY CLASS, first of all, exactly as in 0.0.632: no kernel open for them.
    if (rdna4g_declined_by_class(cls, (uint32_t)geteuid())) return RDNA4G_DECLINE_BY_CLASS_TEXT;
    // The kill file. Outside WindowServer a stat that fails for any reason but "no such file" (a sandbox answers EPERM) declines: fail CLOSED (garnet_gate.h).
    struct stat st;
    const int srcRc = stat(RDNA4_KILL_FILE, &st), srcErr = errno;
    int legRc = -1, legErr = ENOENT, usedLegacyKill = 0;
    if (rdna4g_kill_state(srcRc, srcErr) == RDNA4G_KILL_ABSENT) {
        legRc = stat(RDNA4_LEGACY_KILL_FILE, &st);
        legErr = errno;
    }
    const int ks = rdna4g_kill_state_pair(srcRc, srcErr, legRc, legErr, &usedLegacyKill);
    if (usedLegacyKill)
        RDNA4_ONCE("DEPRECATED kill file %s detected; migrate to %s", RDNA4_LEGACY_KILL_FILE, RDNA4_KILL_FILE);
    int killIgnored = 0;
    if (ks == RDNA4G_KILL_PRESENT) {
        // S5.2b root test path: ignored ONLY by a non-WindowServer root process with GARNET_ALLOW=1 and GARNET_TEST_IGNORE_KILL=1 (WindowServer never is).
        killIgnored = rdna4df_off_file_ignored((int)geteuid(),
            garnet_getenv("GARNET_ALLOW", "RDNA4M_ALLOW"),
            garnet_getenv("GARNET_TEST_IGNORE_KILL", "RDNA4M_TEST_IGNORE_KILL"),
            rdna4_is_ws());
        if (!killIgnored) return usedLegacyKill ? "kill file " RDNA4_LEGACY_KILL_FILE " exists" : "kill file " RDNA4_KILL_FILE " exists";
        RDNA4LOG("admit: kill file %s exists but is IGNORED (root test bypass GARNET_ALLOW=1 + GARNET_TEST_IGNORE_KILL=1, not WindowServer)",
                 usedLegacyKill ? RDNA4_LEGACY_KILL_FILE : RDNA4_KILL_FILE);
    }
    if (rdna4g_kill_declines(isWs, ks, killIgnored)) {
        const int failErr = rdna4g_kill_state(srcRc, srcErr) == RDNA4G_KILL_ABSENT ? legErr : srcErr;
        RDNA4LOG("admit: kill file " RDNA4_KILL_FILE " could not be checked (stat errno %d): declining outside WindowServer (fail closed)", failErr);
        return "kill file " RDNA4_KILL_FILE " could not be read (fail closed outside WindowServer)";
    }
    const int rdy = rdna4_nub_ready(port);
    if (rdna4g_ready_declines(isWs, rdy)) return rdy == 0 ? "nub property Garnet,Ready is 0" : "nub property Garnet,Ready is absent or unreadable (fail closed outside WindowServer)";
    if (rdna4g_autodisarm_declines(isWs, rdna4_nub_autodisarmed(port))) return "nub property Garnet,AutoDisarmed is 1";
    int count = 0;
    if (rdna4_counter_tripped(&count)) return "crash counter: >= 3 starts within 300 s";
    if (cls == RDNA4G_CLASS_OTHER) {   // not WindowServer, not a GARNET_ALLOW root tool: only the kernel can admit it
        const char *denyWhy = rdna4_denylist_decline(cls);
        if (denyWhy) return denyWhy;
        const char *why = rdna4_app_probe(port);
        if (why) return why;
        atomic_store(&rdna4_app_admitted, 1);
    }
    return NULL;
}
GARNET_INTERNAL BOOL rdna4_headless(void) {
    static BOOL v; static dispatch_once_t once;
    dispatch_once(&once, ^{
        struct stat st;
        int result = stat(RDNA4_HEADLESS_NO, &st);
        int error = result ? errno : 0;
        if (result != 0 && (error == ENOENT || error == ENOTDIR)) {
            int leg = stat(RDNA4_LEGACY_HEADLESS_NO, &st);
            if (leg == 0) {
                result = 0;
                error = 0;
                RDNA4_ONCE("DEPRECATED marker %s used; migrate to %s", RDNA4_LEGACY_HEADLESS_NO, RDNA4_HEADLESS_NO);
            }
        }
        v = result != 0;
        RDNA4LOG("isHeadless=%d markerResult=%d errno=%d process=%s",
                 v, result, error, getprogname());
    });
    return v;
}
#endif // !defined(GARNET_ARGUMENT_LIMIT_SNAPSHOT_ONLY)
#ifndef RDNA4_ARGUMENT_LIMIT_SNAPSHOT_INC
#define RDNA4_ARGUMENT_LIMIT_SNAPSHOT_INC

#include "garnet_argument_limit_snapshot.h"

#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/*
 * st_mtime is the portable POSIX spelling on both Darwin and Linux.  Apple
 * clang exposes the same seconds through st_mtimespec, but strict C modes and
 * cross-sysroot checks do not guarantee that member name.  The inode, size and
 * same-handle re-export carry the identity proof; seconds are a metadata
 * backstop, so resetting the nanosecond component keeps this source portable.
 */
#define RDNA4_ARGUMENT_LIMIT_MTIME_SEC(st) ((st).st_mtime)
#define RDNA4_ARGUMENT_LIMIT_MTIME_NSEC(st) (0)

static void rdna4_argument_limit_copy_string(char *dst, size_t capacity,
                                             const char *src) {
    if (!dst || !capacity) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, capacity, "%s", src);
}

static void rdna4_argument_limit_record_source(
    rdna4_argument_limit_snapshot *snapshot, const char *path) {
    if (!snapshot || !path || !path[0] || strlen(path) >= sizeof(snapshot->source_path))
        return;
    rdna4_argument_limit_copy_string(snapshot->source_path,
                                     sizeof(snapshot->source_path), path);
    struct stat st;
    if (stat(path, &st) != 0) return;
    snapshot->source_stat_valid = 1;
    snapshot->source_device = (uint64_t)(uintmax_t)st.st_dev;
    snapshot->source_inode = (uint64_t)(uintmax_t)st.st_ino;
    snapshot->source_size = (uint64_t)(uintmax_t)st.st_size;
    snapshot->source_mtime_seconds =
        (uint64_t)RDNA4_ARGUMENT_LIMIT_MTIME_SEC(st);
    snapshot->source_mtime_nanoseconds =
        (uint64_t)RDNA4_ARGUMENT_LIMIT_MTIME_NSEC(st);
}

static int rdna4_argument_limit_policy_valid(
    const rdna4_argument_limit_policy *policy) {
    if (!policy ||
        (policy->comparison_samplers != 0 &&
         policy->comparison_samplers != 1) ||
        policy->objects_per_configuration == 0 ||
        policy->objects_per_configuration >
            RDNA4_ARGUMENT_LIMIT_DEFAULT_OBJECTS_PER_CONFIG)
        return 0;
    const uint32_t required_dummy =
        policy->comparison_samplers
            ? RDNA4_ARGUMENT_LIMIT_DEFAULT_DUMMY_OBJECTS
            : 1u;
    return policy->reserve_sampler_objects >= required_dummy;
}

static int rdna4_argument_limit_raw_valid(
    const rdna4_radv_darwin_argument_limits_v1 *raw,
    const rdna4_radv_darwin_host_facts_v1 *facts) {
    const uint32_t known_flags =
        RDNA4_RADV_DARWIN_LIMITS_READ_ONLY |
        RDNA4_RADV_DARWIN_LIMITS_NO_VULKAN_OBJECT |
        RDNA4_RADV_DARWIN_LIMITS_NO_PIPELINE_UUID;
    const uint32_t required_flags = known_flags;
    const uint32_t known_valid =
        RDNA4_RADV_DARWIN_LIMITS_VALID_STABLE_CORE |
        RDNA4_RADV_DARWIN_LIMITS_VALID_MEMORY;
    if (!raw || raw->struct_size != RDNA4_ARGUMENT_LIMIT_RAW_SIZE ||
        raw->abi_version != RDNA4_ARGUMENT_LIMIT_SOURCE_ABI ||
        raw->flags != required_flags || (raw->flags & ~known_flags) ||
        !(raw->valid_flags & RDNA4_RADV_DARWIN_LIMITS_VALID_STABLE_CORE) ||
        (raw->valid_flags & ~known_valid))
        return 0;
    if (!raw->build_identity[0] ||
        !memchr(raw->build_identity, '\0', sizeof(raw->build_identity)) ||
        strncmp(raw->build_identity, "rdal-v1 ", 8) != 0 ||
        raw->build_identity[8] == '\0')
        return 0;
    if (!raw->api_version || !raw->driver_version || !raw->driver_id ||
        !raw->max_descriptor_set_size || !raw->max_sampler_allocations ||
        raw->max_bound_descriptor_sets < 5u ||
        !raw->max_per_set_descriptors || !raw->max_storage_buffer_range ||
        !raw->max_uab_descriptors_all_pools ||
        !raw->max_per_stage_descriptor_update_after_bind_samplers ||
        !raw->max_descriptor_set_update_after_bind_samplers ||
        !raw->max_update_after_bind_descriptors_in_all_pools)
        return 0;

    const int wants_memory =
        facts && (facts->valid_flags & RDNA4_RADV_DARWIN_FACT_VRAM_BYTES);
    const int has_memory =
        (raw->valid_flags & RDNA4_RADV_DARWIN_LIMITS_VALID_MEMORY) != 0;
    if (has_memory != wants_memory) return 0;
    if (has_memory &&
        (!raw->max_memory_allocation_size || !raw->max_buffer_size))
        return 0;
    if (!has_memory &&
        (raw->max_memory_allocation_size || raw->max_buffer_size))
        return 0;
    return 1;
}

static rdna4_argument_limit_result
rdna4_argument_limit_begin(rdna4_argument_limit_snapshot *snapshot,
                           const char *path,
                           const rdna4_radv_darwin_host_facts_v1 *facts,
                           const rdna4_argument_limit_policy *policy) {
    if (!snapshot) return RDNA4_ARGUMENT_LIMIT_RESULT_INVALID_ARGUMENT;
    if (snapshot->initialized)
        return RDNA4_ARGUMENT_LIMIT_RESULT_ALREADY_FIXED;

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->struct_size = sizeof(*snapshot);
    snapshot->source_abi_version = RDNA4_ARGUMENT_LIMIT_SOURCE_ABI;
    snapshot->state = RDNA4_ARGUMENT_LIMIT_STATE_UNAVAILABLE;
    snapshot->initialized = 1;
    snapshot->policy = policy ? *policy
                              : rdna4_argument_limit_default_policy();
    if (facts) {
        snapshot->facts_present = 1;
        snapshot->input_facts = *facts;
    }
    rdna4_argument_limit_record_source(snapshot, path);
    if (!rdna4_argument_limit_policy_valid(&snapshot->policy)) {
        snapshot->init_result = RDNA4_ARGUMENT_LIMIT_RESULT_POLICY_INVALID;
        return snapshot->init_result;
    }
    return RDNA4_ARGUMENT_LIMIT_RESULT_OK;
}

static rdna4_argument_limit_result
rdna4_argument_limit_finish_export(
    rdna4_argument_limit_snapshot *snapshot, rdna4_argument_limit_export_fn fn,
    const rdna4_radv_darwin_host_facts_v1 *facts) {
    if (!fn) {
        snapshot->init_result = RDNA4_ARGUMENT_LIMIT_RESULT_EXPORT_MISSING;
        return snapshot->init_result;
    }
    snapshot->export_present = 1;
    memset(&snapshot->raw, 0, sizeof(snapshot->raw));
    snapshot->export_result = fn(facts, &snapshot->raw);
    snapshot->export_errno = errno;
    if (snapshot->export_result != 0) {
        snapshot->init_result =
            RDNA4_ARGUMENT_LIMIT_RESULT_EXPORT_CALL_FAILED;
        return snapshot->init_result;
    }
    snapshot->result_struct_size = snapshot->raw.struct_size;
    snapshot->result_abi_version = snapshot->raw.abi_version;
    snapshot->result_flags = snapshot->raw.flags;
    snapshot->result_valid_flags = snapshot->raw.valid_flags;
    memcpy(snapshot->build_identity, snapshot->raw.build_identity,
           sizeof(snapshot->build_identity));
    if (snapshot->raw.abi_version != RDNA4_ARGUMENT_LIMIT_SOURCE_ABI) {
        snapshot->init_result =
            RDNA4_ARGUMENT_LIMIT_RESULT_EXPORT_ABI_MISMATCH;
        return snapshot->init_result;
    }
    if (!rdna4_argument_limit_raw_valid(&snapshot->raw, facts)) {
        snapshot->init_result = RDNA4_ARGUMENT_LIMIT_RESULT_EXPORT_MALFORMED;
        return snapshot->init_result;
    }

    snapshot->descriptor_capacity =
        snapshot->raw.max_descriptor_set_update_after_bind_samplers <
                snapshot->raw.max_per_stage_descriptor_update_after_bind_samplers
            ? snapshot->raw.max_descriptor_set_update_after_bind_samplers
            : snapshot->raw.max_per_stage_descriptor_update_after_bind_samplers;
    const rdna4_argument_public_sampler_status plan_status =
        rdna4_argument_public_sampler_limit(
            (int)snapshot->policy.comparison_samplers,
            snapshot->descriptor_capacity,
            snapshot->raw.max_sampler_allocations,
            snapshot->policy.reserve_sampler_objects,
            snapshot->policy.objects_per_configuration,
            &snapshot->sampler_plan);
    if (plan_status == RDNA4_ARGUMENT_PUBLIC_SAMPLER_OVERFLOW) {
        snapshot->init_result = RDNA4_ARGUMENT_LIMIT_RESULT_EXPORT_MALFORMED;
        return snapshot->init_result;
    }
    if (plan_status != RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK) {
        snapshot->init_result =
            RDNA4_ARGUMENT_LIMIT_RESULT_CAPACITY_UNAVAILABLE;
        return snapshot->init_result;
    }

    snapshot->public_sampler_limit = snapshot->sampler_plan.public_limit;
    snapshot->public_sampler_limit_available = 1;
    snapshot->state = RDNA4_ARGUMENT_LIMIT_STATE_READY;
    snapshot->init_result = RDNA4_ARGUMENT_LIMIT_RESULT_OK;
    return snapshot->init_result;
}

static rdna4_argument_limit_result
rdna4_argument_limit_finish_handle(
    rdna4_argument_limit_snapshot *snapshot, void *handle,
    const rdna4_radv_darwin_host_facts_v1 *facts) {
    if (!handle) {
        snapshot->init_result =
            RDNA4_ARGUMENT_LIMIT_RESULT_LIBRARY_OPEN_FAILED;
        return snapshot->init_result;
    }

    void *symbol = NULL;
    dlerror();
    symbol = dlsym(handle, RDNA4_ARGUMENT_LIMIT_EXPORT_NAME);
    const char *error = dlerror();
    if (error) {
        rdna4_argument_limit_copy_string(
            snapshot->loader_error, sizeof(snapshot->loader_error), error);
        snapshot->init_result = RDNA4_ARGUMENT_LIMIT_RESULT_EXPORT_MISSING;
        return snapshot->init_result;
    }
    if (!symbol) {
        snapshot->init_result = RDNA4_ARGUMENT_LIMIT_RESULT_EXPORT_MISSING;
        return snapshot->init_result;
    }
    rdna4_argument_limit_export_fn fn = NULL;
    _Static_assert(sizeof(fn) == sizeof(symbol),
                   "POSIX function and object pointers differ");
    memcpy(&fn, &symbol, sizeof(fn));
    return rdna4_argument_limit_finish_export(snapshot, fn, facts);
}

rdna4_argument_limit_policy rdna4_argument_limit_default_policy(void) {
    return (rdna4_argument_limit_policy){
        .comparison_samplers = 1,
        .reserve_sampler_objects =
            RDNA4_ARGUMENT_LIMIT_DEFAULT_DUMMY_OBJECTS,
        .objects_per_configuration =
            RDNA4_ARGUMENT_LIMIT_DEFAULT_OBJECTS_PER_CONFIG,
    };
}

rdna4_argument_limit_result rdna4_argument_limit_snapshot_init_from_path(
    rdna4_argument_limit_snapshot *snapshot, const char *path,
    const rdna4_radv_darwin_host_facts_v1 *facts,
    const rdna4_argument_limit_policy *policy) {
    if (!snapshot) return RDNA4_ARGUMENT_LIMIT_RESULT_INVALID_ARGUMENT;
    if (snapshot->initialized)
        return RDNA4_ARGUMENT_LIMIT_RESULT_ALREADY_FIXED;
    if (!path || !path[0]) {
        rdna4_argument_limit_snapshot staged;
        memset(&staged, 0, sizeof(staged));
        rdna4_argument_limit_begin(&staged, path, facts, policy);
        staged.init_result = RDNA4_ARGUMENT_LIMIT_RESULT_INVALID_ARGUMENT;
        *snapshot = staged;
        return staged.init_result;
    }

    /*
     * Keep a private copy of the policy so the loader path and explicit
     * export path share one immutability rule.
     */
    rdna4_argument_limit_snapshot staged;
    memset(&staged, 0, sizeof(staged));
    rdna4_argument_limit_result result =
        rdna4_argument_limit_begin(&staged, path, facts, policy);
    if (result != RDNA4_ARGUMENT_LIMIT_RESULT_OK) {
        *snapshot = staged;
        return result;
    }

    dlerror();
    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    const char *error = handle ? NULL : dlerror();
    if (!handle) {
        rdna4_argument_limit_copy_string(
            staged.loader_error, sizeof(staged.loader_error), error);
        staged.init_result = RDNA4_ARGUMENT_LIMIT_RESULT_LIBRARY_OPEN_FAILED;
        *snapshot = staged;
        return staged.init_result;
    }
    result = rdna4_argument_limit_finish_handle(&staged, handle, facts);
    dlclose(handle);
    *snapshot = staged;
    return result;
}

rdna4_argument_limit_result rdna4_argument_limit_snapshot_init_from_handle(
    rdna4_argument_limit_snapshot *snapshot, void *handle, const char *path,
    const rdna4_radv_darwin_host_facts_v1 *facts,
    const rdna4_argument_limit_policy *policy) {
    if (!snapshot) return RDNA4_ARGUMENT_LIMIT_RESULT_INVALID_ARGUMENT;
    if (snapshot->initialized)
        return RDNA4_ARGUMENT_LIMIT_RESULT_ALREADY_FIXED;
    rdna4_argument_limit_snapshot staged;
    memset(&staged, 0, sizeof(staged));
    rdna4_argument_limit_result result =
        rdna4_argument_limit_begin(&staged, path, facts, policy);
    if (result == RDNA4_ARGUMENT_LIMIT_RESULT_OK)
        result = rdna4_argument_limit_finish_handle(&staged, handle, facts);
    *snapshot = staged;
    return result;
}

rdna4_argument_limit_result rdna4_argument_limit_snapshot_init_from_loader(
    rdna4_argument_limit_snapshot *snapshot,
    const rdna4_argument_limit_loader *loader, const char *path,
    const rdna4_radv_darwin_host_facts_v1 *facts,
    const rdna4_argument_limit_policy *policy) {
    if (!snapshot) return RDNA4_ARGUMENT_LIMIT_RESULT_INVALID_ARGUMENT;
    if (snapshot->initialized)
        return RDNA4_ARGUMENT_LIMIT_RESULT_ALREADY_FIXED;
    if (!loader || !loader->open_library || !loader->close_library ||
        !loader->resolve_symbol || !path || !path[0]) {
        rdna4_argument_limit_snapshot staged;
        memset(&staged, 0, sizeof(staged));
        rdna4_argument_limit_begin(&staged, path, facts, policy);
        staged.init_result = RDNA4_ARGUMENT_LIMIT_RESULT_INVALID_ARGUMENT;
        *snapshot = staged;
        return staged.init_result;
    }

    rdna4_argument_limit_snapshot staged;
    memset(&staged, 0, sizeof(staged));
    rdna4_argument_limit_result result =
        rdna4_argument_limit_begin(&staged, path, facts, policy);
    if (result != RDNA4_ARGUMENT_LIMIT_RESULT_OK) {
        *snapshot = staged;
        return result;
    }

    void *handle = loader->open_library(loader->context, path);
    if (!handle) {
        staged.init_result =
            RDNA4_ARGUMENT_LIMIT_RESULT_LIBRARY_OPEN_FAILED;
        *snapshot = staged;
        return staged.init_result;
    }
    void *symbol = loader->resolve_symbol(loader->context, handle,
                                          RDNA4_ARGUMENT_LIMIT_EXPORT_NAME);
    if (!symbol) {
        staged.init_result = RDNA4_ARGUMENT_LIMIT_RESULT_EXPORT_MISSING;
    } else {
        rdna4_argument_limit_export_fn fn = NULL;
        _Static_assert(sizeof(fn) == sizeof(symbol),
                       "POSIX function and object pointers differ");
        memcpy(&fn, &symbol, sizeof(fn));
        result = rdna4_argument_limit_finish_export(&staged, fn, facts);
        if (result != RDNA4_ARGUMENT_LIMIT_RESULT_OK)
            staged.init_result = result;
    }
    loader->close_library(loader->context, handle);
    *snapshot = staged;
    return staged.init_result;
}

rdna4_argument_limit_result rdna4_argument_limit_snapshot_init_from_export(
    rdna4_argument_limit_snapshot *snapshot,
    rdna4_argument_limit_export_fn export_fn,
    const rdna4_radv_darwin_host_facts_v1 *facts,
    const rdna4_argument_limit_policy *policy) {
    if (!snapshot) return RDNA4_ARGUMENT_LIMIT_RESULT_INVALID_ARGUMENT;
    if (snapshot->initialized)
        return RDNA4_ARGUMENT_LIMIT_RESULT_ALREADY_FIXED;
    rdna4_argument_limit_snapshot staged;
    memset(&staged, 0, sizeof(staged));
    rdna4_argument_limit_result result =
        rdna4_argument_limit_begin(&staged, NULL, facts, policy);
    if (result == RDNA4_ARGUMENT_LIMIT_RESULT_OK)
        result = rdna4_argument_limit_finish_export(&staged, export_fn, facts);
    *snapshot = staged;
    return result;
}

rdna4_argument_limit_result rdna4_argument_limit_snapshot_public_limit(
    const rdna4_argument_limit_snapshot *snapshot, uint32_t *out_limit) {
    if (!snapshot || !out_limit)
        return RDNA4_ARGUMENT_LIMIT_RESULT_INVALID_ARGUMENT;
    if (snapshot->state != RDNA4_ARGUMENT_LIMIT_STATE_READY ||
        !snapshot->public_sampler_limit_available)
        return RDNA4_ARGUMENT_LIMIT_RESULT_CAPACITY_UNAVAILABLE;
    *out_limit = snapshot->public_sampler_limit;
    return RDNA4_ARGUMENT_LIMIT_RESULT_OK;
}

rdna4_argument_public_sampler_status rdna4_argument_limit_snapshot_admit(
    const rdna4_argument_limit_snapshot *snapshot,
    uint32_t live_unique_configurations, int new_unique_configuration,
    uint64_t live_sampler_objects, uint64_t requested_objects, int *admitted) {
    if (!snapshot || !admitted)
        return RDNA4_ARGUMENT_PUBLIC_SAMPLER_INVALID;
    if (snapshot->state != RDNA4_ARGUMENT_LIMIT_STATE_READY ||
        !snapshot->public_sampler_limit_available)
        return RDNA4_ARGUMENT_PUBLIC_SAMPLER_LIMIT_UNAVAILABLE;
    return rdna4_argument_public_sampler_admit(
        snapshot->public_sampler_limit, live_unique_configurations,
        new_unique_configuration, live_sampler_objects, requested_objects,
        snapshot->raw.max_sampler_allocations, admitted);
}

rdna4_argument_public_sampler_status
rdna4_argument_limit_snapshot_budget_committed(
    const rdna4_argument_sampler_budget *budget,
    uint64_t live_actual_objects, uint64_t *committed) {
    if (!budget || !committed ||
        budget->reserved_objects < budget->reserved_actual_objects)
        return RDNA4_ARGUMENT_PUBLIC_SAMPLER_INVALID;
    const uint64_t unused =
        budget->reserved_objects - budget->reserved_actual_objects;
    if (live_actual_objects > UINT64_MAX - unused)
        return RDNA4_ARGUMENT_PUBLIC_SAMPLER_OVERFLOW;
    *committed = live_actual_objects + unused;
    return RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK;
}

rdna4_argument_public_sampler_status
rdna4_argument_limit_snapshot_budget_reserve_configuration(
    const rdna4_argument_limit_snapshot *snapshot,
    rdna4_argument_sampler_budget *budget,
    uint32_t live_unique_configurations, uint32_t existing_actual_handles,
    uint64_t live_actual_objects, int *admitted) {
    if (!snapshot || !budget || !admitted)
        return RDNA4_ARGUMENT_PUBLIC_SAMPLER_INVALID;
    *admitted = 0;
    if (snapshot->state != RDNA4_ARGUMENT_LIMIT_STATE_READY ||
        !snapshot->public_sampler_limit_available)
        return RDNA4_ARGUMENT_PUBLIC_SAMPLER_LIMIT_UNAVAILABLE;
    const uint32_t objects_per_configuration =
        snapshot->policy.objects_per_configuration;
    if (!objects_per_configuration ||
        existing_actual_handles > objects_per_configuration)
        return RDNA4_ARGUMENT_PUBLIC_SAMPLER_INVALID;

    uint64_t committed = 0;
    rdna4_argument_public_sampler_status status =
        rdna4_argument_limit_snapshot_budget_committed(
            budget, live_actual_objects, &committed);
    if (status != RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK) return status;
    const uint64_t requested =
        objects_per_configuration - existing_actual_handles;
    status = rdna4_argument_limit_snapshot_admit(
        snapshot, live_unique_configurations, 1, committed, requested,
        admitted);
    if (status != RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK || !*admitted)
        return status;
    if (UINT64_MAX - budget->reserved_objects <
            objects_per_configuration ||
        UINT64_MAX - budget->reserved_actual_objects <
            existing_actual_handles)
        return RDNA4_ARGUMENT_PUBLIC_SAMPLER_OVERFLOW;
    budget->reserved_objects += objects_per_configuration;
    budget->reserved_actual_objects += existing_actual_handles;
    return RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK;
}

rdna4_argument_public_sampler_status
rdna4_argument_limit_snapshot_budget_charge_reserved(
    const rdna4_argument_limit_snapshot *snapshot,
    rdna4_argument_sampler_budget *budget, int *charged) {
    if (!snapshot || !budget || !charged)
        return RDNA4_ARGUMENT_PUBLIC_SAMPLER_INVALID;
    *charged = 0;
    if (snapshot->state != RDNA4_ARGUMENT_LIMIT_STATE_READY ||
        !snapshot->public_sampler_limit_available)
        return RDNA4_ARGUMENT_PUBLIC_SAMPLER_LIMIT_UNAVAILABLE;
    if (budget->reserved_objects < budget->reserved_actual_objects)
        return RDNA4_ARGUMENT_PUBLIC_SAMPLER_INVALID;
    if (budget->reserved_actual_objects == budget->reserved_objects)
        return RDNA4_ARGUMENT_PUBLIC_SAMPLER_OBJECT_LIMIT;
    ++budget->reserved_actual_objects;
    *charged = 1;
    return RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK;
}

rdna4_argument_public_sampler_status
rdna4_argument_limit_snapshot_budget_release_reserved(
    const rdna4_argument_limit_snapshot *snapshot,
    rdna4_argument_sampler_budget *budget, int *released) {
    if (!snapshot || !budget || !released)
        return RDNA4_ARGUMENT_PUBLIC_SAMPLER_INVALID;
    *released = 0;
    if (snapshot->state != RDNA4_ARGUMENT_LIMIT_STATE_READY ||
        !snapshot->public_sampler_limit_available)
        return RDNA4_ARGUMENT_PUBLIC_SAMPLER_LIMIT_UNAVAILABLE;
    if (!budget->reserved_actual_objects ||
        budget->reserved_objects < budget->reserved_actual_objects)
        return RDNA4_ARGUMENT_PUBLIC_SAMPLER_INVALID;
    --budget->reserved_actual_objects;
    *released = 1;
    return RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK;
}

rdna4_argument_public_sampler_status
rdna4_argument_limit_snapshot_budget_release_configuration(
    const rdna4_argument_limit_snapshot *snapshot,
    rdna4_argument_sampler_budget *budget, uint32_t actual_handles,
    int *released) {
    if (!snapshot || !budget || !released)
        return RDNA4_ARGUMENT_PUBLIC_SAMPLER_INVALID;
    *released = 0;
    if (snapshot->state != RDNA4_ARGUMENT_LIMIT_STATE_READY ||
        !snapshot->public_sampler_limit_available)
        return RDNA4_ARGUMENT_PUBLIC_SAMPLER_LIMIT_UNAVAILABLE;
    const uint64_t reserved = snapshot->policy.objects_per_configuration;
    if (!reserved || actual_handles > reserved ||
        budget->reserved_objects < reserved ||
        budget->reserved_actual_objects < actual_handles)
        return RDNA4_ARGUMENT_PUBLIC_SAMPLER_INVALID;
    budget->reserved_objects -= reserved;
    budget->reserved_actual_objects -= actual_handles;
    *released = 1;
    return RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK;
}

rdna4_argument_public_sampler_status
rdna4_argument_limit_snapshot_budget_can_charge_direct(
    const rdna4_argument_limit_snapshot *snapshot,
    const rdna4_argument_sampler_budget *budget,
    uint64_t live_actual_objects, uint64_t requested_objects,
    int *admitted) {
    if (!snapshot || !budget || !admitted)
        return RDNA4_ARGUMENT_PUBLIC_SAMPLER_INVALID;
    *admitted = 0;
    if (snapshot->state != RDNA4_ARGUMENT_LIMIT_STATE_READY ||
        !snapshot->public_sampler_limit_available)
        return RDNA4_ARGUMENT_PUBLIC_SAMPLER_LIMIT_UNAVAILABLE;
    uint64_t committed = 0;
    rdna4_argument_public_sampler_status status =
        rdna4_argument_limit_snapshot_budget_committed(
            budget, live_actual_objects, &committed);
    if (status != RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK) return status;
    if (requested_objects > UINT64_MAX - committed)
        return RDNA4_ARGUMENT_PUBLIC_SAMPLER_OVERFLOW;
    if (committed + requested_objects >
        snapshot->raw.max_sampler_allocations)
        return RDNA4_ARGUMENT_PUBLIC_SAMPLER_OBJECT_LIMIT;
    *admitted = 1;
    return RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK;
}

static int rdna4_argument_limit_core_matches(
    const rdna4_radv_darwin_argument_limits_v1 *raw,
    const rdna4_argument_limit_open_observation *o) {
#define RDNA4_ARG_LIMIT_EQ(field) (raw->field == o->field)
    return RDNA4_ARG_LIMIT_EQ(max_descriptor_set_size) &&
           RDNA4_ARG_LIMIT_EQ(max_sampler_allocations) &&
           RDNA4_ARG_LIMIT_EQ(max_bound_descriptor_sets) &&
           RDNA4_ARG_LIMIT_EQ(max_per_set_descriptors) &&
           RDNA4_ARG_LIMIT_EQ(max_storage_buffer_range) &&
           RDNA4_ARG_LIMIT_EQ(max_uab_descriptors_all_pools) &&
           RDNA4_ARG_LIMIT_EQ(max_per_stage_descriptor_samplers) &&
           RDNA4_ARG_LIMIT_EQ(max_per_stage_descriptor_uniform_buffers) &&
           RDNA4_ARG_LIMIT_EQ(max_per_stage_descriptor_storage_buffers) &&
           RDNA4_ARG_LIMIT_EQ(max_per_stage_descriptor_sampled_images) &&
           RDNA4_ARG_LIMIT_EQ(max_per_stage_descriptor_storage_images) &&
           RDNA4_ARG_LIMIT_EQ(max_per_stage_descriptor_input_attachments) &&
           RDNA4_ARG_LIMIT_EQ(max_per_stage_resources) &&
           RDNA4_ARG_LIMIT_EQ(max_descriptor_set_samplers) &&
           RDNA4_ARG_LIMIT_EQ(max_descriptor_set_uniform_buffers) &&
           RDNA4_ARG_LIMIT_EQ(max_descriptor_set_uniform_buffers_dynamic) &&
           RDNA4_ARG_LIMIT_EQ(max_descriptor_set_storage_buffers) &&
           RDNA4_ARG_LIMIT_EQ(max_descriptor_set_storage_buffers_dynamic) &&
           RDNA4_ARG_LIMIT_EQ(max_descriptor_set_sampled_images) &&
           RDNA4_ARG_LIMIT_EQ(max_descriptor_set_storage_images) &&
           RDNA4_ARG_LIMIT_EQ(max_descriptor_set_input_attachments) &&
           RDNA4_ARG_LIMIT_EQ(
               max_per_stage_descriptor_update_after_bind_samplers) &&
           RDNA4_ARG_LIMIT_EQ(
               max_per_stage_descriptor_update_after_bind_uniform_buffers) &&
           RDNA4_ARG_LIMIT_EQ(
               max_per_stage_descriptor_update_after_bind_storage_buffers) &&
           RDNA4_ARG_LIMIT_EQ(
               max_per_stage_descriptor_update_after_bind_sampled_images) &&
           RDNA4_ARG_LIMIT_EQ(
               max_per_stage_descriptor_update_after_bind_storage_images) &&
           RDNA4_ARG_LIMIT_EQ(
               max_per_stage_descriptor_update_after_bind_input_attachments) &&
           RDNA4_ARG_LIMIT_EQ(max_per_stage_update_after_bind_resources) &&
           RDNA4_ARG_LIMIT_EQ(
               max_descriptor_set_update_after_bind_samplers) &&
           RDNA4_ARG_LIMIT_EQ(
               max_descriptor_set_update_after_bind_uniform_buffers) &&
           RDNA4_ARG_LIMIT_EQ(
               max_descriptor_set_update_after_bind_uniform_buffers_dynamic) &&
           RDNA4_ARG_LIMIT_EQ(
               max_descriptor_set_update_after_bind_storage_buffers) &&
           RDNA4_ARG_LIMIT_EQ(
               max_descriptor_set_update_after_bind_storage_buffers_dynamic) &&
           RDNA4_ARG_LIMIT_EQ(
               max_descriptor_set_update_after_bind_sampled_images) &&
           RDNA4_ARG_LIMIT_EQ(
               max_descriptor_set_update_after_bind_storage_images) &&
           RDNA4_ARG_LIMIT_EQ(
               max_descriptor_set_update_after_bind_input_attachments) &&
           RDNA4_ARG_LIMIT_EQ(
               max_update_after_bind_descriptors_in_all_pools) &&
           RDNA4_ARG_LIMIT_EQ(max_fragment_combined_output_resources) &&
           RDNA4_ARG_LIMIT_EQ(max_color_attachments) &&
           RDNA4_ARG_LIMIT_EQ(max_fragment_output_attachments) &&
           RDNA4_ARG_LIMIT_EQ(max_fragment_dual_src_attachments);
#undef RDNA4_ARG_LIMIT_EQ
}

rdna4_argument_limit_result rdna4_argument_limit_snapshot_crosscheck(
    const rdna4_argument_limit_snapshot *snapshot,
    const rdna4_argument_limit_open_observation *observation) {
    if (!snapshot || !observation)
        return RDNA4_ARGUMENT_LIMIT_RESULT_INVALID_ARGUMENT;
    if (snapshot->state != RDNA4_ARGUMENT_LIMIT_STATE_READY)
        return RDNA4_ARGUMENT_LIMIT_RESULT_CROSSCHECK_UNAVAILABLE;
    if (observation->struct_size != sizeof(*observation))
        return RDNA4_ARGUMENT_LIMIT_RESULT_INVALID_ARGUMENT;
    if ((observation->observed_flags &
         RDNA4_ARGUMENT_LIMIT_OBSERVED_CORE_MASK) !=
        RDNA4_ARGUMENT_LIMIT_OBSERVED_CORE_MASK)
        return RDNA4_ARGUMENT_LIMIT_RESULT_CROSSCHECK_UNAVAILABLE;
    if (snapshot->raw.api_version != observation->api_version ||
        snapshot->raw.driver_version != observation->driver_version ||
        snapshot->raw.driver_id != observation->driver_id)
        return RDNA4_ARGUMENT_LIMIT_RESULT_CROSSCHECK_IDENTITY_MISMATCH;
    if (!rdna4_argument_limit_core_matches(&snapshot->raw, observation))
        return RDNA4_ARGUMENT_LIMIT_RESULT_CROSSCHECK_LIMIT_MISMATCH;
    if ((snapshot->raw.valid_flags &
         RDNA4_RADV_DARWIN_LIMITS_VALID_MEMORY) &&
        (!(observation->observed_flags &
           RDNA4_ARGUMENT_LIMIT_OBSERVED_MEMORY_LIMITS) ||
         snapshot->raw.max_memory_allocation_size !=
             observation->max_memory_allocation_size ||
         snapshot->raw.max_buffer_size != observation->max_buffer_size))
        return RDNA4_ARGUMENT_LIMIT_RESULT_CROSSCHECK_LIMIT_MISMATCH;
    return RDNA4_ARGUMENT_LIMIT_RESULT_OK;
}

rdna4_argument_limit_result rdna4_argument_limit_snapshot_source_matches_path(
    const rdna4_argument_limit_snapshot *snapshot, const char *path) {
    if (!snapshot) return RDNA4_ARGUMENT_LIMIT_RESULT_INVALID_ARGUMENT;
    if (!snapshot->initialized || !snapshot->source_stat_valid)
        return RDNA4_ARGUMENT_LIMIT_RESULT_CROSSCHECK_UNAVAILABLE;
    const char *use = path && path[0] ? path : snapshot->source_path;
    if (!use || !use[0])
        return RDNA4_ARGUMENT_LIMIT_RESULT_CROSSCHECK_UNAVAILABLE;
    struct stat st;
    if (stat(use, &st) != 0)
        return RDNA4_ARGUMENT_LIMIT_RESULT_CROSSCHECK_UNAVAILABLE;
    if (snapshot->source_device != (uint64_t)(uintmax_t)st.st_dev ||
        snapshot->source_inode != (uint64_t)(uintmax_t)st.st_ino ||
        snapshot->source_size != (uint64_t)(uintmax_t)st.st_size ||
        snapshot->source_mtime_seconds !=
            (uint64_t)RDNA4_ARGUMENT_LIMIT_MTIME_SEC(st) ||
        snapshot->source_mtime_nanoseconds !=
            (uint64_t)RDNA4_ARGUMENT_LIMIT_MTIME_NSEC(st))
        return RDNA4_ARGUMENT_LIMIT_RESULT_SOURCE_CHANGED;
    return RDNA4_ARGUMENT_LIMIT_RESULT_OK;
}

#endif
#if !defined(GARNET_ARGUMENT_LIMIT_SNAPSHOT_ONLY)
// ---------------------------------------------------------------------------------------------------------------
// Lazy in-process RADV (NATIVE-S4-M10 route a', F5). Opened on the first resource/queue request, NEVER in
// initWithAcceleratorPort: (every Metal-enumerating process loads this bundle). RDNA4N is root-only and exclusive:
// a refusal (non-root, second client) becomes nil + NSError, never an abort.
// ---------------------------------------------------------------------------------------------------------------
#define X(n) GARNET_INTERNAL PFN_##n n;
RDNA4_VK_FUNCS(X)
#undef X
static PFN_vkCreateInstance rdna4_vkCreateInstance;
GARNET_INTERNAL PFN_vkGetInstanceProcAddr rdna4_gipa;
GARNET_INTERNAL PFN_vkGetMemoryHostPointerPropertiesEXT rdna4_vkGetMHPP;   // 11h.6: VK_EXT_external_memory_host
GARNET_INTERNAL PFN_vkGetBufferDeviceAddress rdna4_vkGetBufferAddress;
GARNET_INTERNAL PFN_vkCmdSetSampleLocationsEXT rdna4_vkCmdSetSampleLocationsEXT;
GARNET_INTERNAL PFN_vkQueueWaitIdle rdna4_vkQueueWaitIdle;
GARNET_INTERNAL RDNA4NativeBridge rdna4_native_bridge;

GARNET_INTERNAL RDNA4BackendState RDNA4R;

/*
 * One immutable pre-open limit source, independent of RDNA4R. Preparing it
 * may dlopen/dlsym the bundle dylib, but never creates a Vulkan object or
 * changes RDNA4R.tried/RDNA4R.ok. Getter and admission code only read the
 * frozen snapshot below.
 */
GARNET_INTERNAL rdna4_argument_limit_snapshot RDNA4ALS;
static dispatch_once_t rdna4_argument_limit_once;
GARNET_INTERNAL _Atomic int rdna4_argument_limit_full_open_state; /* -1 fail, 0 pending, 1 match */

static const char *rdna4_argument_limit_state_name(rdna4_argument_limit_state state) {
    switch (state) {
    case RDNA4_ARGUMENT_LIMIT_STATE_READY: return "ready";
    case RDNA4_ARGUMENT_LIMIT_STATE_UNAVAILABLE: return "unavailable";
    default: return "uninitialized";
    }
}

static const rdna4_radv_darwin_host_facts_v1 *rdna4_argument_limit_host_facts(
    const rdna4dc_native_memory *nativeMemory,
    rdna4_radv_darwin_host_facts_v1 *facts) {
    memset(facts, 0, sizeof(*facts));
    facts->struct_size = sizeof(*facts);
    if (!rdna4dc_native_memory_valid(nativeMemory) || !nativeMemory->pool)
        return NULL;
    /* Match RADV full-open vram.total_heap_size (visible + high pool, not raw physical). */
    facts->valid_flags = RDNA4_RADV_DARWIN_FACT_VRAM_BYTES;
    facts->vram_bytes = nativeMemory->pool;
    return facts;
}

GARNET_INTERNAL void rdna4_argument_limit_prepare(const rdna4dc_native_memory *nativeMemory) {
    dispatch_once(&rdna4_argument_limit_once, ^{
        NSString *path = [[[NSBundle bundleForClass:[GarnetDevice class]] resourcePath]
            stringByAppendingPathComponent:@"libvulkan_radeon.dylib"];
        rdna4_argument_limit_policy policy = rdna4_argument_limit_default_policy();
        rdna4_radv_darwin_host_facts_v1 facts;
        const rdna4_radv_darwin_host_facts_v1 *factsInput =
            rdna4_argument_limit_host_facts(nativeMemory, &facts);
        rdna4_argument_limit_result result = rdna4_argument_limit_snapshot_init_from_path(
            &RDNA4ALS, path.fileSystemRepresentation, factsInput, &policy);
        RDNA4LOG("argument limits: source %s (result %d), export=%u ABI=%u struct=%u flags=0x%x valid=0x%x identity='%s' path='%s'%s%s",
                 rdna4_argument_limit_state_name(RDNA4ALS.state), result,
                 RDNA4ALS.export_present, RDNA4ALS.result_abi_version,
                 RDNA4ALS.result_struct_size, RDNA4ALS.result_flags,
                 RDNA4ALS.result_valid_flags, RDNA4ALS.build_identity,
                 RDNA4ALS.source_path,
                 RDNA4ALS.loader_error[0] ? " loader_error=" : "",
                 RDNA4ALS.loader_error);
        if (result == RDNA4_ARGUMENT_LIMIT_RESULT_OK) {
            RDNA4LOG("argument limits: public sampler count %u = min(table %u, object %u); descriptor C=%llu, sampler objects=%u, dummy/source reserve=%u, handles/config=%u",
                     RDNA4ALS.public_sampler_limit, RDNA4ALS.sampler_plan.table_limit,
                     RDNA4ALS.sampler_plan.object_limit,
                     (unsigned long long)RDNA4ALS.descriptor_capacity,
                     RDNA4ALS.raw.max_sampler_allocations,
                     RDNA4ALS.policy.reserve_sampler_objects,
                     RDNA4ALS.policy.objects_per_configuration);
        } else {
            RDNA4LOG("argument limits: preserving the Tier1 public contract; no 0/1/16/31/96/128 fallback is published");
        }
    });
}

static rdna4_argument_limit_open_observation rdna4_argument_limit_observation(
    const VkPhysicalDeviceProperties *pp,
    const VkPhysicalDeviceVulkan12Properties *v12,
    uint32_t maxPerSetDescriptors, uint64_t maxMemoryAllocationSize,
    uint64_t maxBufferSize) {
    rdna4_argument_limit_open_observation o;
    memset(&o, 0, sizeof(o));
    o.struct_size = sizeof(o);
    o.observed_flags = RDNA4_ARGUMENT_LIMIT_OBSERVED_CORE_MASK |
                       RDNA4_ARGUMENT_LIMIT_OBSERVED_MEMORY_LIMITS;
    o.api_version = pp->apiVersion;
    o.driver_version = pp->driverVersion;
    o.driver_id = v12->driverID;
    o.max_descriptor_set_size = pp->limits.maxFragmentCombinedOutputResources;
    o.max_sampler_allocations = pp->limits.maxSamplerAllocationCount;
    o.max_bound_descriptor_sets = pp->limits.maxBoundDescriptorSets;
    o.max_per_set_descriptors = maxPerSetDescriptors;
    o.max_storage_buffer_range = pp->limits.maxStorageBufferRange;
    o.max_uab_descriptors_all_pools = v12->maxUpdateAfterBindDescriptorsInAllPools;
    o.max_per_stage_descriptor_samplers = pp->limits.maxPerStageDescriptorSamplers;
    o.max_per_stage_descriptor_uniform_buffers = pp->limits.maxPerStageDescriptorUniformBuffers;
    o.max_per_stage_descriptor_storage_buffers = pp->limits.maxPerStageDescriptorStorageBuffers;
    o.max_per_stage_descriptor_sampled_images = pp->limits.maxPerStageDescriptorSampledImages;
    o.max_per_stage_descriptor_storage_images = pp->limits.maxPerStageDescriptorStorageImages;
    o.max_per_stage_descriptor_input_attachments = pp->limits.maxPerStageDescriptorInputAttachments;
    o.max_per_stage_resources = pp->limits.maxPerStageResources;
    o.max_descriptor_set_samplers = pp->limits.maxDescriptorSetSamplers;
    o.max_descriptor_set_uniform_buffers = pp->limits.maxDescriptorSetUniformBuffers;
    o.max_descriptor_set_uniform_buffers_dynamic = pp->limits.maxDescriptorSetUniformBuffersDynamic;
    o.max_descriptor_set_storage_buffers = pp->limits.maxDescriptorSetStorageBuffers;
    o.max_descriptor_set_storage_buffers_dynamic = pp->limits.maxDescriptorSetStorageBuffersDynamic;
    o.max_descriptor_set_sampled_images = pp->limits.maxDescriptorSetSampledImages;
    o.max_descriptor_set_storage_images = pp->limits.maxDescriptorSetStorageImages;
    o.max_descriptor_set_input_attachments = pp->limits.maxDescriptorSetInputAttachments;
    o.max_per_stage_descriptor_update_after_bind_samplers = v12->maxPerStageDescriptorUpdateAfterBindSamplers;
    o.max_per_stage_descriptor_update_after_bind_uniform_buffers = v12->maxPerStageDescriptorUpdateAfterBindUniformBuffers;
    o.max_per_stage_descriptor_update_after_bind_storage_buffers = v12->maxPerStageDescriptorUpdateAfterBindStorageBuffers;
    o.max_per_stage_descriptor_update_after_bind_sampled_images = v12->maxPerStageDescriptorUpdateAfterBindSampledImages;
    o.max_per_stage_descriptor_update_after_bind_storage_images = v12->maxPerStageDescriptorUpdateAfterBindStorageImages;
    o.max_per_stage_descriptor_update_after_bind_input_attachments = v12->maxPerStageDescriptorUpdateAfterBindInputAttachments;
    o.max_per_stage_update_after_bind_resources = v12->maxPerStageUpdateAfterBindResources;
    o.max_descriptor_set_update_after_bind_samplers = v12->maxDescriptorSetUpdateAfterBindSamplers;
    o.max_descriptor_set_update_after_bind_uniform_buffers = v12->maxDescriptorSetUpdateAfterBindUniformBuffers;
    o.max_descriptor_set_update_after_bind_uniform_buffers_dynamic = v12->maxDescriptorSetUpdateAfterBindUniformBuffersDynamic;
    o.max_descriptor_set_update_after_bind_storage_buffers = v12->maxDescriptorSetUpdateAfterBindStorageBuffers;
    o.max_descriptor_set_update_after_bind_storage_buffers_dynamic = v12->maxDescriptorSetUpdateAfterBindStorageBuffersDynamic;
    o.max_descriptor_set_update_after_bind_sampled_images = v12->maxDescriptorSetUpdateAfterBindSampledImages;
    o.max_descriptor_set_update_after_bind_storage_images = v12->maxDescriptorSetUpdateAfterBindStorageImages;
    o.max_descriptor_set_update_after_bind_input_attachments = v12->maxDescriptorSetUpdateAfterBindInputAttachments;
    o.max_update_after_bind_descriptors_in_all_pools = v12->maxUpdateAfterBindDescriptorsInAllPools;
    o.max_fragment_combined_output_resources = pp->limits.maxFragmentCombinedOutputResources;
    o.max_color_attachments = pp->limits.maxColorAttachments;
    o.max_fragment_output_attachments = pp->limits.maxFragmentOutputAttachments;
    o.max_fragment_dual_src_attachments = pp->limits.maxFragmentDualSrcAttachments;
    o.max_memory_allocation_size = maxMemoryAllocationSize;
    o.max_buffer_size = maxBufferSize;
    return o;
}

static int rdna4_argument_limit_verify_open(
    const VkPhysicalDeviceProperties *pp,
    const VkPhysicalDeviceVulkan12Properties *v12,
    uint32_t maxPerSetDescriptors, uint64_t maxMemoryAllocationSize,
    uint64_t maxBufferSize,
    const rdna4_argument_limit_snapshot *seen,
    rdna4_argument_limit_result seenResult, int sourceMatches) {
    if (RDNA4ALS.state != RDNA4_ARGUMENT_LIMIT_STATE_READY) {
        RDNA4LOG("argument limits: full-open crosscheck not applicable (source unavailable)");
        atomic_store(&rdna4_argument_limit_full_open_state, 0);
        return 0;
    }
    if (!sourceMatches || seenResult != RDNA4_ARGUMENT_LIMIT_RESULT_OK ||
        !seen || memcmp(&seen->raw, &RDNA4ALS.raw, sizeof(seen->raw))) {
        RDNA4LOG("argument limits: full-open same-handle identity/raw-limit MISMATCH (source %d, re-export %d); argument tables fail closed, snapshot remains frozen",
                 sourceMatches, seenResult);
        atomic_store(&rdna4_argument_limit_full_open_state, -1);
        return -1;
    }
    rdna4_argument_limit_open_observation o =
        rdna4_argument_limit_observation(
            pp, v12, maxPerSetDescriptors, maxMemoryAllocationSize,
            maxBufferSize);
    rdna4_argument_limit_result result =
        rdna4_argument_limit_snapshot_crosscheck(&RDNA4ALS, &o);
    if (result != RDNA4_ARGUMENT_LIMIT_RESULT_OK) {
        RDNA4LOG("argument limits: full-open identity/raw-limit MISMATCH (%d); argument tables fail closed, snapshot remains frozen", result);
        atomic_store(&rdna4_argument_limit_full_open_state, -1);
        return -1;
    }
    atomic_store(&rdna4_argument_limit_full_open_state, 1);
    RDNA4LOG("argument limits: full-open identity and all raw limits MATCH the pre-open snapshot");
    return 1;
}

GARNET_INTERNAL _Atomic int rdna4_submit_fault;
// A failed submit or fence wait does not prove GPU work stopped. Keep its objects
// until process exit and reject new submissions; there is no in-process reset.
static NSMutableArray *rdna4_unresolved_commands;

// Published once, only after a successful lazy open. Getters never open RADV or
// wait for its class lock, and cannot observe a failed/partially initialized try.
GARNET_INTERNAL rdna4dc_caps RDNA4Caps;
GARNET_INTERNAL _Atomic int rdna4_caps_ready;
static rdna4x_backend_capabilities RDNA4QueryCaps;
GARNET_INTERNAL rdna4x_backend_capabilities rdna4_query_caps(void) {
    if (atomic_load_explicit(&rdna4_caps_ready, memory_order_acquire)) return RDNA4QueryCaps;
    return rdna4x_query_caps(0, 0, 0, 0, 0);
}
GARNET_INTERNAL rdna4dc_caps rdna4_device_caps(void) {
    if (atomic_load_explicit(&rdna4_caps_ready, memory_order_acquire)) return RDNA4Caps;
    return rdna4dc_fallback();
}

static rdna4_gather_caps RDNA4GatherCaps;
static _Atomic int rdna4_gather_caps_ready;
GARNET_INTERNAL rdna4_gather_caps rdna4_device_gather_caps(void) {
    if (atomic_load_explicit(&rdna4_gather_caps_ready, memory_order_acquire)) return RDNA4GatherCaps;
    return (rdna4_gather_caps){0};
}

GARNET_INTERNAL NSError *rdna4_err(NSInteger code, NSString *msg) {
    return [NSError errorWithDomain:@"GarnetMetal" code:code userInfo:@{ NSLocalizedDescriptionKey: msg }];
}



// ---------------------------------------------------------------------------------------------------------------
// native #12 bundle work (lag investigation).
// T1: cheap always-on timing, aggregated per process and drained every 10 s by one timer (no per-event logging). All values ns.
//   spv   = rdna4_spv_lookup (sha256 + spvcache file read + meta)       shm  = vkCreateShaderModule
//   gfx   = vkCreateGraphicsPipelines   cmp = vkCreateComputePipelines   imp = IOSurface / NoCopy import (vkGetMemoryHostPointerProperties..vkAllocateMemory)
//   enc   = command buffer CPU time, creation (begin) to submit entry (includes app think time between encoders)
//   gpu   = submit -> fence signalled, seen by the completion thread (a lower bound on queueing: the completion queue is SERIAL, so it also holds
//           the head-of-line wait behind earlier fences)          wake = dispatch_async enqueue -> block start on the serial completion queue
//   qlk   = time blocked acquiring RDNA4R.qlock (every acquisition is timed, contended or not)
//   fgap  = gap between submits of consecutive "frame" command buffers (those that wrote a display surface: the D-copy identification)
//   fgpu  = submit -> fence for frame command buffers
// T2: a VkPipelineCache created at device open from /private/var/tmp (or /private/tmp) and saved there (WindowServer only).
// ---------------------------------------------------------------------------------------------------------------
GARNET_INTERNAL RDNA4T1State T1;
GARNET_INTERNAL _Atomic uint64_t rdna4_t1_frames, rdna4_t1_lastframe, rdna4_pipes_new;

GARNET_INTERNAL void rdna4_quarantine(id resources, VkResult fault) {
    rdna4_qlock();
    atomic_store(&rdna4_submit_fault, (int)fault);
    if (!rdna4_unresolved_commands) rdna4_unresolved_commands = [NSMutableArray array];
    [rdna4_unresolved_commands addObject:resources];
    pthread_mutex_unlock(&RDNA4R.qlock);
    RDNA4LOG("GPU work unresolved (%d): retaining resources until process exit; further submissions disabled", fault);
}

static struct { char path[300]; uint8_t key[32], uuid[16]; uint32_t vendor, device; BOOL save; uint64_t lastHash, savedPipes; pthread_mutex_t mu; } RDNA4PC = { .mu = PTHREAD_MUTEX_INITIALIZER };

static BOOL rdna4_pc_derive_key(void) {
    NSBundle *b = [NSBundle bundleForClass:[GarnetDevice class]];
    NSString *lib = [[b resourcePath] stringByAppendingPathComponent:@"libvulkan_radeon.dylib"];
    NSData *backend = [NSData dataWithContentsOfFile:lib options:NSDataReadingMappedIfSafe error:NULL];
    if (!backend.length || backend.length > UINT32_MAX) return NO;
    // Vulkan keys pipeline inputs itself; a Metal bundle rebuild does not change cached RADV machine code.
    static const char domain[] = "rdna4-vulkan-pipeline-cache-v2";
    uint8_t material[sizeof domain - 1 + CC_SHA256_DIGEST_LENGTH];
    memcpy(material, domain, sizeof domain - 1);
    CC_SHA256(backend.bytes, (CC_LONG)backend.length, material + sizeof domain - 1);
    CC_SHA256(material, sizeof material, RDNA4PC.key);
    return YES;
}
// Called from rdna4_radv_open_once after the device exists (class lock held). Never fails the open: no cache = pipelines compile as before.
static void rdna4_pc_open(const VkPhysicalDeviceProperties *pp) {
    if (!vkCreatePipelineCache || !vkGetPipelineCacheData) { RDNA4LOG("T2 pipeline cache: entry points not exposed by RADV, running without"); return; }
    if (access("/private/tmp/garnet-nopcache", F_OK) == 0 || (rdna4_allow() && garnet_getenv("GARNET_NOPCACHE", "RDNA4M_NOPCACHE"))) { RDNA4LOG("T2 pipeline cache: disabled (kill file /private/tmp/garnet-nopcache or GARNET_NOPCACHE)"); return; }
    memcpy(RDNA4PC.uuid, pp->pipelineCacheUUID, 16); RDNA4PC.vendor = pp->vendorID; RDNA4PC.device = pp->deviceID;
    if (!rdna4_pc_derive_key()) { RDNA4LOG("T2 pipeline cache: backend identity unavailable, running without"); return; }
    // GPU-apps G4: WindowServer and GARNET_ALLOW root tools use /private/var/tmp/garnet-pipecache.bin; any other (admitted application) process uses a file of its OWN under the
    // user's cache directory (confstr _CS_DARWIN_USER_CACHE_DIR, the one cache directory a sandboxed GPU process may write) and has NO cache if that directory is unavailable (garnet_gate.h).
    const int pcls = rdna4g_class(rdna4_is_ws(), rdna4_allow());
    char ucd[1024]; ucd[0] = 0;
    if (pcls == RDNA4G_CLASS_OTHER) { const size_t cn = confstr(_CS_DARWIN_USER_CACHE_DIR, ucd, sizeof ucd); if (cn == 0 || cn > sizeof ucd) ucd[0] = 0; }
    const char *envdirVal = rdna4_allow() ? garnet_getenv("GARNET_PCACHE_DIR", "RDNA4M_PCACHE_DIR") : NULL;
    const char *envdir = envdirVal ? envdirVal : NULL;
    const int vtw = (pcls != RDNA4G_CLASS_OTHER) ? (access("/private/var/tmp", W_OK) == 0) : 0;
    if (!rdna4g_pcache_path(RDNA4PC.path, sizeof RDNA4PC.path, pcls, envdir, vtw, ucd, getprogname())) { RDNA4LOG("T2 pipeline cache: no cache directory for this process (user cache dir unavailable), running without"); return; }
    RDNA4PC.save = rdna4g_pcache_save(pcls, (rdna4_allow() && garnet_getenv("GARNET_PCACHE_SAVE", "RDNA4M_PCACHE_SAVE")) ? 1 : 0) ? YES : NO;   // WindowServer and an application write (its own file); a root tool only with GARNET_PCACHE_SAVE: its file would block WindowServer's rename in the sticky dir
    char marker[sizeof RDNA4PC.path + 16]; snprintf(marker, sizeof marker, "%s.loading", RDNA4PC.path);
    if (access(marker, F_OK) == 0) { unlink(RDNA4PC.path); unlink(marker); RDNA4LOG("T2 pipeline cache: a previous load never finished (%s present): cache file discarded", marker); }
    void *buf = NULL; size_t flen = 0; const void *payload = NULL; size_t plen = 0; int c = -1;
    int fd = open(RDNA4PC.path, O_RDONLY);
    if (fd >= 0) {
        struct stat st;
        if (fstat(fd, &st) == 0 && st.st_size > 0 && (uint64_t)st.st_size <= RDNA4PC_MAX_PAYLOAD + sizeof(rdna4pc_hdr) && (buf = malloc((size_t)st.st_size))) {
            size_t got = 0; while (got < (size_t)st.st_size) { ssize_t n = read(fd, (char *)buf + got, (size_t)st.st_size - got); if (n <= 0) break; got += (size_t)n; }
            flen = got;
        }
        close(fd);
        if (buf) { c = rdna4pc_check(buf, flen, RDNA4PC.key, RDNA4PC.uuid, RDNA4PC.vendor, RDNA4PC.device, &payload, &plen);
                   if (c) { RDNA4LOG("T2 pipeline cache: %s ignored: %s", RDNA4PC.path, rdna4pc_why(c)); payload = NULL; plen = 0; } }
        else RDNA4LOG("T2 pipeline cache: %s unreadable/empty/oversized, ignored", RDNA4PC.path);
    } else RDNA4LOG("T2 pipeline cache: no file %s yet (cold start; saving %s)", RDNA4PC.path, RDNA4PC.save ? "ON" : "OFF in this process");
    BOOL seeded = NO;
    if (!payload && pcls == RDNA4G_CLASS_OTHER) {
        // Cold/invalid per-app cache: seed read-only from WindowServer's warmed cache so first launch skips compiling giant QuartzCore shaders.
        static const char *const kSeedPath = "/private/var/tmp/garnet-pipecache.bin";
        int sfd = open(kSeedPath, O_RDONLY);
        if (sfd >= 0) {
            struct stat sst; void *sbuf = NULL; size_t sflen = 0;
            if (fstat(sfd, &sst) == 0 && sst.st_size > 0 && (uint64_t)sst.st_size <= RDNA4PC_MAX_PAYLOAD + sizeof(rdna4pc_hdr) && (sbuf = malloc((size_t)sst.st_size))) {
                size_t got = 0; while (got < (size_t)sst.st_size) { ssize_t n = read(sfd, (char *)sbuf + got, (size_t)sst.st_size - got); if (n <= 0) break; got += (size_t)n; }
                sflen = got;
            }
            close(sfd);
            if (sbuf && rdna4pc_check(sbuf, sflen, RDNA4PC.key, RDNA4PC.uuid, RDNA4PC.vendor, RDNA4PC.device, &payload, &plen) == 0) {
                free(buf); buf = sbuf; seeded = YES;
                RDNA4LOG("T2 pipeline cache: seeded read-only from %s (%zu B)", kSeedPath, plen);
            } else free(sbuf);
        }
    }
    VkPipelineCacheCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, .initialDataSize = plen, .pInitialData = payload };
    int mk = -1; if (payload && !seeded) mk = open(marker, O_WRONLY | O_CREAT, 0644);
    uint64_t t0 = rdna4_now();
    VkResult r = vkCreatePipelineCache(RDNA4R.dev, &ci, NULL, &RDNA4R.pc);
    uint64_t dt = rdna4_now() - t0;
    if (mk >= 0) { close(mk); unlink(marker); }
    if (r != VK_SUCCESS && payload) {   // RADV refused the data: start empty
        RDNA4LOG("T2 pipeline cache: vkCreatePipelineCache(initialData %zu B) = %d, retrying empty", plen, r);
        ci.initialDataSize = 0; ci.pInitialData = NULL; payload = NULL; seeded = NO; r = vkCreatePipelineCache(RDNA4R.dev, &ci, NULL, &RDNA4R.pc);
    }
    if (r != VK_SUCCESS) { RDNA4R.pc = VK_NULL_HANDLE; RDNA4LOG("T2 pipeline cache: vkCreatePipelineCache = %d, running without", r); free(buf); return; }
    if (payload && !seeded) RDNA4PC.lastHash = rdna4pc_fnv(payload, plen);
    RDNA4LOG("T2 pipeline cache: %p created from %s in %.2f ms (initialData %zu B%s); key %02x%02x%02x%02x uuid %02x%02x%02x%02x", (void *)RDNA4R.pc, RDNA4PC.path, (double)dt / 1e6, plen, payload ? ", ACCEPTED" : ", none",
           RDNA4PC.key[0], RDNA4PC.key[1], RDNA4PC.key[2], RDNA4PC.key[3], RDNA4PC.uuid[0], RDNA4PC.uuid[1], RDNA4PC.uuid[2], RDNA4PC.uuid[3]);
    free(buf);
}
GARNET_INTERNAL void rdna4_pc_save(const char *why) {
    if (!RDNA4R.ok || !RDNA4R.pc || !RDNA4PC.save || !RDNA4PC.path[0]) return;
    uint64_t np = atomic_load(&rdna4_pipes_new);
    pthread_mutex_lock(&RDNA4PC.mu);
    if (np == RDNA4PC.savedPipes) { pthread_mutex_unlock(&RDNA4PC.mu); return; }
    RDNA4PC.savedPipes = np;   // retried only when another pipeline appears (no log spam on a persistent write failure)
    uint64_t t0 = rdna4_now(); size_t sz = 0; uint8_t *buf = NULL;
    if (vkGetPipelineCacheData(RDNA4R.dev, RDNA4R.pc, &sz, NULL) == VK_SUCCESS && sz >= 32 && sz <= RDNA4PC_MAX_PAYLOAD && (buf = malloc(sizeof(rdna4pc_hdr) + sz))) {
        VkResult r = vkGetPipelineCacheData(RDNA4R.dev, RDNA4R.pc, &sz, buf + sizeof(rdna4pc_hdr));
        if (r == VK_SUCCESS && rdna4pc_vk_header_ok(buf + sizeof(rdna4pc_hdr), sz, RDNA4PC.uuid, RDNA4PC.vendor, RDNA4PC.device)) {
            uint64_t h = rdna4pc_fnv(buf + sizeof(rdna4pc_hdr), sz);
            if (h == RDNA4PC.lastHash) { free(buf); pthread_mutex_unlock(&RDNA4PC.mu); return; }   // nothing new in the cache
            rdna4pc_hdr hd; rdna4pc_fill(&hd, RDNA4PC.key, RDNA4PC.uuid, RDNA4PC.vendor, RDNA4PC.device, buf + sizeof hd, sz); memcpy(buf, &hd, sizeof hd);
            char tmp[sizeof RDNA4PC.path + 24]; snprintf(tmp, sizeof tmp, "%s.tmp.%d", RDNA4PC.path, (int)getpid());
            int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644); BOOL ok = NO; int en = 0;
            if (fd >= 0) {
                size_t put = 0, tot = sizeof hd + sz; while (put < tot) { ssize_t n = write(fd, buf + put, tot - put); if (n <= 0) break; put += (size_t)n; }
                ok = put == tot && fsync(fd) == 0; en = errno; close(fd);
                if (ok && rename(tmp, RDNA4PC.path) != 0) { ok = NO; en = errno; }
                if (!ok) unlink(tmp);
            } else en = errno;
            if (ok) { RDNA4PC.lastHash = h; RDNA4LOG("T2 pipeline cache: SAVED %zu B to %s (%s; %llu pipelines created so far) in %.2f ms", sizeof hd + sz, RDNA4PC.path, why, (unsigned long long)np, (double)(rdna4_now() - t0) / 1e6); }
            else RDNA4LOG("T2 pipeline cache: save to %s FAILED (errno %d)", RDNA4PC.path, en);
        } else RDNA4LOG("T2 pipeline cache: vkGetPipelineCacheData = %d / header check failed, not saved", r);
    }
    free(buf);
    pthread_mutex_unlock(&RDNA4PC.mu);
}
static void rdna4_pc_atexit(void) { rdna4_pc_save("exit"); }











static void rdna4_t1_report(void) {
    static unsigned tick;
    struct { const char *n; rdna4t_hist *h; rdna4t_snap s; } m[] = { { "spvcache_read", &T1.spv, {0} }, { "shader_module", &T1.shm, {0} }, { "gfx_pipeline", &T1.gfx, {0} }, { "comp_pipeline", &T1.cmp, {0} }, { "import", &T1.imp, {0} },
        { "cb_encode", &T1.enc, {0} }, { "cb_gpu(submit->done)", &T1.gpu, {0} }, { "completion_wake", &T1.wake, {0} }, { "qlock_wait", &T1.qlk, {0} }, { "frame_gap", &T1.fgap, {0} }, { "frame_gpu", &T1.fgpu, {0} } };
    unsigned any = 0; char line[640];
    for (size_t i = 0; i < sizeof m / sizeof m[0]; i++) {
        rdna4t_take(m[i].h, &m[i].s);
        if (rdna4t_fmt(m[i].n, &m[i].s, line, sizeof line)) { any++; RDNA4LOG("%s [%s pid %d, 10 s window]", line, getprogname(), (int)getpid()); }
    }
    uint64_t fr = atomic_exchange(&rdna4_t1_frames, 0);
    if (any || fr) RDNA4LOG("T1 counts/10s [%s pid %d]: pipelines gfx=%llu comp=%llu imports=%llu command_buffers=%llu frames=%llu (frame = command buffer that wrote a display surface)", getprogname(), (int)getpid(),
        (unsigned long long)m[2].s.n, (unsigned long long)m[3].s.n, (unsigned long long)m[4].s.n, (unsigned long long)m[5].s.n, (unsigned long long)fr);
    else if (tick % 6 == 5) RDNA4LOG("T1 idle: no pipeline/import/command buffer in the last 60 s [%s pid %d]", getprogname(), (int)getpid());
    rdna4_pool_report(tick);   // P1: one line when the pool is ON and something happened (nothing when OFF)
    rdna4_drawopt_report(tick);   // P5b: one line when the drawopt switch is ON and something happened (nothing when OFF)
    rdna4_m6_report();   // bundle 11: one line when the kernel latch garnet-m6 is ON (nothing when OFF)
    rdna4_pool_tick();
    rdna4_imp_report(tick); rdna4_imp_tick();   // P4
    rdna4_vs_report(tick); rdna4_vs_tick();     // Phase B: WindowServer OPTIMAL VRAM surface cache
    rdna4_led_report(tick);   // build 16 (P5 Step 0): the live-import ledger, one line per tick while any surface texture is alive
    if (++tick % 3 == 0) rdna4_pc_save("periodic");
}
static void rdna4_t1_start(void) {
    static dispatch_source_t src;
    if (src) return;
    src = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
    dispatch_source_set_timer(src, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC), 10 * NSEC_PER_SEC, 500 * NSEC_PER_MSEC);
    dispatch_source_set_event_handler(src, ^{ rdna4_t1_report(); });
    dispatch_resume(src);
    atexit(rdna4_pc_atexit);
    RDNA4LOG("T1 timing: summaries every 10 s (pid %d %s)", (int)getpid(), getprogname());
}
// GARNET_TEST_OPEN_REFUSE=<n> (needs GARNET_ALLOW): the first n open attempts fail like a refused RDNA4N open, without touching Vulkan.
static BOOL rdna4_test_refuse(void) {
    static long left = -1;
    if (left < 0) { const char *e = garnet_getenv("GARNET_TEST_OPEN_REFUSE", "RDNA4M_TEST_OPEN_REFUSE"); left = (rdna4_allow() && e) ? atol(e) : 0; }
    if (left > 0) { left--; return YES; }
    return NO;
}
static int rdna4_csv_has_token(const char *csv, const char *tok) {
    if (!csv || !*csv || !tok || !*tok) return 0;
    const size_t tlen = strlen(tok);
    const char *p = csv;
    while (*p) {
        while (*p == ',') p++;
        const char *e = strchr(p, ',');
        const size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len == tlen && strncmp(p, tok, tlen) == 0) return 1;
        if (!e) break;
        p = e + 1;
    }
    return 0;
}

// One open attempt (#12 R4: the retry policy is in rdna4_radv_open). Returns YES when the RADV device + queue are up; on failure *err
// is set. Called with the class lock held.
static BOOL rdna4_radv_open_once(NSError **err) {
    rdna4_argument_limit_snapshot seen;
    memset(&seen, 0, sizeof(seen));
    rdna4_argument_limit_result seenResult =
        RDNA4_ARGUMENT_LIMIT_RESULT_CROSSCHECK_UNAVAILABLE;
    int sourceMatches = 0;
    {
        NSString *dir = [[NSBundle bundleForClass:[GarnetDevice class]] resourcePath];
        NSString *path = [dir stringByAppendingPathComponent:@"libvulkan_radeon.dylib"];
        RDNA4LOG("radv open: dlopen %s", path.UTF8String);
        void *h = dlopen(path.UTF8String, RTLD_NOW | RTLD_LOCAL);
        if (!h) { RDNA4R.err = rdna4_err(1, [NSString stringWithFormat:@"dlopen %@: %s", path, dlerror()]); goto fail; }
        RDNA4R.lib = h;
        if (RDNA4ALS.state == RDNA4_ARGUMENT_LIMIT_STATE_READY) {
            sourceMatches = rdna4_argument_limit_snapshot_source_matches_path(
                &RDNA4ALS, path.fileSystemRepresentation) ==
                RDNA4_ARGUMENT_LIMIT_RESULT_OK;
            if (sourceMatches) {
                seenResult = rdna4_argument_limit_snapshot_init_from_handle(
                    &seen, h, path.fileSystemRepresentation,
                    RDNA4ALS.facts_present ? &RDNA4ALS.input_facts : NULL,
                    &RDNA4ALS.policy);
            }
        }
        rdna4_gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vk_icdGetInstanceProcAddr");
        if (!rdna4_gipa) { RDNA4R.err = rdna4_err(2, @"no vk_icdGetInstanceProcAddr in RADV"); goto fail; }
        {   // G6: an admitted APPLICATION opens its native connection on the accelerator (a sandboxed app may open user clients only on an IOAccelerator service); WindowServer and root tools keep Mesa's default and no
            // setter is called. Before vkCreateInstance: the connection is opened when the physical devices are enumerated. Fail closed: a RADV without the setter cannot be used for an application.
            const char *svcCls = rdna4g_mesa_service_for_class(rdna4g_class(rdna4_is_ws(), rdna4_allow()));
            if (svcCls) {
                int (*setSvc)(const char *) = (int (*)(const char *))dlsym(h, RDNA4G_MESA_SETTER);
                if (!setSvc) { RDNA4R.err = rdna4_err(7, @"no " RDNA4G_MESA_SETTER " in RADV (an application needs the accelerator route)"); goto fail; }
                const int src = setSvc(svcCls);
                if (src != 0) { RDNA4R.err = rdna4_err(7, [NSString stringWithFormat:@RDNA4G_MESA_SETTER "(%s) = %d", svcCls, src]); goto fail; }
                RDNA4LOG("radv open: application class: RDNA4N is opened on %s", svcCls);
            }
        }
        rdna4_vkCreateInstance = (PFN_vkCreateInstance)rdna4_gipa(NULL, "vkCreateInstance");
        if (!rdna4_vkCreateInstance) { RDNA4R.err = rdna4_err(3, @"no vkCreateInstance"); goto fail; }
        VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "garnetmetal", .apiVersion = VK_API_VERSION_1_3 };
        VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai };
        {   // Phase A4: auto-adapt nosam and Phase B VRAM cache budget to Small-BAR (256 MiB) vs Full-BAR (>= 4 GiB).
            // Probe GarnetDriver's IORegistry properties via port 0 (kIOMasterPortDefault / kIOMainPortDefault); fail-closed to Small-BAR.
            int bar0FullMode = 0;
            uint64_t bar0MB = 0;
            io_service_t drv = IOServiceGetMatchingService(0, IOServiceMatching("GarnetDriver"));
            if (drv) {
                CFTypeRef fm = IORegistryEntryCreateCFProperty(drv, CFSTR("BAR0,FullMode"), kCFAllocatorDefault, 0);
                if (fm) {
                    if (CFGetTypeID(fm) == CFBooleanGetTypeID()) bar0FullMode = CFBooleanGetValue((CFBooleanRef)fm) ? 1 : 0;
                    else if (CFGetTypeID(fm) == CFNumberGetTypeID()) { int v = 0; if (CFNumberGetValue((CFNumberRef)fm, kCFNumberIntType, &v)) bar0FullMode = v ? 1 : 0; }
                    CFRelease(fm);
                }
                CFTypeRef sz = IORegistryEntryCreateCFProperty(drv, CFSTR("BAR0,SizeMB"), kCFAllocatorDefault, 0);
                if (!sz) sz = IORegistryEntryCreateCFProperty(drv, CFSTR("ReBAR,BAR0,CurrentMB"), kCFAllocatorDefault, 0);
                if (sz) {
                    (void)rdna4_number_u64(sz, &bar0MB);
                    CFRelease(sz);
                }
                IOObjectRelease(drv);
            }
            struct stat sb_;
            const int forceNosam = (stat("/private/tmp/garnet-nosam", &sb_) == 0) ? 1 : 0;
            const int forceNosamOff = (stat("/private/tmp/garnet-nosam-off", &sb_) == 0) ? 1 : 0;
            RDNA4R.barPolicy = garnet_bar_choose_policy(bar0FullMode, bar0MB, forceNosam, forceNosamOff);
            if (RDNA4R.barPolicy.enable_nosam) {
                const char *pt = getenv("RADV_PERFTEST");
                if (!rdna4_csv_has_token(pt, "nosam")) {
                    char v[256];
                    snprintf(v, sizeof v, "%s%snosam", pt && *pt ? pt : "", pt && *pt ? "," : "");
                    setenv("RADV_PERFTEST", v, 1);
                }
            }
            RDNA4LOG("radv open: BAR policy: fullMode=%d bar0=%llu MiB -> %s, nosam=%s (RADV_PERFTEST=%s), vram_surf_cap=%llu MiB",
                     bar0FullMode, (unsigned long long)bar0MB, RDNA4R.barPolicy.full_bar ? "FULL-BAR" : "SMALL-BAR",
                     RDNA4R.barPolicy.enable_nosam ? "ON" : "OFF", getenv("RADV_PERFTEST") ?: "-",
                     (unsigned long long)(RDNA4R.barPolicy.vram_surf_cap_bytes >> 20));
        }
        VkResult r = rdna4_vkCreateInstance(&ici, NULL, &RDNA4R.inst);
        if (r != VK_SUCCESS) { RDNA4R.err = rdna4_err(4, [NSString stringWithFormat:@"vkCreateInstance = %d", r]); goto fail; }
#define X(n) n = (PFN_##n)rdna4_gipa(RDNA4R.inst, #n); if (!n) RDNA4LOG("radv: %s not exposed", #n);
        RDNA4_VK_FUNCS(X)
#undef X
        RDNA4R.realCreateFence = vkCreateFence;
        RDNA4R.realDestroyFence = vkDestroyFence;
        RDNA4R.realResetFences = vkResetFences;
        RDNA4R.realAllocCmd = vkAllocateCommandBuffers;
        RDNA4R.realFreeCmd = vkFreeCommandBuffers;
        RDNA4R.realResetCmd = vkResetCommandBuffer;
        RDNA4R.realFreeMem = vkFreeMemory;
        RDNA4R.realCreateFB = vkCreateFramebuffer;
        RDNA4R.realDestroyFB = vkDestroyFramebuffer;
        RDNA4R.realAllocDesc = vkAllocateDescriptorSets;
        rdna4_vkQueueWaitIdle = (PFN_vkQueueWaitIdle)rdna4_gipa(RDNA4R.inst, "vkQueueWaitIdle");
        rdna4_native_bridge.copy = (void *)dlsym(h, "radv_darwin_buffer_copy");
        rdna4_native_bridge.fill = (void *)dlsym(h, "radv_darwin_buffer_fill");
        rdna4_native_bridge.wait = (void *)dlsym(h, "radv_darwin_buffer_wait");
        uint32_t cnt = 1;
        r = vkEnumeratePhysicalDevices(RDNA4R.inst, &cnt, &RDNA4R.pd);
        if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || cnt == 0) {
            RDNA4R.err = rdna4_err(5, [NSString stringWithFormat:@"vkEnumeratePhysicalDevices = %d count %u (RDNA4N refused? root-only, exclusive)", r, cnt]);
            if (vkDestroyInstance && RDNA4R.inst) { vkDestroyInstance(RDNA4R.inst, NULL); RDNA4R.inst = VK_NULL_HANDLE; }   // a retry starts from a fresh instance
            goto fail;
        }
        VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(RDNA4R.pd, &pp);
        RDNA4LOG("radv: physical device %s", pp.deviceName);
        uint64_t maxAllocation = 0, maxBuffer = 0; uint32_t subgroupSize = 0;
        rdna4x_backend_capabilities queryCaps = rdna4x_query_caps(0, 0, 0, 0, 0);
        if (@available(macOS 11.0, *)) queryCaps.flags |= RDNA4X_CAP_QUERY_LANGUAGE;
        if (vkGetPhysicalDeviceProperties2 && pp.apiVersion >= VK_API_VERSION_1_1) {
            VkPhysicalDeviceSubgroupProperties sg = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES };
            VkPhysicalDeviceMaintenance4Properties m4 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_PROPERTIES };
            VkPhysicalDeviceVulkan12Properties indexing = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES };
            if (pp.apiVersion >= VK_API_VERSION_1_2) sg.pNext = &indexing;
            if (pp.apiVersion >= VK_API_VERSION_1_3) indexing.pNext = &m4;
            VkPhysicalDeviceMaintenance3Properties m3 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES, .pNext = &sg };
            VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &m3 };
            vkGetPhysicalDeviceProperties2(RDNA4R.pd, &p2);
            maxAllocation = m3.maxMemoryAllocationSize;
            RDNA4R.maxPerSetDescriptors = m3.maxPerSetDescriptors;
            maxBuffer = m4.maxBufferSize;
            RDNA4R.descriptorLimits = indexing; RDNA4R.descriptorLimits.pNext = NULL;
            if (sg.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) subgroupSize = sg.subgroupSize;
            queryCaps.subgroup_size = sg.subgroupSize;
            queryCaps.subgroup_supported_stages = sg.supportedStages;
            queryCaps.subgroup_supported_operations = sg.supportedOperations;
        }
        uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(RDNA4R.pd, &nq, NULL);
        VkQueueFamilyProperties qf[16]; if (nq > 16) nq = 16;
        vkGetPhysicalDeviceQueueFamilyProperties(RDNA4R.pd, &nq, qf);
        uint32_t qfi = 0; for (; qfi < nq; qfi++) if (qf[qfi].queueFlags & VK_QUEUE_GRAPHICS_BIT) break;
        if (qfi == nq) { RDNA4R.err = rdna4_err(6, @"no graphics queue family"); goto fail; }
        uint32_t cqfi = nq;
        for (uint32_t i = 0; i < nq; i++) {
            if ((qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && !(qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) { cqfi = i; break; }
        }
        const int hasDedicatedComputeQF = (cqfi < nq) ? 1 : 0;
        RDNA4R.qfi = qfi;
        float prio = 1.0f;
        VkDeviceQueueCreateInfo dq = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qfi, .queueCount = 1, .pQueuePriorities = &prio };
        VkPhysicalDeviceFeatures pf; vkGetPhysicalDeviceFeatures(RDNA4R.pd, &pf);
        const rdna4_gather_caps gatherCaps = rdna4_gather_caps_from_vulkan(
            pf.shaderImageGatherExtended != VK_FALSE,
            pp.limits.minTexelGatherOffset, pp.limits.maxTexelGatherOffset);
        RDNA4LOG("radv: shaderImageGatherExtended %s, texel gather offsets [%d, %u]",
                 gatherCaps.supported ? "supported" : "unsupported",
                 gatherCaps.min_texel_gather_offset, gatherCaps.max_texel_gather_offset);
        // Translated system AIR uses 8/16-bit arithmetic and fragment demotion.
        // Support alone is insufficient: enable the queried features on our device.
        VkPhysicalDeviceVulkan13Features supported13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
        VkPhysicalDeviceVulkan12Features supported12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &supported13 };
        VkPhysicalDeviceVulkan11Features supported11 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &supported12 };
        VkPhysicalDeviceFeatures2 supported = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &supported11 };
        vkGetPhysicalDeviceFeatures2(RDNA4R.pd, &supported);
        VkPhysicalDeviceVulkan13Features enabled13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
            .shaderDemoteToHelperInvocation = supported13.shaderDemoteToHelperInvocation };
        VkPhysicalDeviceVulkan12Features enabled12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &enabled13,
            .samplerMirrorClampToEdge = supported12.samplerMirrorClampToEdge,
            .shaderBufferInt64Atomics = pf.shaderInt64 && supported12.shaderBufferInt64Atomics,
            .shaderSharedInt64Atomics = pf.shaderInt64 && supported12.shaderSharedInt64Atomics,
            .shaderFloat16 = supported12.shaderFloat16, .shaderInt8 = supported12.shaderInt8,
            .storageBuffer8BitAccess = supported12.storageBuffer8BitAccess,
            .uniformAndStorageBuffer8BitAccess = supported12.uniformAndStorageBuffer8BitAccess,
            .descriptorIndexing = supported12.descriptorIndexing,
            .runtimeDescriptorArray = supported12.runtimeDescriptorArray,
            .descriptorBindingVariableDescriptorCount = supported12.descriptorBindingVariableDescriptorCount,
            .descriptorBindingPartiallyBound = supported12.descriptorBindingPartiallyBound,
            .descriptorBindingSampledImageUpdateAfterBind = supported12.descriptorBindingSampledImageUpdateAfterBind,
            .descriptorBindingStorageImageUpdateAfterBind = supported12.descriptorBindingStorageImageUpdateAfterBind,
            .descriptorBindingUniformTexelBufferUpdateAfterBind = supported12.descriptorBindingUniformTexelBufferUpdateAfterBind,
            .descriptorBindingStorageTexelBufferUpdateAfterBind = supported12.descriptorBindingStorageTexelBufferUpdateAfterBind,
            .shaderInputAttachmentArrayDynamicIndexing = supported12.shaderInputAttachmentArrayDynamicIndexing,
            .shaderUniformTexelBufferArrayDynamicIndexing = supported12.shaderUniformTexelBufferArrayDynamicIndexing,
            .shaderStorageTexelBufferArrayDynamicIndexing = supported12.shaderStorageTexelBufferArrayDynamicIndexing,
            .shaderUniformBufferArrayNonUniformIndexing = supported12.shaderUniformBufferArrayNonUniformIndexing,
            .shaderSampledImageArrayNonUniformIndexing = supported12.shaderSampledImageArrayNonUniformIndexing,
            .shaderStorageBufferArrayNonUniformIndexing = supported12.shaderStorageBufferArrayNonUniformIndexing,
            .shaderStorageImageArrayNonUniformIndexing = supported12.shaderStorageImageArrayNonUniformIndexing,
            .shaderInputAttachmentArrayNonUniformIndexing = supported12.shaderInputAttachmentArrayNonUniformIndexing,
            .shaderUniformTexelBufferArrayNonUniformIndexing = supported12.shaderUniformTexelBufferArrayNonUniformIndexing,
            .shaderStorageTexelBufferArrayNonUniformIndexing = supported12.shaderStorageTexelBufferArrayNonUniformIndexing,
            .timelineSemaphore = supported12.timelineSemaphore,
            .bufferDeviceAddress = supported12.bufferDeviceAddress };
        VkPhysicalDeviceVulkan11Features enabled11 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &enabled12,
            .shaderDrawParameters = supported11.shaderDrawParameters,
            .storageBuffer16BitAccess = supported11.storageBuffer16BitAccess,
            .uniformAndStorageBuffer16BitAccess = supported11.uniformAndStorageBuffer16BitAccess,
            .storageInputOutput16 = supported11.storageInputOutput16 };
        // translated AIR uses Int64 (10a); 11e: storage images are translated with format Unknown + Read/WriteWithoutFormat (add-air.py)
        VkPhysicalDeviceFeatures ef = { .tessellationShader = pf.tessellationShader,
                                        .shaderInt64 = pf.shaderInt64, .shaderInt16 = pf.shaderInt16, .fragmentStoresAndAtomics = pf.fragmentStoresAndAtomics, .shaderStorageImageReadWithoutFormat = pf.shaderStorageImageReadWithoutFormat,
                                        .shaderStorageImageWriteWithoutFormat = pf.shaderStorageImageWriteWithoutFormat,
                                        .independentBlend = pf.independentBlend,
                                        .dualSrcBlend = pf.dualSrcBlend,
                                        .depthClamp = pf.depthClamp,
                                        .fillModeNonSolid = pf.fillModeNonSolid,
                                        .multiViewport = pf.multiViewport,
                                        .textureCompressionBC = pf.textureCompressionBC,
                                        .sampleRateShading = pf.sampleRateShading,
                                        .shaderStorageImageExtendedFormats = pf.shaderStorageImageExtendedFormats,
                                        .shaderStorageImageMultisample = pf.shaderStorageImageMultisample,
                                        .vertexPipelineStoresAndAtomics = pf.vertexPipelineStoresAndAtomics,
                                        .shaderClipDistance = pf.shaderClipDistance,
                                        .shaderCullDistance = pf.shaderCullDistance,
                                        .shaderSampledImageArrayDynamicIndexing = pf.shaderSampledImageArrayDynamicIndexing,
                                        .shaderStorageImageArrayDynamicIndexing = pf.shaderStorageImageArrayDynamicIndexing,
                                        .shaderUniformBufferArrayDynamicIndexing = pf.shaderUniformBufferArrayDynamicIndexing,
                                        .shaderStorageBufferArrayDynamicIndexing = pf.shaderStorageBufferArrayDynamicIndexing,
                                        .depthBiasClamp = pf.depthBiasClamp,   // bundle 10: setDepthBias:slopeScale:clamp:
                                        .multiDrawIndirect = pf.multiDrawIndirect,
                                        .drawIndirectFirstInstance = pf.drawIndirectFirstInstance,
                                        .imageCubeArray = pf.imageCubeArray,
                                        .samplerAnisotropy = pf.samplerAnisotropy,
                                        .shaderImageGatherExtended = gatherCaps.supported ? VK_TRUE : VK_FALSE,
                                        .occlusionQueryPrecise = pf.occlusionQueryPrecise };   // build 18 (P2): Counting visibility mode
        RDNA4R.multiDrawIndirect = ef.multiDrawIndirect != VK_FALSE;
        RDNA4R.dualSrcBlend = ef.dualSrcBlend;
        RDNA4R.depthClamp = ef.depthClamp;
        RDNA4R.fillModeNonSolid = ef.fillModeNonSolid;
        RDNA4R.multiViewport = ef.multiViewport;
        RDNA4R.bcCompression = ef.textureCompressionBC;
        RDNA4R.sampleRateShading = ef.sampleRateShading;
        RDNA4R.storageImageExtFormats = ef.shaderStorageImageExtendedFormats;
        RDNA4R.storageImageMultisample = ef.shaderStorageImageMultisample;
        RDNA4R.maxViewports = ef.multiViewport ? MIN(pp.limits.maxViewports, 16u) : 1u;
        RDNA4R.maxTessGenLevel = pp.limits.maxTessellationGenerationLevel;
        RDNA4R.maxTessPatchSize = pp.limits.maxTessellationPatchSize;
        RDNA4R.tessellation = ef.tessellationShader && RDNA4R.maxTessGenLevel >= 64 && RDNA4R.maxTessPatchSize >= 32;
        RDNA4LOG("radv: tessellation %s (feature %u, maxGen %u, maxPatch %u, shaderDrawParameters %u)",
                 RDNA4R.tessellation ? "enabled" : "unavailable", ef.tessellationShader,
                 RDNA4R.maxTessGenLevel, RDNA4R.maxTessPatchSize, enabled11.shaderDrawParameters);
        RDNA4R.uniformTexelBuffers = enabled12.runtimeDescriptorArray &&
            enabled12.descriptorBindingPartiallyBound &&
            enabled12.descriptorBindingUniformTexelBufferUpdateAfterBind &&
            enabled12.shaderUniformTexelBufferArrayDynamicIndexing &&
            enabled12.shaderUniformTexelBufferArrayNonUniformIndexing &&
            vkCreateBufferView && vkDestroyBufferView;
        RDNA4R.storageTexelBuffers = enabled12.runtimeDescriptorArray &&
            enabled12.descriptorBindingPartiallyBound &&
            enabled12.descriptorBindingStorageTexelBufferUpdateAfterBind &&
            enabled12.shaderStorageTexelBufferArrayDynamicIndexing &&
            enabled12.shaderStorageTexelBufferArrayNonUniformIndexing &&
            vkCreateBufferView && vkDestroyBufferView;
        RDNA4R.descriptorIndexing = enabled12.descriptorIndexing;
        RDNA4R.nonUniformSampledImages = enabled12.shaderSampledImageArrayNonUniformIndexing;
        RDNA4R.nonUniformStorageImages = enabled12.shaderStorageImageArrayNonUniformIndexing;
        RDNA4R.nonUniformBuffers = enabled12.shaderUniformBufferArrayNonUniformIndexing &&
            enabled12.shaderStorageBufferArrayNonUniformIndexing;
        RDNA4R.gpuArgumentTables = enabled12.runtimeDescriptorArray &&
            enabled12.descriptorBindingVariableDescriptorCount && enabled12.descriptorBindingPartiallyBound &&
            enabled12.descriptorBindingSampledImageUpdateAfterBind && enabled12.descriptorBindingStorageImageUpdateAfterBind &&
            RDNA4R.nonUniformSampledImages && RDNA4R.nonUniformStorageImages && pp.limits.maxBoundDescriptorSets >= 5;
        RDNA4LOG("radv: descriptor indexing %u, non-uniform sampled/storage images %u/%u, buffers %u, dynamic arrays sampled/storage/buffer %u/%u/%u/%u",
                 RDNA4R.descriptorIndexing, RDNA4R.nonUniformSampledImages, RDNA4R.nonUniformStorageImages,
                 RDNA4R.nonUniformBuffers, ef.shaderSampledImageArrayDynamicIndexing,
                 ef.shaderStorageImageArrayDynamicIndexing, ef.shaderUniformBufferArrayDynamicIndexing,
                 ef.shaderStorageBufferArrayDynamicIndexing);
        RDNA4LOG("radv: apiVersion %u.%u shaderInt64 supported %u, storage image read/write without format %u/%u", VK_API_VERSION_MAJOR(pp.apiVersion), VK_API_VERSION_MINOR(pp.apiVersion), pf.shaderInt64,
               pf.shaderStorageImageReadWithoutFormat, pf.shaderStorageImageWriteWithoutFormat);
        RDNA4R.lim = pp.limits; RDNA4R.dbClamp = pf.depthBiasClamp ? YES : NO;
        if (rdna4_argument_limit_verify_open(
                &pp, &RDNA4R.descriptorLimits, RDNA4R.maxPerSetDescriptors,
                maxAllocation, maxBuffer,
                &seen, seenResult, sourceMatches) < 0)
            RDNA4R.gpuArgumentTables = NO;   // Tier1 remains open; only the argument-table path fails closed.
        RDNA4R.samplerAnisotropy = ef.samplerAnisotropy;
        RDNA4R.samplerMirrorClamp = enabled12.samplerMirrorClampToEdge;
        RDNA4LOG("radv: buffer offset alignments uniform %llu storage %llu", (unsigned long long)pp.limits.minUniformBufferOffsetAlignment,
                 (unsigned long long)pp.limits.minStorageBufferOffsetAlignment);
        RDNA4R.indirectFirstInstance = ef.drawIndirectFirstInstance;
        RDNA4R.cubeArray = ef.imageCubeArray;
        RDNA4R.occPrecise = pf.occlusionQueryPrecise ? YES : NO;
        RDNA4R.occOK = (vkCreateQueryPool && vkDestroyQueryPool && vkCmdResetQueryPool && vkCmdBeginQuery && vkCmdEndQuery && vkGetQueryPoolResults && vkGetFenceStatus) ? YES : NO;   // build 18 (P2)
        {   // bundle 10: what the device offers for depth/stencil and MSAA (each depth texture / pipeline checks these again before it uses a format)
            static const VkFormat dsf[4] = { VK_FORMAT_D16_UNORM, VK_FORMAT_D32_SFLOAT, VK_FORMAT_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT };
            for (int i = 0; i < 4 && vkGetPhysicalDeviceFormatProperties; i++) { VkFormatProperties fp0 = {0}; vkGetPhysicalDeviceFormatProperties(RDNA4R.pd, dsf[i], &fp0);
                RDNA4LOG("radv: depth/stencil format %d optimal features 0x%x (sampled %d, depth/stencil attachment %d)%s", (int)dsf[i], fp0.optimalTilingFeatures, (fp0.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0,
                       (fp0.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0, (fp0.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) ? "" : ": depth textures / pipelines of this format are refused"); }
            RDNA4LOG("radv: framebuffer sample counts color 0x%x depth 0x%x stencil 0x%x; depthBiasClamp %d", pp.limits.framebufferColorSampleCounts, pp.limits.framebufferDepthSampleCounts, pp.limits.framebufferStencilSampleCounts, pf.depthBiasClamp);
        }
        VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT orderedColor = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_FEATURES_EXT };
        VkPhysicalDeviceRobustness2FeaturesEXT supportedNull = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT };
        VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT fragInterlockFeat = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT };
        VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR baryFeat = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR };
        VkPhysicalDeviceShaderAtomicFloatFeaturesEXT atomicFloatFeat = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT };
        VkPhysicalDeviceShaderAtomicFloat2FeaturesEXT atomicFloat2Feat = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_2_FEATURES_EXT };
        VkPhysicalDeviceMeshShaderFeaturesEXT meshFeat = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT };
        VkPhysicalDeviceAccelerationStructureFeaturesKHR accelFeat = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR };
        VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeat = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR };
        VkPhysicalDeviceRayTracingPositionFetchFeaturesKHR rtPosFetchFeat = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_POSITION_FETCH_FEATURES_KHR };
        VkPhysicalDeviceMeshShaderPropertiesEXT meshProps = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT };
        VkPhysicalDeviceAccelerationStructurePropertiesKHR accelProps = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR };
        VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gplFeat = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT };
        VkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3Feat = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT };
        VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT gplProps = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT };
        const char *orderedColorExt = NULL;
        BOOL hasDrmMod = NO, hasMemoryBudget = NO, hasSampleLoc = NO, hasInterlock = NO;
        BOOL hasBarycentric = NO, hasAtomicFloat = NO, hasAtomicFloat2 = NO, hasAtomicInt64 = NO;
        BOOL hasMeshShader = NO, hasDeferredHostOps = NO, hasAccelStruct = NO, hasRayQuery = NO, hasRtPosFetch = NO, hasPushDesc = NO;
        BOOL hasPipeLib = NO, hasGpl = NO, hasEds3 = NO;
        VkPhysicalDevicePushDescriptorPropertiesKHR pushDescProps = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR };
        {
            uint32_t ne = 0; vkEnumerateDeviceExtensionProperties(RDNA4R.pd, NULL, &ne, NULL);
            VkExtensionProperties *ex = calloc(ne ? ne : 1, sizeof *ex); vkEnumerateDeviceExtensionProperties(RDNA4R.pd, NULL, &ne, ex);
            NSMutableString *all = [NSMutableString string];
            for (uint32_t i = 0; i < ne; i++) [all appendFormat:@"%s ", ex[i].extensionName];
            RDNA4LOG("radv: %u device extensions (listed in chunks):", ne);
            for (NSUInteger o = 0; o < all.length; o += 600) RDNA4LOG("radv: ext[%lu]: %s", (unsigned long)o, [all substringWithRange:NSMakeRange(o, MIN((NSUInteger)600, all.length - o))].UTF8String);
            BOOL hasRO = NO, hasLR = NO, hasFL = NO, hasDR = NO, hasNull = NO;
            for (uint32_t i = 0; i < ne; i++) {
                if (!strcmp(ex[i].extensionName, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME)) RDNA4R.hostExt = YES;
                hasDrmMod |= !strcmp(ex[i].extensionName, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
                hasMemoryBudget |= !strcmp(ex[i].extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
                hasSampleLoc |= !strcmp(ex[i].extensionName, VK_EXT_SAMPLE_LOCATIONS_EXTENSION_NAME);
                hasInterlock |= !strcmp(ex[i].extensionName, VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME);
                hasNull |= !strcmp(ex[i].extensionName, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
                hasBarycentric |= !strcmp(ex[i].extensionName, VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME);
                hasAtomicFloat |= !strcmp(ex[i].extensionName, VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME);
                hasAtomicFloat2 |= !strcmp(ex[i].extensionName, VK_EXT_SHADER_ATOMIC_FLOAT_2_EXTENSION_NAME);
                hasAtomicInt64 |= !strcmp(ex[i].extensionName, VK_KHR_SHADER_ATOMIC_INT64_EXTENSION_NAME);
                hasMeshShader |= !strcmp(ex[i].extensionName, VK_EXT_MESH_SHADER_EXTENSION_NAME);
                hasDeferredHostOps |= !strcmp(ex[i].extensionName, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
                hasAccelStruct |= !strcmp(ex[i].extensionName, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
                hasRayQuery |= !strcmp(ex[i].extensionName, VK_KHR_RAY_QUERY_EXTENSION_NAME);
                hasRtPosFetch |= !strcmp(ex[i].extensionName, VK_KHR_RAY_TRACING_POSITION_FETCH_EXTENSION_NAME);
                hasPushDesc |= !strcmp(ex[i].extensionName, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
                hasPipeLib |= !strcmp(ex[i].extensionName, VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME);
                hasGpl |= !strcmp(ex[i].extensionName, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME);
                hasEds3 |= !strcmp(ex[i].extensionName, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
                hasRO |= !strcmp(ex[i].extensionName, "VK_EXT_rasterization_order_attachment_access") || !strcmp(ex[i].extensionName, "VK_ARM_rasterization_order_attachment_access");
                if (!strcmp(ex[i].extensionName, VK_EXT_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME)) orderedColorExt = VK_EXT_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME;
                else if (!orderedColorExt && !strcmp(ex[i].extensionName, VK_ARM_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME)) orderedColorExt = VK_ARM_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME;
                hasLR |= !strcmp(ex[i].extensionName, "VK_KHR_dynamic_rendering_local_read");
                hasFL |= !strcmp(ex[i].extensionName, "VK_EXT_attachment_feedback_loop_layout");
                hasDR |= !strcmp(ex[i].extensionName, "VK_KHR_dynamic_rendering");
            }
            free(ex);
            VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR flr = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR, .pNext = &orderedColor };
            void *f2Next = &flr;
            if (hasNull) { supportedNull.pNext = f2Next; f2Next = &supportedNull; }
            if (hasInterlock) { fragInterlockFeat.pNext = f2Next; f2Next = &fragInterlockFeat; }
            if (hasBarycentric) { baryFeat.pNext = f2Next; f2Next = &baryFeat; }
            if (hasAtomicFloat) { atomicFloatFeat.pNext = f2Next; f2Next = &atomicFloatFeat; }
            if (hasAtomicFloat2) { atomicFloat2Feat.pNext = f2Next; f2Next = &atomicFloat2Feat; }
            if (hasMeshShader) { meshFeat.pNext = f2Next; f2Next = &meshFeat; }
            if (hasAccelStruct) { accelFeat.pNext = f2Next; f2Next = &accelFeat; }
            if (hasRayQuery) { rayQueryFeat.pNext = f2Next; f2Next = &rayQueryFeat; }
            if (hasRtPosFetch) { rtPosFetchFeat.pNext = f2Next; f2Next = &rtPosFetchFeat; }
            if (hasGpl) { gplFeat.pNext = f2Next; f2Next = &gplFeat; }
            if (hasEds3) { eds3Feat.pNext = f2Next; f2Next = &eds3Feat; }
            VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = f2Next };
            if (vkGetPhysicalDeviceFeatures2) vkGetPhysicalDeviceFeatures2(RDNA4R.pd, &f2);
            RDNA4LOG("radv: framebuffer-fetch support: rasterization_order_attachment_access ext %d (color feature %d, depth %d, stencil %d); dynamic_rendering_local_read ext %d (feature %d); attachment_feedback_loop_layout ext %d; dynamic_rendering ext %d; classic render-pass input attachments: core 1.0",
                   hasRO, orderedColor.rasterizationOrderColorAttachmentAccess, orderedColor.rasterizationOrderDepthAttachmentAccess, orderedColor.rasterizationOrderStencilAttachmentAccess, hasLR, flr.dynamicRenderingLocalRead, hasFL, hasDR);
            orderedColor.pNext = NULL;
            supportedNull.pNext = NULL;
            fragInterlockFeat.pNext = NULL;
            baryFeat.pNext = NULL;
            atomicFloatFeat.pNext = NULL;
            atomicFloat2Feat.pNext = NULL;
            meshFeat.pNext = NULL;
            accelFeat.pNext = NULL;
            rayQueryFeat.pNext = NULL;
            rtPosFetchFeat.pNext = NULL;
            gplFeat.pNext = NULL;
            eds3Feat.pNext = NULL;
            if (vkGetPhysicalDeviceProperties2 && pp.apiVersion >= VK_API_VERSION_1_2) {
                VkPhysicalDeviceSampleLocationsPropertiesEXT slp = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLE_LOCATIONS_PROPERTIES_EXT };
                VkPhysicalDeviceDepthStencilResolveProperties dsr = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES };
                void *p2bNext = &dsr;
                if (hasSampleLoc) { slp.pNext = p2bNext; p2bNext = &slp; }
                if (hasMeshShader) { meshProps.pNext = p2bNext; p2bNext = &meshProps; }
                if (hasAccelStruct) { accelProps.pNext = p2bNext; p2bNext = &accelProps; }
                if (hasPushDesc) { pushDescProps.pNext = p2bNext; p2bNext = &pushDescProps; }
                if (hasGpl) { gplProps.pNext = p2bNext; p2bNext = &gplProps; }
                VkPhysicalDeviceProperties2 p2b = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = p2bNext };
                vkGetPhysicalDeviceProperties2(RDNA4R.pd, &p2b);
                meshProps.pNext = NULL;
                accelProps.pNext = NULL;
                pushDescProps.pNext = NULL;
                gplProps.pNext = NULL;
                if (hasPushDesc) RDNA4R.maxPushDesc = pushDescProps.maxPushDescriptors;
                RDNA4R.depthResolveModes = dsr.supportedDepthResolveModes;
                RDNA4R.stencilResolveModes = dsr.supportedStencilResolveModes;
                RDNA4R.dsResolveIndep = dsr.independentResolve != VK_FALSE;
                RDNA4R.dsResolveIndepNone = dsr.independentResolveNone != VK_FALSE;
                if (hasSampleLoc) {
                    RDNA4R.sampleLocationCounts = slp.sampleLocationSampleCounts;
                    RDNA4R.sampleLocationGrid = slp.maxSampleLocationGridSize;
                    RDNA4R.sampleLocationRange[0] = slp.sampleLocationCoordinateRange[0];
                    RDNA4R.sampleLocationRange[1] = slp.sampleLocationCoordinateRange[1];
                    RDNA4R.variableSampleLocations = slp.variableSampleLocations != VK_FALSE;
                }
            }
        }
        const char *devExts[32]; uint32_t devExtCount = 0;
        if (RDNA4R.hostExt) devExts[devExtCount++] = VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME;
        if (hasSampleLoc) devExts[devExtCount++] = VK_EXT_SAMPLE_LOCATIONS_EXTENSION_NAME;
        RDNA4R.orderedColorAccess = orderedColorExt && orderedColor.rasterizationOrderColorAttachmentAccess;
        if (RDNA4R.orderedColorAccess) {
            orderedColor.rasterizationOrderDepthAttachmentAccess = VK_FALSE;
            orderedColor.rasterizationOrderStencilAttachmentAccess = VK_FALSE;
            enabled13.pNext = &orderedColor;
            devExts[devExtCount++] = orderedColorExt;
        }
        VkPhysicalDeviceRobustness2FeaturesEXT enabledNull = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT,
            .robustBufferAccess2 = pf.robustBufferAccess && supportedNull.robustBufferAccess2,
            .nullDescriptor = supportedNull.nullDescriptor };
        RDNA4R.nullDescriptors = enabledNull.nullDescriptor;
        RDNA4R.robustTexelBuffers = enabledNull.robustBufferAccess2;
        ef.robustBufferAccess = RDNA4R.robustTexelBuffers;
        if (RDNA4R.nullDescriptors || RDNA4R.robustTexelBuffers) {
            enabledNull.pNext = enabled13.pNext;
            enabled13.pNext = &enabledNull;
            devExts[devExtCount++] = VK_EXT_ROBUSTNESS_2_EXTENSION_NAME;
        }
        RDNA4R.fragInterlock = hasInterlock && fragInterlockFeat.fragmentShaderPixelInterlock;
        if (RDNA4R.fragInterlock) {
            fragInterlockFeat.fragmentShaderShadingRateInterlock = VK_FALSE;
            fragInterlockFeat.pNext = enabled13.pNext;
            enabled13.pNext = &fragInterlockFeat;
            devExts[devExtCount++] = VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME;
        }
        VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR enabledBary = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR,
            .fragmentShaderBarycentric = hasBarycentric && baryFeat.fragmentShaderBarycentric };
        if (enabledBary.fragmentShaderBarycentric) {
            enabledBary.pNext = enabled13.pNext;
            enabled13.pNext = &enabledBary;
            devExts[devExtCount++] = VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME;
        }
        VkPhysicalDeviceShaderAtomicFloatFeaturesEXT enabledAtomicFloat = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT,
            .shaderBufferFloat32Atomics = hasAtomicFloat && atomicFloatFeat.shaderBufferFloat32Atomics,
            .shaderBufferFloat32AtomicAdd = hasAtomicFloat && atomicFloatFeat.shaderBufferFloat32AtomicAdd,
            .shaderSharedFloat32Atomics = hasAtomicFloat && atomicFloatFeat.shaderSharedFloat32Atomics,
            .shaderSharedFloat32AtomicAdd = hasAtomicFloat && atomicFloatFeat.shaderSharedFloat32AtomicAdd,
            .shaderImageFloat32Atomics = hasAtomicFloat && atomicFloatFeat.shaderImageFloat32Atomics,
            .shaderImageFloat32AtomicAdd = hasAtomicFloat && atomicFloatFeat.shaderImageFloat32AtomicAdd };
        BOOL enableAtomicFloat = enabledAtomicFloat.shaderBufferFloat32Atomics ||
            enabledAtomicFloat.shaderBufferFloat32AtomicAdd || enabledAtomicFloat.shaderSharedFloat32Atomics ||
            enabledAtomicFloat.shaderSharedFloat32AtomicAdd || enabledAtomicFloat.shaderImageFloat32Atomics ||
            enabledAtomicFloat.shaderImageFloat32AtomicAdd;
        if (enableAtomicFloat) {
            enabledAtomicFloat.pNext = enabled13.pNext;
            enabled13.pNext = &enabledAtomicFloat;
            devExts[devExtCount++] = VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME;
        }
        VkPhysicalDeviceShaderAtomicFloat2FeaturesEXT enabledAtomicFloat2 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_2_FEATURES_EXT,
            .shaderBufferFloat32AtomicMinMax = enableAtomicFloat && hasAtomicFloat2 && atomicFloat2Feat.shaderBufferFloat32AtomicMinMax,
            .shaderSharedFloat32AtomicMinMax = enableAtomicFloat && hasAtomicFloat2 && atomicFloat2Feat.shaderSharedFloat32AtomicMinMax,
            .shaderImageFloat32AtomicMinMax = enableAtomicFloat && hasAtomicFloat2 && atomicFloat2Feat.shaderImageFloat32AtomicMinMax };
        BOOL enableAtomicFloat2 = enabledAtomicFloat2.shaderBufferFloat32AtomicMinMax ||
            enabledAtomicFloat2.shaderSharedFloat32AtomicMinMax || enabledAtomicFloat2.shaderImageFloat32AtomicMinMax;
        if (enableAtomicFloat2) {
            enabledAtomicFloat2.pNext = enabled13.pNext;
            enabled13.pNext = &enabledAtomicFloat2;
            devExts[devExtCount++] = VK_EXT_SHADER_ATOMIC_FLOAT_2_EXTENSION_NAME;
        }
        VkPhysicalDeviceMeshShaderFeaturesEXT enabledMesh = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT,
            .taskShader = pp.apiVersion >= VK_API_VERSION_1_2 && hasMeshShader && meshFeat.meshShader && meshFeat.taskShader,
            .meshShader = pp.apiVersion >= VK_API_VERSION_1_2 && hasMeshShader && meshFeat.meshShader };
        if (enabledMesh.meshShader) {
            enabledMesh.pNext = enabled13.pNext;
            enabled13.pNext = &enabledMesh;
            devExts[devExtCount++] = VK_EXT_MESH_SHADER_EXTENSION_NAME;
        }
        BOOL enableAccel = pp.apiVersion >= VK_API_VERSION_1_2 && enabled12.bufferDeviceAddress &&
            hasDeferredHostOps && hasAccelStruct && accelFeat.accelerationStructure;
        VkPhysicalDeviceAccelerationStructureFeaturesKHR enabledAccel = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,
            .accelerationStructure = enableAccel,
            .accelerationStructureIndirectBuild = enableAccel && accelFeat.accelerationStructureIndirectBuild,
            .descriptorBindingAccelerationStructureUpdateAfterBind = enableAccel && enabled12.descriptorIndexing &&
                accelFeat.descriptorBindingAccelerationStructureUpdateAfterBind };
        if (enableAccel) {
            enabledAccel.pNext = enabled13.pNext;
            enabled13.pNext = &enabledAccel;
            devExts[devExtCount++] = VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME;
            devExts[devExtCount++] = VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME;
        }
        VkPhysicalDeviceRayQueryFeaturesKHR enabledRayQuery = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR,
            .rayQuery = enableAccel && hasRayQuery && rayQueryFeat.rayQuery };
        if (enabledRayQuery.rayQuery) {
            enabledRayQuery.pNext = enabled13.pNext;
            enabled13.pNext = &enabledRayQuery;
            devExts[devExtCount++] = VK_KHR_RAY_QUERY_EXTENSION_NAME;
        }
        VkPhysicalDeviceRayTracingPositionFetchFeaturesKHR enabledRtPosFetch = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_POSITION_FETCH_FEATURES_KHR,
            .rayTracingPositionFetch = enabledRayQuery.rayQuery && hasRtPosFetch && rtPosFetchFeat.rayTracingPositionFetch };
        if (enabledRtPosFetch.rayTracingPositionFetch) {
            enabledRtPosFetch.pNext = enabled13.pNext;
            enabled13.pNext = &enabledRtPosFetch;
            devExts[devExtCount++] = VK_KHR_RAY_TRACING_POSITION_FETCH_EXTENSION_NAME;
        }
        BOOL hasSparseQueue = (qf[qfi].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT) != 0;
        BOOL hasBindSparse = rdna4_gipa(RDNA4R.inst, "vkQueueBindSparse") != NULL;
        if (pf.sparseBinding && hasSparseQueue && hasBindSparse) {
            ef.sparseBinding = VK_TRUE;
            ef.sparseResidencyBuffer = pf.sparseResidencyBuffer;
            ef.sparseResidencyImage2D = pf.sparseResidencyImage2D;
            ef.sparseResidencyImage3D = pf.sparseResidencyImage3D;
            ef.sparseResidency2Samples = pf.sparseResidency2Samples;
            ef.sparseResidency4Samples = pf.sparseResidency4Samples;
            ef.sparseResidency8Samples = pf.sparseResidency8Samples;
            ef.sparseResidency16Samples = pf.sparseResidency16Samples;
            ef.sparseResidencyAliased = pf.sparseResidencyAliased;
        }
        RDNA4LOG("radv: fragment shader pixel interlock %s", RDNA4R.fragInterlock ? "enabled" : "unavailable");
        RDNA4LOG("radv: null descriptors %s", RDNA4R.nullDescriptors ? "enabled" : "unavailable (typed dummy images)");
        RDNA4LOG("radv: robust texture-buffer access %s", RDNA4R.robustTexelBuffers ? "enabled" : "unavailable");
        RDNA4LOG("radv: GPU argument tables %s", RDNA4R.gpuArgumentTables ? "enabled" : "unavailable");
        RDNA4R.drmModExt = hasDrmMod && vkGetPhysicalDeviceImageFormatProperties2;
        if (RDNA4R.drmModExt) devExts[devExtCount++] = VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME;
        if (hasPushDesc) devExts[devExtCount++] = VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME;
        const int killGpl = (access(GARNET_NOGPL_FILE, F_OK) == 0 ||
            (rdna4_allow() && garnet_getenv("GARNET_NOGPL", "RDNA4M_NOGPL"))) ? 1 : 0;
        VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT enabledGpl = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT,
            .graphicsPipelineLibrary = (!killGpl && pp.apiVersion >= VK_API_VERSION_1_3 &&
                hasPipeLib && hasGpl && gplFeat.graphicsPipelineLibrary && gplProps.graphicsPipelineLibraryFastLinking)
                ? VK_TRUE : VK_FALSE };
        if (enabledGpl.graphicsPipelineLibrary) {
            enabledGpl.pNext = enabled13.pNext;
            enabled13.pNext = &enabledGpl;
            devExts[devExtCount++] = VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME;
            devExts[devExtCount++] = VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME;
        }
        VkPhysicalDeviceExtendedDynamicState3FeaturesEXT enabledEds3 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT,
            .extendedDynamicState3PolygonMode = (enabledGpl.graphicsPipelineLibrary && hasEds3 && eds3Feat.extendedDynamicState3PolygonMode) ? VK_TRUE : VK_FALSE,
            .extendedDynamicState3DepthClampEnable = (enabledGpl.graphicsPipelineLibrary && hasEds3 && eds3Feat.extendedDynamicState3DepthClampEnable) ? VK_TRUE : VK_FALSE };
        if (enabledEds3.extendedDynamicState3PolygonMode || enabledEds3.extendedDynamicState3DepthClampEnable) {
            enabledEds3.pNext = enabled13.pNext;
            enabled13.pNext = &enabledEds3;
            devExts[devExtCount++] = VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME;
        }
        RDNA4LOG("radv: VK_EXT_image_drm_format_modifier %s (VK_KHR_image_format_list is core at API 1.2; device API is 1.3)", RDNA4R.drmModExt ? "enabled" : "unavailable");
        const int killAsyncCompute = (access(GARNET_NO_ASYNC_COMPUTE_FILE, F_OK) == 0) ? 1 : 0;
        const int wantAsyncCompute = garnet_async_compute_device_enable(
            hasDedicatedComputeQF, enabled12.timelineSemaphore != VK_FALSE ? 1 : 0,
            rdna4_is_ws() ? 1 : 0, killAsyncCompute);
        VkDeviceQueueCreateInfo dqs[2] = {
            dq,
            { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = hasDedicatedComputeQF ? cqfi : qfi, .queueCount = 1, .pQueuePriorities = &prio }
        };
        VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &enabled11,
                                   .queueCreateInfoCount = wantAsyncCompute ? 2u : 1u, .pQueueCreateInfos = dqs, .pEnabledFeatures = &ef,
                                   .enabledExtensionCount = devExtCount, .ppEnabledExtensionNames = devExts };
        r = vkCreateDevice(RDNA4R.pd, &dci, NULL, &RDNA4R.dev);
        if (r != VK_SUCCESS) { RDNA4R.err = rdna4_err(7, [NSString stringWithFormat:@"vkCreateDevice = %d", r]); goto fail; }
        if (hasPushDesc && vkGetDeviceProcAddr) {
            RDNA4R.cmdPushDescSet = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(RDNA4R.dev, "vkCmdPushDescriptorSetKHR");
            RDNA4R.pushDescExt = (RDNA4R.cmdPushDescSet != NULL && RDNA4R.maxPushDesc >= 8u);
        }
        RDNA4LOG("radv: VK_KHR_push_descriptor %s (maxPushDescriptors %u)", RDNA4R.pushDescExt ? "enabled" : "unavailable", RDNA4R.maxPushDesc);
        if (enabledGpl.graphicsPipelineLibrary && vkGetDeviceProcAddr) {
            RDNA4R.cmdSetCullMode = (PFN_vkCmdSetCullMode)vkGetDeviceProcAddr(RDNA4R.dev, "vkCmdSetCullMode");
            RDNA4R.cmdSetFrontFace = (PFN_vkCmdSetFrontFace)vkGetDeviceProcAddr(RDNA4R.dev, "vkCmdSetFrontFace");
            RDNA4R.cmdSetPrimitiveTopology = (PFN_vkCmdSetPrimitiveTopology)vkGetDeviceProcAddr(RDNA4R.dev, "vkCmdSetPrimitiveTopology");
            RDNA4R.cmdSetPrimitiveRestartEnable = (PFN_vkCmdSetPrimitiveRestartEnable)vkGetDeviceProcAddr(RDNA4R.dev, "vkCmdSetPrimitiveRestartEnable");
            RDNA4R.cmdSetDepthTestEnable = (PFN_vkCmdSetDepthTestEnable)vkGetDeviceProcAddr(RDNA4R.dev, "vkCmdSetDepthTestEnable");
            RDNA4R.cmdSetDepthWriteEnable = (PFN_vkCmdSetDepthWriteEnable)vkGetDeviceProcAddr(RDNA4R.dev, "vkCmdSetDepthWriteEnable");
            RDNA4R.cmdSetDepthCompareOp = (PFN_vkCmdSetDepthCompareOp)vkGetDeviceProcAddr(RDNA4R.dev, "vkCmdSetDepthCompareOp");
            RDNA4R.cmdSetStencilTestEnable = (PFN_vkCmdSetStencilTestEnable)vkGetDeviceProcAddr(RDNA4R.dev, "vkCmdSetStencilTestEnable");
            RDNA4R.cmdSetStencilOp = (PFN_vkCmdSetStencilOp)vkGetDeviceProcAddr(RDNA4R.dev, "vkCmdSetStencilOp");
            RDNA4R.gplExt = (RDNA4R.cmdSetCullMode && RDNA4R.cmdSetFrontFace && RDNA4R.cmdSetPrimitiveTopology &&
                             RDNA4R.cmdSetPrimitiveRestartEnable && RDNA4R.cmdSetDepthTestEnable && RDNA4R.cmdSetDepthWriteEnable &&
                             RDNA4R.cmdSetDepthCompareOp && RDNA4R.cmdSetStencilTestEnable && RDNA4R.cmdSetStencilOp) ? YES : NO;
            if (RDNA4R.gplExt && enabledEds3.extendedDynamicState3PolygonMode) {
                RDNA4R.cmdSetPolygonMode = (PFN_vkCmdSetPolygonModeEXT)vkGetDeviceProcAddr(RDNA4R.dev, "vkCmdSetPolygonModeEXT");
                RDNA4R.eds3PolyMode = (RDNA4R.cmdSetPolygonMode != NULL);
            }
            if (RDNA4R.gplExt && enabledEds3.extendedDynamicState3DepthClampEnable) {
                RDNA4R.cmdSetDepthClampEnable = (PFN_vkCmdSetDepthClampEnableEXT)vkGetDeviceProcAddr(RDNA4R.dev, "vkCmdSetDepthClampEnableEXT");
                RDNA4R.eds3DepthClamp = (RDNA4R.cmdSetDepthClampEnable != NULL);
            }
        }
        RDNA4LOG("radv: GPL fast-link %s (fastLink=%u eds3Poly=%d eds3Clamp=%d kill=%d)",
                 RDNA4R.gplExt ? "enabled" : "disabled", (unsigned)gplProps.graphicsPipelineLibraryFastLinking,
                 RDNA4R.eds3PolyMode ? 1 : 0, RDNA4R.eds3DepthClamp ? 1 : 0, killGpl);
        if (enabled12.bufferDeviceAddress && vkGetDeviceProcAddr) {
            rdna4_vkGetBufferAddress = (PFN_vkGetBufferDeviceAddress)vkGetDeviceProcAddr(RDNA4R.dev, "vkGetBufferDeviceAddress");
            RDNA4R.bufferAddress = rdna4_vkGetBufferAddress != NULL;
        }
        if (hasSampleLoc && vkGetDeviceProcAddr) {
            rdna4_vkCmdSetSampleLocationsEXT = (PFN_vkCmdSetSampleLocationsEXT)vkGetDeviceProcAddr(RDNA4R.dev, "vkCmdSetSampleLocationsEXT");
            RDNA4R.sampleLocationsExt = rdna4_vkCmdSetSampleLocationsEXT != NULL && RDNA4R.sampleLocationCounts != 0;
        }
        vkGetDeviceQueue(RDNA4R.dev, qfi, 0, &RDNA4R.q);
        pthread_mutex_init(&RDNA4R.computeQlock, NULL);
        RDNA4R.computeQ = VK_NULL_HANDLE;
        RDNA4R.computeQfi = qfi;
        RDNA4R.asyncComputeOn = NO;
        RDNA4R.waitSemaphores = vkGetDeviceProcAddr
            ? (PFN_vkWaitSemaphores)vkGetDeviceProcAddr(RDNA4R.dev, "vkWaitSemaphores") : NULL;
        if (!RDNA4R.waitSemaphores && vkGetDeviceProcAddr)
            RDNA4R.waitSemaphores = (PFN_vkWaitSemaphores)vkGetDeviceProcAddr(RDNA4R.dev, "vkWaitSemaphoresKHR");
        if (wantAsyncCompute && vkCreateSemaphore && vkDestroySemaphore) {
            vkGetDeviceQueue(RDNA4R.dev, cqfi, 0, &RDNA4R.computeQ);
            if (RDNA4R.computeQ != VK_NULL_HANDLE) {
                RDNA4R.computeQfi = cqfi;
                RDNA4R.asyncComputeOn = YES;
            }
        }
        RDNA4LOG("radv: async compute queue %s (dedicatedQF=%d cqfi=%u timeline=%d ws=%d kill=%d)",
                 RDNA4R.asyncComputeOn ? "enabled" : "disabled",
                 hasDedicatedComputeQF, hasDedicatedComputeQF ? cqfi : 0u,
                 enabled12.timelineSemaphore != VK_FALSE ? 1 : 0,
                 rdna4_is_ws() ? 1 : 0, killAsyncCompute);
        RDNA4R.hostAlign = 4096;
        if (RDNA4R.hostExt && vkGetDeviceProcAddr && vkGetPhysicalDeviceProperties2) {
            rdna4_vkGetMHPP = (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(RDNA4R.dev, "vkGetMemoryHostPointerPropertiesEXT");
            VkPhysicalDeviceExternalMemoryHostPropertiesEXT emh = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT };
            VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &emh };
            vkGetPhysicalDeviceProperties2(RDNA4R.pd, &p2);
            if (emh.minImportedHostPointerAlignment) RDNA4R.hostAlign = emh.minImportedHostPointerAlignment;
            RDNA4LOG("radv: VK_EXT_external_memory_host enabled, vkGetMemoryHostPointerPropertiesEXT %s, minImportedHostPointerAlignment %llu", rdna4_vkGetMHPP ? "present" : "MISSING", (unsigned long long)RDNA4R.hostAlign);
        } else RDNA4LOG("radv: VK_EXT_external_memory_host NOT available (IOSurface-backed textures will be refused)");
        vkGetPhysicalDeviceMemoryProperties(RDNA4R.pd, &RDNA4R.mp);
        pthread_mutex_init(&RDNA4R.qlock, NULL);
        VkCommandPoolCreateInfo opc = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qfi, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT };
        r = vkCreateCommandPool(RDNA4R.dev, &opc, NULL, &RDNA4R.opool);   // one-shot uploads (replaceRegion, getBytes)
        if (r != VK_SUCCESS) { RDNA4R.err = rdna4_err(8, [NSString stringWithFormat:@"vkCreateCommandPool(one-shot) = %d", r]); goto fail; }
        rdna4_pc_open(&pp);
        rdna4_pool_latch();   // P1: reads /private/tmp/rdna4m-pool ONCE
        rdna4_imp_latch();    // P4: reads the kill file /private/tmp/rdna4m-noimpcache ONCE (build 16: the cache is ON by default)
        rdna4_vs_latch();     // Phase B: WindowServer OPTIMAL VRAM surface cache (kill file /private/tmp/garnet-no-vram-surf)
        rdna4_drawopt_latch();   // P5b: reads /private/tmp/rdna4m-drawopt ONCE
        RDNA4R.ok = YES;
        rdna4dc_heap heaps[VK_MAX_MEMORY_HEAPS] = {0};
        uint32_t heapCount = MIN(RDNA4R.mp.memoryHeapCount, VK_MAX_MEMORY_HEAPS);
        for (uint32_t i = 0; i < heapCount; i++) {
            heaps[i].size = RDNA4R.mp.memoryHeaps[i].size;
            heaps[i].device_local = (RDNA4R.mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
        }
        BOOL hasWriteTimestamp = (rdna4_gipa(RDNA4R.inst, "vkCmdWriteTimestamp2") != NULL ||
                                  rdna4_gipa(RDNA4R.inst, "vkCmdWriteTimestamp") != NULL);
        rdna4dc_metal3_probe m3probe = {
            .api_1_2 = pp.apiVersion >= VK_API_VERSION_1_2,
            .shader_int64 = ef.shaderInt64 != VK_FALSE,
            .buffer_device_address = RDNA4R.bufferAddress ? 1 : 0,
            .sample_rate_shading = ef.sampleRateShading != VK_FALSE,
            .ext_barycentric = enabledBary.fragmentShaderBarycentric != VK_FALSE,
            .feat_barycentric = enabledBary.fragmentShaderBarycentric != VK_FALSE,
            .ext_atomic_float = enableAtomicFloat ? 1 : 0,
            .feat_atomic_float_buffer = enabledAtomicFloat.shaderBufferFloat32Atomics != VK_FALSE,
            .feat_atomic_float_buffer_add = enabledAtomicFloat.shaderBufferFloat32AtomicAdd != VK_FALSE,
            .feat_atomic_float_shared = enabledAtomicFloat.shaderSharedFloat32Atomics != VK_FALSE,
            .feat_atomic_float_shared_add = enabledAtomicFloat.shaderSharedFloat32AtomicAdd != VK_FALSE,
            .feat_atomic_float_image = enabledAtomicFloat.shaderImageFloat32Atomics != VK_FALSE,
            .feat_atomic_float_image_add = enabledAtomicFloat.shaderImageFloat32AtomicAdd != VK_FALSE,
            .ext_atomic_float2 = enableAtomicFloat2 ? 1 : 0,
            .feat_atomic_float_buffer_minmax = enabledAtomicFloat2.shaderBufferFloat32AtomicMinMax != VK_FALSE,
            .feat_atomic_float_shared_minmax = enabledAtomicFloat2.shaderSharedFloat32AtomicMinMax != VK_FALSE,
            .ext_atomic_int64 = (pp.apiVersion >= VK_API_VERSION_1_2 && hasAtomicInt64) ? 1 : 0,
            .feat_atomic_int64_buffer = enabled12.shaderBufferInt64Atomics != VK_FALSE,
            .feat_atomic_int64_shared = enabled12.shaderSharedInt64Atomics != VK_FALSE,
            .ext_mesh_shader = enabledMesh.meshShader != VK_FALSE,
            .feat_task_shader = enabledMesh.taskShader != VK_FALSE,
            .feat_mesh_shader = enabledMesh.meshShader != VK_FALSE,
            .mesh_max_payload_bytes = meshProps.maxTaskPayloadSize,
            .object_max_workgroup_count = { meshProps.maxTaskWorkGroupCount[0], meshProps.maxTaskWorkGroupCount[1], meshProps.maxTaskWorkGroupCount[2] },
            .object_max_workgroup_total_count = meshProps.maxTaskWorkGroupTotalCount,
            .object_max_workgroup_invocations = meshProps.maxTaskWorkGroupInvocations,
            .mesh_max_workgroup_count = { meshProps.maxMeshWorkGroupCount[0], meshProps.maxMeshWorkGroupCount[1], meshProps.maxMeshWorkGroupCount[2] },
            .mesh_max_workgroup_total_count = meshProps.maxMeshWorkGroupTotalCount,
            .mesh_max_workgroup_invocations = meshProps.maxMeshWorkGroupInvocations,
            .mesh_max_output_vertices = meshProps.maxMeshOutputVertices,
            .mesh_max_output_primitives = meshProps.maxMeshOutputPrimitives,
            .ext_deferred_host_ops = enableAccel ? 1 : 0,
            .ext_accel_struct = enableAccel ? 1 : 0,
            .feat_accel_struct = enabledAccel.accelerationStructure != VK_FALSE,
            .feat_accel_indirect_build = enabledAccel.accelerationStructureIndirectBuild != VK_FALSE,
            .feat_accel_uab = enabledAccel.descriptorBindingAccelerationStructureUpdateAfterBind != VK_FALSE,
            .rt_min_scratch_alignment = accelProps.minAccelerationStructureScratchOffsetAlignment,
            .rt_max_geometries = accelProps.maxGeometryCount,
            .rt_max_instances = accelProps.maxInstanceCount,
            .rt_max_primitives = accelProps.maxPrimitiveCount,
            .ext_ray_query = enabledRayQuery.rayQuery != VK_FALSE,
            .feat_ray_query = enabledRayQuery.rayQuery != VK_FALSE,
            .ext_ray_tracing_position_fetch = enabledRtPosFetch.rayTracingPositionFetch != VK_FALSE,
            .feat_ray_tracing_position_fetch = enabledRtPosFetch.rayTracingPositionFetch != VK_FALSE,
            .timestamp_query_funcs = (RDNA4R.occOK && hasWriteTimestamp) ? 1 : 0,
            .timestamp_compute_and_graphics = pp.limits.timestampComputeAndGraphics != VK_FALSE,
            .timestamp_period_ns = pp.limits.timestampPeriod,
            .timestamp_valid_bits = qf[qfi].timestampValidBits,
            .sparse_queue = hasSparseQueue ? 1 : 0,
            .sparse_bind_func = hasBindSparse ? 1 : 0,
            .sparse_binding = ef.sparseBinding != VK_FALSE,
            .sparse_residency_buffer = ef.sparseResidencyBuffer != VK_FALSE,
            .sparse_residency_image_2d = ef.sparseResidencyImage2D != VK_FALSE,
            .sparse_residency_image_3d = ef.sparseResidencyImage3D != VK_FALSE,
            .sparse_residency_2_samples = ef.sparseResidency2Samples != VK_FALSE,
            .sparse_residency_4_samples = ef.sparseResidency4Samples != VK_FALSE,
            .sparse_residency_8_samples = ef.sparseResidency8Samples != VK_FALSE,
            .sparse_residency_16_samples = ef.sparseResidency16Samples != VK_FALSE,
            .sparse_residency_aliased = ef.sparseResidencyAliased != VK_FALSE
        };
        rdna4dc_properties caps = { .name = pp.deviceName, .storage_buffer_range = pp.limits.maxStorageBufferRange,
            .allocation_size = maxAllocation, .heaps = heaps, .heap_count = heapCount,
            .workgroup_size = {pp.limits.maxComputeWorkGroupSize[0], pp.limits.maxComputeWorkGroupSize[1], pp.limits.maxComputeWorkGroupSize[2]},
            .invocations = pp.limits.maxComputeWorkGroupInvocations, .shared_memory = pp.limits.maxComputeSharedMemorySize, .subgroup_size = subgroupSize,
            .logical_buffer_size = maxBuffer, .addressing64 = RDNA4R.bufferAddress && ef.shaderInt64,
            .metal3 = &m3probe };
        PFN_vkGetPhysicalDeviceMemoryProperties2 memoryProperties2 = (PFN_vkGetPhysicalDeviceMemoryProperties2)
            rdna4_gipa(RDNA4R.inst, "vkGetPhysicalDeviceMemoryProperties2");
        if (hasMemoryBudget && memoryProperties2) {
            VkPhysicalDeviceMemoryBudgetPropertiesEXT budget = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT };
            VkPhysicalDeviceMemoryProperties2 memory = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, .pNext = &budget };
            memoryProperties2(RDNA4R.pd, &memory);
            caps.budget_valid = 1;
            for (uint32_t i = 0; i < heapCount; ++i) {
                if (!heaps[i].device_local) continue;
                uint64_t part = MIN(heaps[i].size, budget.heapBudget[i]);
                caps.allocation_budget = part > UINT64_MAX - caps.allocation_budget ? UINT64_MAX : caps.allocation_budget + part;
            }
        }
        if (pp.apiVersion >= VK_API_VERSION_1_1 && RDNA4R.gpuArgumentTables)
            queryCaps.flags |= RDNA4X_CAP_QUERY_DEVICE;
        RDNA4QueryCaps = queryCaps;
        RDNA4Caps = rdna4dc_from_properties(&caps);
        const rdna4dc_metal3_caps m3 = RDNA4Caps.metal3;
        RDNA4R.barycentric = m3.barycentric ? YES : NO;
        RDNA4R.atomicFloat = (m3.atomic_float_buffer || m3.atomic_float_shared || m3.atomic_float_image) ? YES : NO;
        RDNA4R.atomicFloat2 = m3.atomic_float_minmax ? YES : NO;
        RDNA4R.atomicInt64 = (m3.atomic_int64_buffer || m3.atomic_int64_shared) ? YES : NO;
        RDNA4R.meshShader = m3.mesh_shader ? YES : NO;
        RDNA4R.objectShader = m3.object_shader ? YES : NO;
        RDNA4R.deferredHostOps = m3.deferred_host_ops ? YES : NO;
        RDNA4R.accelStruct = m3.acceleration_structure ? YES : NO;
        RDNA4R.rayQuery = m3.ray_query ? YES : NO;
        RDNA4R.rtPositionFetch = m3.ray_tracing_position_fetch ? YES : NO;
        RDNA4R.timestampQuery = m3.timestamp_supported ? YES : NO;
        RDNA4R.timestampComputeAndGraphics = m3.timestamp_compute_and_graphics ? YES : NO;
        RDNA4R.timestampPeriod = m3.timestamp_period_ns;
        RDNA4R.timestampValidBits = m3.timestamp_valid_bits;
        RDNA4R.sparseBinding = m3.sparse_binding ? YES : NO;
        RDNA4R.sparseResidencyBuffer = m3.sparse_residency_buffer ? YES : NO;
        RDNA4R.sparseResidencyImage2D = m3.sparse_residency_image_2d ? YES : NO;
        RDNA4R.sparseResidencyImage3D = m3.sparse_residency_image_3d ? YES : NO;
        RDNA4LOG("radv: metal3 probe: bary=%d pull=%d atomicF32=%d/%d/%d(mm=%d) atomicI64=%d/%d mesh=%d/%d(payload=%u grid=%u) rt=%d/%d(posFetch=%d) ts=%d(%.2fns,%ub) sparse=%d(buf=%d 2d=%d 3d=%d)",
                 m3.barycentric, m3.pull_model_interpolation,
                 m3.atomic_float_buffer, m3.atomic_float_shared, m3.atomic_float_image, m3.atomic_float_minmax,
                 m3.atomic_int64_buffer, m3.atomic_int64_shared,
                 m3.object_shader, m3.mesh_shader, m3.mesh_max_payload_bytes, m3.mesh_max_workgroup_total_count,
                 m3.acceleration_structure, m3.ray_query, m3.ray_tracing_position_fetch,
                 m3.timestamp_supported, (double)m3.timestamp_period_ns, m3.timestamp_valid_bits,
                 m3.sparse_binding, m3.sparse_residency_buffer, m3.sparse_residency_image_2d, m3.sparse_residency_image_3d);
        atomic_store_explicit(&rdna4_caps_ready, 1, memory_order_release);
        RDNA4GatherCaps = gatherCaps;
        atomic_store_explicit(&rdna4_gather_caps_ready, 1, memory_order_release);
        rdna4_t1_start();
        rdna4s_prebind_on_radv_open();
        rdna4_gpl_prewarm_start();
        RDNA4LOG("radv open: OK (queue family %u)", qfi);
        return YES;
    fail:
        RDNA4LOG("radv open: FAILED %s", RDNA4R.err.localizedDescription.UTF8String);
        if (err) *err = RDNA4R.err;
        return NO;
    }
}

// #12 R4: a refused RDNA4N open (code 5: busy / not ready) is transient. The first open of the process retries with backoff
// (0.1, 0.2, 0.4, 0.8, 1.5 s, bounded to 3 s from the first attempt) before giving up, logging every attempt; after that, a later request retries ONCE if the
// last failure is >= 2 s old (so a lazy open that lost a race with a closing client is not lost for the life of the process).
// Any other failure (no dylib, vkCreateDevice, ...) is permanent for the process, as before.
GARNET_INTERNAL BOOL rdna4_radv_open(NSError **err) {
    @synchronized ([GarnetDevice class]) {
        if (RDNA4R.ok) return YES;
        static uint64_t lastFail;
        BOOL transient = RDNA4R.tried && RDNA4R.err.code == 5;
        if (RDNA4R.tried && !(transient && rdna4_now() - lastFail >= 2000000000ULL)) { if (err) *err = RDNA4R.err; return NO; }
        static const useconds_t backoff[] = { 100000, 200000, 400000, 800000, 1500000 };
        const uint64_t budget = 3000000000ULL;   // first open of the process: keep trying until 3 s have passed since the first attempt began (an attempt itself can take ~2 s on the PC)
        BOOL first = !RDNA4R.tried; RDNA4R.tried = YES;
        uint64_t t0 = rdna4_now();
        for (int a = 0; a < (first ? 6 : 1); a++) {
            if (a) {
                uint64_t el = rdna4_now() - t0;
                if (el >= budget) { RDNA4LOG("radv open: giving up after %.2f s and %d attempt(s)", (double)el / 1e9, a); break; }
                useconds_t d = backoff[a - 1]; if ((uint64_t)d * 1000ULL > budget - el) d = (useconds_t)((budget - el) / 1000ULL);
                RDNA4LOG("radv open: refused (%s); retry %d after %u ms (%.2f s used of 3 s)", RDNA4R.err.localizedDescription.UTF8String, a, d / 1000, (double)el / 1e9); usleep(d);
            }
            RDNA4LOG("radv open: attempt %d%s", a + 1, first ? "" : " (retry after an earlier refusal)");
            if (rdna4_test_refuse()) { RDNA4R.err = rdna4_err(5, @"vkEnumeratePhysicalDevices = -3 count 0 (RDNA4M_TEST_OPEN_REFUSE: simulated refusal)"); continue; }
            if (rdna4_radv_open_once(err)) { if (a || !first) RDNA4LOG("radv open: succeeded on attempt %d", a + 1); return YES; }
            if (RDNA4R.err.code != 5) break;
        }
        lastFail = rdna4_now();
        if (err) *err = RDNA4R.err;
        return NO;
    }
}

GARNET_INTERNAL int rdna4_find_mem(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < RDNA4R.mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (RDNA4R.mp.memoryTypes[i].propertyFlags & want) == want) return (int)i;
    return -1;
}

#endif // !defined(GARNET_ARGUMENT_LIMIT_SNAPSHOT_ONLY)
