// garnet_shaders.m - GarnetMetal modular compilation unit.
#if !defined(GARNET_INTERNAL) && !defined(GARNET_METAL_MODULAR_BUILD)
#define GARNET_INTERNAL static
#endif
#if !defined(GARNET_SHADER_REQUEST_CAPTURE_ONLY)
#ifdef GARNET_METAL_MODULAR_BUILD
#include "garnet_metal_internal.h"
#endif
#ifndef GARNET_SHADERS_INTERNAL_H
#include "garnet_shaders_internal.h"
#include "garnet_dumpacl.h"
#include "garnet_cienv.h"
#endif
#endif

#if !defined(GARNET_SHADER_REQUEST_CAPTURE_ONLY)
// ---------------------------------------------------------------------------------------------------------------
// AIR dump for the pipeline-creation overrides (cache miss -> /private/tmp/garnet-xlate/<sha256>.air).
// ---------------------------------------------------------------------------------------------------------------
GARNET_INTERNAL NSString *rdna4_sha256hex(NSData *d) {
    unsigned char h[CC_SHA256_DIGEST_LENGTH]; CC_SHA256(d.bytes, (CC_LONG)d.length, h);
    NSMutableString *s = [NSMutableString string];
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) [s appendFormat:@"%02x", h[i]];
    return s;
}

// RDNA4_ONCE: one log line per call site per process (no-op stubs are called thousands of times a second by the compositor; moved up in build 6 for the dump helpers)
// Bundle 9 (app crash study item 1): runs when Metal loads this bundle.  Core Image reflects a CIKL string kernel through [device newLibraryWithSource:] and asks the stitchable function
// for its arguments; that goes to -[_MTLDevice compiler] (nil here), the argument list stays empty and CIKernelReflection::consolidate faults (Preview, Adjust Color).  With the variable
// at 0 Core Image uses its own kernel-language parser instead.  An existing value (even "1") is never overwritten.  A process that calls kernelWithString before any Metal device exists
// has already read the flag: the arm script's `launchctl setenv` is the backstop for that.
__attribute__((constructor)) static void rdna4_cienv_ctor(void) {
    int r = rdna4ce_apply((rdna4ce_getenv_fn)getenv, setenv);
    if (r == RDNA4CE_SET) RDNA4_ONCE("cienv: set " RDNA4CE_VAR "=" RDNA4CE_VALUE " (Core Image takes its kernel-language parser, not the nil [device compiler])");
    else if (r == RDNA4CE_ALREADY_SET) RDNA4_ONCE("cienv: " RDNA4CE_VAR " is already set in this process (=%s); left alone", getenv(RDNA4CE_VAR));
    else RDNA4_ONCE_WARN("cienv: setenv " RDNA4CE_VAR " failed (errno %d)", errno);
}
// 0.0.641 (G4 review LOW) kept, bundle build 6 (C1 finding A): the dump area is /tmp/rdna4m (sticky 01777 parent, rdna4g_dumproot_ok) and every uid dumps into ITS OWN subdirectory /private/tmp/garnet-xlate/<euid> (0700, a
// real directory owned by the caller, rdna4g_dumpdir_ok). No chmod ever follows a symlink; any failed check skips the dump (and says so once per reason in the log). The translate daemon's
// account is granted list+search on the subdirectory by an ACL entry on the open directory descriptor (garnet_dumpacl.h); a failed grant is logged and the dump still happens.
GARNET_INTERNAL int rdna4_dump_dir_ready(char *sub, size_t subn) {
    if (!rdna4_sb_can_stat(RDNA4G_DUMP_DIR) || !rdna4_sb_can_write(RDNA4G_DUMP_DIR)) {
        RDNA4_ONCE("dump: " RDNA4G_DUMP_DIR " is blocked by sandbox: skipped");
        return 0;
    }
    (void)mkdir(RDNA4G_DUMP_DIR, RDNA4G_DUMP_ROOT_MODE);
    struct stat rs; memset(&rs, 0, sizeof rs);
    int rrc = lstat(RDNA4G_DUMP_DIR, &rs);
    if (rrc == 0 && rdna4g_dumproot_needs_fix((uint32_t)rs.st_uid, (uint32_t)geteuid(), (uint32_t)rs.st_mode) && S_ISDIR(rs.st_mode) && !S_ISLNK(rs.st_mode)) {
        (void)fchmodat(AT_FDCWD, RDNA4G_DUMP_DIR, RDNA4G_DUMP_ROOT_MODE, AT_SYMLINK_NOFOLLOW);   // ours but not sticky-1777 (the parent a pre-build-6 WindowServer made 0700): repaired, never through a link
        rrc = lstat(RDNA4G_DUMP_DIR, &rs);
    }
    if (!rdna4g_dumproot_ok(rrc, S_ISDIR(rs.st_mode), S_ISLNK(rs.st_mode), (uint32_t)rs.st_uid, (uint32_t)rs.st_mode, (uint32_t)geteuid())) {
        RDNA4_ONCE("dump: " RDNA4G_DUMP_DIR " is not usable as the dump parent (lstat rc %d, owner %u, mode %o, caller %u): skipped", rrc, (unsigned)rs.st_uid, (unsigned)(rs.st_mode & 07777), (unsigned)geteuid());
        return 0;
    }
    if (!rdna4g_dumpsub_path(sub, subn, (uint32_t)geteuid())) return 0;
    (void)mkdir(sub, 0700);
    struct stat st; memset(&st, 0, sizeof st);
    const int rc = lstat(sub, &st);
    if (!rdna4g_dumpdir_ok(rc, S_ISDIR(st.st_mode), S_ISLNK(st.st_mode), (uint32_t)st.st_uid, (uint32_t)geteuid())) {
        RDNA4_ONCE("dump: %s is not usable (lstat rc %d, owner %u, caller %u): skipped", sub, rc, (unsigned)st.st_uid, (unsigned)geteuid());
        return 0;
    }
    static _Atomic int aclDone;   // once per process: narrow + grant on a descriptor that is the directory lstat just described
    if (!atomic_load(&aclDone)) {
        const int fd = open(sub, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
        struct stat fs; memset(&fs, 0, sizeof fs);
        if (fd < 0 || fstat(fd, &fs) != 0 || fs.st_ino != st.st_ino || fs.st_dev != st.st_dev || (uint32_t)fs.st_uid != (uint32_t)geteuid()) {
            RDNA4LOG("dump: %s changed between the check and the open: skipped", sub);
            if (fd >= 0) close(fd);
            return 0;
        }
        if (rdna4g_dumpdir_needs_narrow((uint32_t)fs.st_mode)) (void)fchmod(fd, 0700);
        const int g = rdna4da_grant_list(fd, (uid_t)RDNA4G_DAEMON_UID);
        RDNA4LOG("dump: per-user directory %s ready (daemon list grant: %s)", sub, g == 1 ? "added" : g == 0 ? "already present" : "FAILED, the translate daemon may not see this directory");
        close(fd);
        atomic_store(&aclDone, 1);
    }
    return 1;
}

// A dump file appears under its final name only when complete and world-readable: written to a ".tmp" name (the daemon only looks at *.air), fchmod'ed 0644, renamed. 1 = written (or already there with this size).
GARNET_INTERNAL int rdna4_dump_write(const char *path, NSData *data) {
    struct stat ex;
    if (lstat(path, &ex) == 0 && S_ISREG(ex.st_mode) && (uint64_t)ex.st_size == (uint64_t)data.length) return 1;
    char tmp[1100];
    if (snprintf(tmp, sizeof tmp, "%s.tmpXXXXXX", path) >= (int)sizeof tmp) { errno = ENAMETOOLONG; return 0; }
    const int fd = mkstemp(tmp);   // O_EXCL: concurrent pipeline threads must not share a temporary inode.
    if (fd < 0) return 0;
    const char *p = data.bytes; size_t left = data.length; int failure = 0;
    while (left) {
        const ssize_t w = write(fd, p, left);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) { failure = w == 0 ? EIO : errno; break; }
        p += w; left -= (size_t)w;
    }
    if (!failure && fchmod(fd, 0644) != 0) failure = errno;
    if (close(fd) != 0 && !failure) failure = errno;
    if (failure || rename(tmp, path) != 0) {
        const int savedErrno = failure ? failure : errno;
        (void)unlink(tmp);
        errno = savedErrno;
        return 0;
    }
    return 1;
}

// Dumps one function's bitcodeData; appends the .air path to paths, or a problem note to notes.
GARNET_INTERNAL void rdna4_dump_function(id fn, const char *role, NSMutableArray *paths, NSMutableArray *notes) {
    if (!fn) { [notes addObject:[NSString stringWithFormat:@"%s function is nil", role]]; return; }
    SEL sel = NSSelectorFromString(@"bitcodeData");
    NSData *bc = nil;
    if ([fn respondsToSelector:sel]) bc = ((NSData *(*)(id, SEL))objc_msgSend)(fn, sel);
    NSString *name = [fn respondsToSelector:@selector(name)] ? [fn name] : @"?";
    NSInteger stage = [fn respondsToSelector:@selector(functionType)] ? (NSInteger)[fn functionType] : -1;
    RDNA4LOG("dump %s function '%s' class %s stage %ld bitcodeData %lu bytes", role,
           name.UTF8String, class_getName(object_getClass(fn)), (long)stage, (unsigned long)bc.length);
    if (!bc.length) { [notes addObject:[NSString stringWithFormat:@"%s '%@': no bitcodeData", role, name]]; return; }
    NSString *sha = rdna4_sha256hex(bc);
    char sub[64];
    if (!rdna4_dump_dir_ready(sub, sizeof sub)) { [notes addObject:[NSString stringWithFormat:@"%s function: " RDNA4G_DUMP_DIR " or its per-user subdirectory is not usable (a symlink, or owned by someone else): not dumped", role]]; return; }
    NSString *air = [NSString stringWithFormat:@"%s/%@.air", sub, sha];
    NSString *side = [NSString stringWithFormat:@"%s/%@.%s.json", sub, sha, role];
    NSDictionary *meta = @{ @"sha256": sha, @"function": name, @"role": @(role), @"stage": @(stage),
                            @"stageName": stage == 1 ? @"vertex" : stage == 2 ? @"fragment" : stage == 3 ? @"kernel" : @"other",
                            @"bytes": @(bc.length), @"air": air };
    NSData *js = [NSJSONSerialization dataWithJSONObject:meta options:NSJSONWritingPrettyPrinted error:NULL];
    // The daemon starts on the .air, so publish the sidecar first.
    if (!rdna4_dump_write(side.UTF8String, js)) {
        const int savedErrno = errno;
        [notes addObject:[NSString stringWithFormat:@"%s '%@': write to sidecar %s failed (errno %d: %s)", role, name, side.UTF8String, savedErrno, strerror(savedErrno)]];
        return;
    }
    if (!rdna4_dump_write(air.UTF8String, bc)) {
        const int savedErrno = errno;
        [notes addObject:[NSString stringWithFormat:@"%s '%@': write to AIR %s failed (errno %d: %s)", role, name, air.UTF8String, savedErrno, strerror(savedErrno)]];
        return;
    }
    [paths addObject:[NSString stringWithFormat:@"%@ (%s '%@' stage %ld, sidecar %@)", air, role, name, (long)stage, side]];
}

GARNET_INTERNAL NSError *rdna4_dump_pipeline(MTLRenderPipelineDescriptor *d, BOOL vertexMissing, BOOL fragmentMissing) {
    NSMutableArray *paths = [NSMutableArray array], *notes = [NSMutableArray array];
    if (vertexMissing) rdna4_dump_function(d.vertexFunction, "vertex", paths, notes);
    if (fragmentMissing && d.fragmentFunction) rdna4_dump_function(d.fragmentFunction, "fragment", paths, notes);
    NSString *status = paths.count ? @"AIR dumped for offline translation" : @"AIR dump did not produce a file";
    NSString *msg = [NSString stringWithFormat:@"GarnetMetal: pipeline not in spvcache; %@: %@%@",
                     status,
                     [paths componentsJoinedByString:@"; "],
                     notes.count ? [@" | problems: " stringByAppendingString:[notes componentsJoinedByString:@"; "]] : @""];
    RDNA4LOG("%s", msg.UTF8String);
    return rdna4_err(100, msg);
}


#endif // !defined(GARNET_SHADER_REQUEST_CAPTURE_ONLY)
#ifndef RDNA4_SHADER_CAPTURE_FLAG
#define RDNA4_SHADER_CAPTURE_FLAG "/private/tmp/garnet-shader-capture"
#endif
#ifdef GARNET_SHADER_REQUEST_CAPTURE_ONLY
#define rdna4_sb_lstat lstat
#define rdna4_sb_open_ro open
#endif
static int rdna4x_capture_request(NSString *directory, NSString *sha, NSString *name,
    const char *role, NSData *air, NSArray *dependencies) {
    struct stat flag;
    if (rdna4_sb_lstat(RDNA4_SHADER_CAPTURE_FLAG, &flag) || !S_ISREG(flag.st_mode) ||
        (flag.st_uid != 0 && flag.st_uid != geteuid()) || !directory || !sha || !air.length) return 0;
    char requested[65];
    int marker = rdna4_sb_open_ro(RDNA4_SHADER_CAPTURE_FLAG, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (marker < 0) return 0;
    ssize_t count = read(marker, requested, sizeof requested);
    close(marker);
    if (count != (ssize_t)sizeof requested || requested[64] != '\n' ||
        memcmp(requested, sha.UTF8String, 64)) return 0;
    NSString *path = [directory stringByAppendingPathComponent:[sha stringByAppendingString:@".capture.plist"]];
    if (lstat(path.fileSystemRepresentation, &flag) == 0) return 1;
    NSDictionary *request = @{@"sha256": sha, @"function": name, @"role": [NSString stringWithUTF8String:role],
        @"air": air, @"dependencies": dependencies ?: @[]};
    NSData *data = [NSPropertyListSerialization dataWithPropertyList:request
        format:NSPropertyListBinaryFormat_v1_0 options:0 error:NULL];
    if (!data) return -1;
    return rdna4_dump_write(path.fileSystemRepresentation, data) ? 1 : -1;
}
#if !defined(GARNET_SHADER_REQUEST_CAPTURE_ONLY)
// ---------------------------------------------------------------------------------------------------------------
// Bundle 14: in-process shader translation (driver design). The policy, the cache, the crash markers, the reader and the engine are in garnet_xlate.h (host tests test-xlate.c and
// test-xlate-corpus.sh run that very header); this is the glue. A spvcache miss in an ADMITTED APPLICATION (or an RDNA4M_ALLOW root tool with RDNA4M_TEST_INPROC=1) is translated inside the process by the
// operating system's own LLVM (the bitcode reader, loaded on the first miss only) and Resources/libgarnetxlate.dylib (the LGPL translator), and installed in the user's cache directory
// <confstr user cache dir>/org.garnet.xlate/<K>/. WindowServer NEVER applies: it keeps the precompiled spvcache plus the daemon and loads neither library.
//   /private/tmp/garnet-noinproc      exists (or cannot be read) -> in-process translation OFF for the process (read once); the dump + daemon path is then exactly bundle 13's
//   METAL2VULKAN_*                  any such variable in the environment -> OFF (several change the translator's output; the cache key does not cover them)
//   RDNA4M_TEST_INPROC=1              (root + RDNA4M_ALLOW=1) -> a root tool translates in process (retires the add-air.py seeding root probes needed)
//   RDNA4M_TEST_LLVM_LIB=<dylib>      (root + RDNA4M_ALLOW=1) -> that library is the bitcode reader (a second LLVM for host comparisons)
//   RDNA4M_TEST_USERCACHE=<dir>       (root + RDNA4M_ALLOW=1) -> replaces confstr(_CS_DARWIN_USER_CACHE_DIR) as the parent of org.garnet.xlate
// ---------------------------------------------------------------------------------------------------------------


// Exact C ABI match for third-party/metal2vulkan-rdna4/garnetxlate/garnetxlate.h Stage 2 & Stage 3 exports.

extern int sandbox_check(pid_t pid, const char *operation, int type, ...);
extern bool _dyld_get_shared_cache_uuid(uuid_t uuid);   // libdyld (dyld_priv.h): the UUID of the running dyld shared cache; it pins every library in it
GARNET_INTERNAL RDNA4XlateState RDNA4XL = { .mu = PTHREAD_MUTEX_INITIALIZER };
static NSMutableSet<NSString *> *rdna4x_logged, *rdna4x_touched;   // guarded by RDNA4XL.mu
static BOOL rdna4x_test_inproc(void) { const char *e = garnet_getenv("GARNET_TEST_INPROC", "RDNA4M_TEST_INPROC"); return rdna4_allow() && e && !strcmp(e, "1"); }
static void rdna4x_gate_read(void) {
    pthread_mutex_lock(&RDNA4XL.mu);
    if (!RDNA4XL.gateDone) {
        RDNA4XL.gateDone = 1;
        if (!rdna4_is_ws()) {   // WindowServer never reads these: it never applies
            struct stat st; const int rc = rdna4_sb_stat(RDNA4X_KILL_FILE, &st), er = errno;
            const int admitted = atomic_load(&rdna4_app_admitted);
            RDNA4XL.killed = rdna4x_killed_from_stat_for_app(rc, er, admitted);
            RDNA4XL.envDirty = rdna4x_env_dirty(*_NSGetEnviron());
            if (RDNA4XL.killed) RDNA4LOG_WARN("inproc: kill file " RDNA4X_KILL_FILE " %s: in-process translation is OFF for this process", rc == 0 ? "exists" : "could not be read (fail closed)");
            if (RDNA4XL.envDirty) RDNA4LOG("inproc: a " RDNA4X_ENV_PREFIX "* variable is set: in-process translation is OFF for this process");
        }
    }
    pthread_mutex_unlock(&RDNA4XL.mu);
}
// Does this process translate in process? (garnet_xlate.h rdna4x_applies; the admitted flag is set by rdna4_admit before any pipeline exists.)
GARNET_INTERNAL BOOL rdna4x_active(void) {
    rdna4x_gate_read();
    return rdna4x_applies(rdna4_is_ws(), atomic_load(&rdna4_app_admitted), rdna4_allow(), rdna4x_test_inproc(), RDNA4XL.killed, RDNA4XL.envDirty) ? YES : NO;
}
static NSString *rdna4x_resources(void) { return [[NSBundle bundleForClass:[GarnetDevice class]] resourcePath]; }
GARNET_INTERNAL NSString *rdna4x_xlib_path(void) { return [rdna4x_resources() stringByAppendingPathComponent:@RDNA4X_LIB_NAME]; }
GARNET_INTERNAL rdna4x_backend_capabilities rdna4_rt_xlate_caps(void) {
    rdna4x_backend_capabilities caps = rdna4_query_caps();
    if (RDNA4R.rtPositionFetch) caps.flags |= GARNETX_CAP_RT_POSITION_FETCH;
    caps.flags |= GARNETX_CAP_RT_IFT_ABI_V2;
    if (RDNA4R.accelStruct && RDNA4R.rayQuery && RDNA4R.bufferAddress) {
        caps.flags |= GARNETX_CAP_RT_PRIMITIVE_DATA;
        if (rdna4_rt_curves_enabled())
            caps.flags |= GARNETX_CAP_RT_CURVES;
        if (rdna4_rt_motion_enabled())
            caps.flags |= GARNETX_CAP_RT_MOTION_BLUR;
    }
    if (RDNA4R.shaderInt64)
        caps.flags |= GARNETX_CAP_NATIVE_INT64;
    return caps;
}
// Sets up Tier-0 <DARWIN_USER_CACHE_DIR>/org.garnet.xlate/<K>, falling back to Tier-1
// <DARWIN_USER_TEMP_DIR>/TemporaryItems/org.garnet.xlate/<K>, and finally Tier-2 bounded 64 MiB
// process-local memory cache (dirState == 2) for strictly read-only sandboxes like WebKit.GPU.
// Caller holds RDNA4XL.mu.
static void rdna4x_setup_locked(void) {
    uint8_t xl[16], rd[16]; memset(rd, 0, sizeof rd);
    if (!rdna4x_file_uuid(rdna4x_xlib_path().fileSystemRepresentation, xl)) { RDNA4XL.dirState = -1; RDNA4LOG_WARN("inproc: %s is missing or has no LC_UUID: in-process translation is unavailable", RDNA4X_LIB_NAME); return; }
    const char *rl = rdna4_allow() ? garnet_getenv("GARNET_TEST_LLVM_LIB", "RDNA4M_TEST_LLVM_LIB") : NULL;
    if (rl && rl[0]) { if (!rdna4x_file_uuid(rl, rd)) memset(rd, 0, sizeof rd); }
    else { uuid_t cu; if (_dyld_get_shared_cache_uuid(cu)) memcpy(rd, cu, 16); }   // the reader lives in the dyld shared cache (no file, not loaded yet): the cache's UUID pins it
    char osv[64] = ""; size_t osn = sizeof osv; if (sysctlbyname("kern.osversion", osv, &osn, NULL, 0) != 0) osv[0] = 0;
    uint8_t ov[CC_SHA256_DIGEST_LENGTH]; rdna4x_override_hash(ov);
    const rdna4x_backend_capabilities queryCaps = rdna4_rt_xlate_caps();
    char key[17]; rdna4x_key_with_capabilities(key, xl, rd, ov, osv, &queryCaps);
    char ucd[1024]; ucd[0] = 0;
    const char *tu = rdna4_allow() ? garnet_getenv("GARNET_TEST_USERCACHE", "RDNA4M_TEST_USERCACHE") : NULL;
    if (tu && tu[0]) snprintf(ucd, sizeof ucd, "%s", tu);
    else { const size_t cn = confstr(_CS_DARWIN_USER_CACHE_DIR, ucd, sizeof ucd); if (cn == 0 || cn > sizeof ucd) ucd[0] = 0; }
    char utd[1024]; utd[0] = 0;
    const char *tt = rdna4_allow() ? garnet_getenv("GARNET_TEST_USERTEMP", "RDNA4M_TEST_USERTEMP") : NULL;
    if (tt && tt[0]) snprintf(utd, sizeof utd, "%s", tt);
    else { const size_t tn = confstr(_CS_DARWIN_USER_TEMP_DIR, utd, sizeof utd); if (tn == 0 || tn > sizeof utd) utd[0] = 0; }
    const int tier = rdna4x_select_disk_cache(ucd, utd, key, RDNA4XL.dir, sizeof RDNA4XL.dir);
    if (tier == 1 || tier == 2) {
        RDNA4XL.dirNS = [NSString stringWithUTF8String:RDNA4XL.dir];
        RDNA4XL.dirState = 1;
        RDNA4LOG("inproc: enabled for %s (%s); cache %s (%s, key %s); reader: the OS's GPUCompiler libLLVM, loaded on the first miss",
                 getprogname(), rdna4_is_ws() ? "WindowServer?!" : rdna4_allow() ? "root test" : "application",
                 RDNA4XL.dir, tier == 2 ? "TemporaryItems fallback" : "user cache", key);
        return;
    }
    if (!RDNA4XL.memcacheInit) {
        rdna4x_memcache_init(&RDNA4XL.memcache, RDNA4X_MEMCACHE_CAP);
        RDNA4XL.memcacheInit = 1;
    }
    strlcpy(RDNA4XL.dir, RDNA4X_MEMCACHE_DIR, sizeof RDNA4XL.dir);
    RDNA4XL.dirNS = nil;
    RDNA4XL.dirState = 2;
    RDNA4LOG("inproc: enabled for %s (disk cache denied; using bounded %u MiB in-memory SPIR-V cache, key %s)",
             getprogname(), (unsigned)(RDNA4X_MEMCACHE_CAP >> 20), key);
}
static int rdna4x_ensure_setup(void) {
    if (!rdna4x_active()) return -1;
    if (!rdna4_radv_open(NULL)) return -1;
    pthread_mutex_lock(&RDNA4XL.mu);
    if (RDNA4XL.dirState == 0 ||
        (RDNA4XL.dirState == -1 && RDNA4XL.dirTries < 3 && rdna4_now() >= RDNA4XL.dirNextNs)) {
        RDNA4XL.dirTries++;
        rdna4x_setup_locked();
        if (RDNA4XL.dirState <= 0) RDNA4XL.dirNextNs = rdna4_now() + 2000000000ull;
    }
    const int st = RDNA4XL.dirState;
    pthread_mutex_unlock(&RDNA4XL.mu);
    return st;
}
// The user cache directory K, or nil (not applicable, unavailable, or Tier-2 memory-only mode).
GARNET_INTERNAL NSString *rdna4x_user_dir(void) {
    if (rdna4x_ensure_setup() != 1) return nil;
    pthread_mutex_lock(&RDNA4XL.mu);
    NSString *r = RDNA4XL.dirState == 1 ? RDNA4XL.dirNS : nil;
    pthread_mutex_unlock(&RDNA4XL.mu);
    return r;
}
GARNET_INTERNAL BOOL rdna4x_memcache_active(void) {
    return rdna4x_ensure_setup() == 2 ? YES : NO;
}
GARNET_INTERNAL BOOL rdna4x_memcache_lookup(NSString *sha, NSData **outSpv, NSDictionary **outMeta, NSString **outRefusal) {
    if (outSpv) *outSpv = nil;
    if (outMeta) *outMeta = nil;
    if (outRefusal) *outRefusal = nil;
    if (!sha.length || !RDNA4XL.memcacheInit) return NO;
    uint8_t *spvBuf = NULL, *metaBuf = NULL;
    size_t spvLen = 0, metaLen = 0;
    char errBuf[256] = "";
    const int hit = rdna4x_memcache_get(&RDNA4XL.memcache, sha.UTF8String,
                                        outSpv ? &spvBuf : NULL, &spvLen,
                                        outMeta ? &metaBuf : NULL, &metaLen,
                                        errBuf, sizeof errBuf);
    if (hit < 0) {
        if (outRefusal && errBuf[0]) *outRefusal = [NSString stringWithUTF8String:errBuf];
        return NO;
    }
    if (hit <= 0) return NO;
    if (outSpv && spvBuf && spvLen >= 20 && !(spvLen & 3))
        *outSpv = [NSData dataWithBytesNoCopy:spvBuf length:spvLen freeWhenDone:YES];
    else
        free(spvBuf);
    if (outMeta && metaBuf && metaLen) {
        id j = [NSJSONSerialization JSONObjectWithData:[NSData dataWithBytesNoCopy:metaBuf length:metaLen freeWhenDone:YES] options:0 error:NULL];
        if ([j isKindOfClass:[NSDictionary class]]) *outMeta = (NSDictionary *)j;
    } else {
        free(metaBuf);
    }
    return YES;
}
GARNET_INTERNAL BOOL rdna4x_memcache_store(NSString *sha, const void *spv, size_t spvLen, const void *meta, size_t metaLen) {
    if (!sha.length || !RDNA4XL.memcacheInit) return NO;
    return rdna4x_memcache_put(&RDNA4XL.memcache, sha.UTF8String, (const uint8_t *)spv, spvLen, (const uint8_t *)meta, metaLen) == 0 ? YES : NO;
}
// A translation that failed on I/O (the cache directory vanished: a sandboxed
// application's container cache is materialised lazily, another key's cleanup
// removes siblings, /private/var folders are purged) gets ONE more chance with
// the directory rebuilt. Without this, one transient I/O miss leaves the pipeline
// a permanent no-write placeholder: nothing retries the hot-swap watcher.
static BOOL rdna4x_dir_recheck(void) {
    if (!rdna4x_active() || !rdna4_radv_open(NULL)) return NO;
    pthread_mutex_lock(&RDNA4XL.mu);
    RDNA4XL.dirState = 0; RDNA4XL.dirTries = 0; RDNA4XL.dirNextNs = 0;
    rdna4x_setup_locked();
    const BOOL ok = RDNA4XL.dirState == 1 || RDNA4XL.dirState == 2;
    pthread_mutex_unlock(&RDNA4XL.mu);
    return ok;
}
static int rdna4x_sb_probe_fn(void *ctx) {
    (void)ctx;
    return sandbox_check(getpid(), NULL, 0);
}
GARNET_INTERNAL BOOL rdna4x_sandboxed(void) {
    pthread_mutex_lock(&RDNA4XL.mu);
    int next = 0;
    const int sb = rdna4g_sb_check_latch(RDNA4XL.sandboxedLatched, rdna4x_sb_probe_fn, NULL, &next);
    if (next) RDNA4XL.sandboxedLatched = 1;
    pthread_mutex_unlock(&RDNA4XL.mu);
    return sb != 0;
}
// Unsandboxed cache hits refresh modification time once per sha per process.
GARNET_INTERNAL void rdna4x_touch_hit(NSString *path, NSString *sha) {
    NSString *u = RDNA4XL.dirNS;
    if (!u || ![path hasPrefix:u] || rdna4x_sandboxed()) return;
    pthread_mutex_lock(&RDNA4XL.mu);
    if (!rdna4x_touched) rdna4x_touched = [NSMutableSet set];
    const BOOL first = ![rdna4x_touched containsObject:sha]; if (first) [rdna4x_touched addObject:sha];
    pthread_mutex_unlock(&RDNA4XL.mu);
    if (first) { rdna4x_touch(path.fileSystemRepresentation); if ([path hasSuffix:@".spv"]) { NSString *m = [[path stringByDeletingPathExtension] stringByAppendingString:@".meta.json"]; rdna4x_touch(m.fileSystemRepresentation); } }
}
// ---- build 18 (P3): linked functions (linked functions) ----
// A pipeline stage whose descriptor links functions (MTLLinkedFunctions.functions / privateFunctions / groups) is translated TOGETHER with them: garnetxlate's garnetx_translate_linked resolves the entry's direct
// visible-function references (RenderBox's custom effects, Maps' magenta fragment) to the exact authored dependencies. The cache key is rdna4x_link_sha (entry + sorted dependencies), never the entry's sha alone.
// The context is per THREAD and per creation call (thread-local, cleared by a cleanup scope): the lookups, the translation request and the hot-swap registration all run on the thread that creates the
// pipeline, keyed by the entry function object, so no signature changed and a function linked differently in another descriptor never sees this one's key.
@implementation RDNA4LinkCtx @end
static __thread RDNA4LinkCtx * __unsafe_unretained rdna4_tl_lk[3];   // 0 vertex, 1 fragment, 2 kernel
static __thread void *rdna4_tl_keep[3];                             // the +1 that keeps rdna4_tl_lk[i] alive until it is replaced or the scope ends
GARNET_INTERNAL void rdna4_lk_install(int slot, RDNA4LinkCtx *c) {
    if (rdna4_tl_keep[slot]) { CFRelease(rdna4_tl_keep[slot]); rdna4_tl_keep[slot] = NULL; }
    rdna4_tl_lk[slot] = nil;
    if (c) { rdna4_tl_keep[slot] = (void *)CFBridgingRetain(c); rdna4_tl_lk[slot] = c; }
}
GARNET_INTERNAL void rdna4_lk_scope_end(rdna4_lkscope *sc) { (void)sc; for (int i = 0; i < 3; i++) rdna4_lk_install(i, nil); }
GARNET_INTERNAL RDNA4LinkCtx *rdna4_lk_for(id fn) { if (!fn) return nil; for (int i = 0; i < 3; i++) if (rdna4_tl_lk[i] && rdna4_tl_lk[i]->fn == fn) return rdna4_tl_lk[i]; return nil; }
__attribute__((unused)) GARNET_INTERNAL NSError *rdna4_lk_error(id fn) { RDNA4LinkCtx *lk = rdna4_lk_for(fn); return lk ? lk->linkageError : nil; }

static struct {
    pthread_mutex_t mu;
    rdna4g_sync state;
    unsigned waiters;
    BOOL exclusive;
} RDNA4SYNC = { .mu = PTHREAD_MUTEX_INITIALIZER };
GARNET_INTERNAL BOOL rdna4_sync_begin(BOOL share) {
    pthread_mutex_lock(&RDNA4SYNC.mu);
    const BOOL slot = !RDNA4SYNC.waiters || (share && !RDNA4SYNC.exclusive && RDNA4SYNC.waiters < 3);
    const BOOL allowed = slot && rdna4g_sync_allowed(&RDNA4SYNC.state);
    if (allowed) {
        RDNA4SYNC.waiters++;
        RDNA4SYNC.exclusive = !share;
    }
    pthread_mutex_unlock(&RDNA4SYNC.mu);
    return allowed;
}
GARNET_INTERNAL void rdna4_sync_end(BOOL found, BOOL timedout) {
    pthread_mutex_lock(&RDNA4SYNC.mu);
    if (found || timedout) rdna4g_sync_result(&RDNA4SYNC.state, found);
    RDNA4SYNC.waiters--;
    pthread_mutex_unlock(&RDNA4SYNC.mu);
}
GARNET_INTERNAL void rdna4_sync_note_async_success(void) {
    if (!rdna4_is_ws()) return;
    pthread_mutex_lock(&RDNA4SYNC.mu);
    rdna4g_sync_result(&RDNA4SYNC.state, 1);
    pthread_mutex_unlock(&RDNA4SYNC.mu);
}

static void rdna4x_translation_error(NSError **error, const char *role, NSString *name,
                                     NSString *sha, int code, NSString *reason) {
    if (!error) return;
    *error = rdna4_err(code, [NSString stringWithFormat:
        @"in-process translation failed for %s '%@' %@: code %d: %@",
        role, name.length ? name : @"?", sha.length ? sha : @"<no sha>", code,
        reason.length ? reason : @"no failure reason was reported"]);
}

GARNET_INTERNAL NSString *rdna4x_cached_refusal(NSString *dir, NSString *sha) {
    char path[RDNA4X_PATH_MAX];
    if (!rdna4x_file_path(path, sizeof path, dir.fileSystemRepresentation, sha.UTF8String, RDNA4X_EXT_FAIL)) return nil;
    const int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return nil;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) { close(fd); return nil; }
    char bytes[513]; size_t length = 0;
    while (length < sizeof bytes - 1) {
        const ssize_t n = read(fd, bytes + length, sizeof bytes - 1 - length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        length += (size_t)n;
    }
    close(fd);
    if (!length) return nil;
    for (size_t i = 0; i < length; ++i)
        if ((unsigned char)bytes[i] < 0x20 || (unsigned char)bytes[i] > 0x7e) bytes[i] = ' ';
    while (length && bytes[length - 1] == ' ') --length;
    if (!length) return nil;
    bytes[length] = 0;
    NSString *reason = [NSString stringWithUTF8String:bytes];
    return reason.length ? reason : nil;
}


GARNET_INTERNAL void rdna4x_engine_prepare(void) {
    pthread_mutex_lock(&RDNA4XL.mu);
    if (!RDNA4XL.memcacheInit) {
        rdna4x_memcache_init(&RDNA4XL.memcache, RDNA4X_MEMCACHE_CAP);
        RDNA4XL.memcacheInit = 1;
    }
    if (!RDNA4XL.engineInit) {
        const char *rl = rdna4_allow() ? garnet_getenv("GARNET_TEST_LLVM_LIB", "RDNA4M_TEST_LLVM_LIB") : NULL;
        rdna4x_engine_init(&RDNA4XL.eng, NULL, NULL, rl, rdna4x_xlib_path().fileSystemRepresentation, RDNA4X_WORKER_STACK);
        RDNA4XL.eng.lowerTranslatedArguments = RDNA4R.gpuArgumentTables;
        RDNA4XL.eng.capabilities = rdna4_rt_xlate_caps();
        rdna4x_engine_set_memcache(&RDNA4XL.eng, &RDNA4XL.memcache);
        RDNA4XL.engineInit = 1;
    }
    pthread_mutex_unlock(&RDNA4XL.mu);
}

// Lazy translator library access for ICB render stage lowering and Metal 3 Stage 2 linking.
GARNET_INTERNAL const rdna4x_xlib *rdna4x_ensure_xlib_raw(NSError **err) {
    pthread_mutex_lock(&RDNA4XL.mu);
    if (!RDNA4XL.icbXlib.h && !RDNA4XL.icbXlibTried) {
        char ebuf[256] = "";
        if (rdna4x_xlib_load(&RDNA4XL.icbXlib, rdna4x_xlib_path().fileSystemRepresentation, ebuf, sizeof ebuf) != 0) {
            strlcpy(RDNA4XL.icbXlibErr, ebuf[0] ? ebuf : "failed to load libgarnetxlate.dylib", sizeof RDNA4XL.icbXlibErr);
            memset(&RDNA4XL.icbXlib, 0, sizeof RDNA4XL.icbXlib);
        } else {
            *(void **)&RDNA4XL.detectStage = dlsym(RDNA4XL.icbXlib.h, "garnetx_detect_stage");
            *(void **)&RDNA4XL.listExportedSymbols = dlsym(RDNA4XL.icbXlib.h, "garnetx_list_exported_symbols");
            *(void **)&RDNA4XL.translateLinkedLibraries = dlsym(RDNA4XL.icbXlib.h, "garnetx_translate_linked_libraries");
            *(void **)&RDNA4XL.translateStage3 = dlsym(RDNA4XL.icbXlib.h, "garnetx_translate_stage3");
        }
        RDNA4XL.icbXlibTried = 1;
    }
    const rdna4x_xlib *out = RDNA4XL.icbXlib.h ? &RDNA4XL.icbXlib : NULL;
    if (!out && err) {
        *err = rdna4_err(42, [NSString stringWithUTF8String:RDNA4XL.icbXlibErr[0] ? RDNA4XL.icbXlibErr : "libgarnetxlate.dylib unavailable"]);
    }
    pthread_mutex_unlock(&RDNA4XL.mu);
    return out;
}

GARNET_INTERNAL const rdna4x_xlib *rdna4x_ensure_xlib(NSError **err) {
    const rdna4x_xlib *out = rdna4x_ensure_xlib_raw(err);
    if (out && !out->lower_icb_stage) {
        if (err) *err = rdna4_err(42, @"libgarnetxlate.dylib lacks garnetx_lower_icb_stage");
        return NULL;
    }
    return out;
}

static BOOL rdna4x_ensure_stage2_xlib(const rdna4x_xlib **xlibOut,
                                      rdna4x_list_exported_symbols_fn *listOut,
                                      rdna4x_translate_linked_libraries_fn *translateOut,
                                      NSError **err) {
    const rdna4x_xlib *xl = rdna4x_ensure_xlib_raw(err);
    if (!xl) return NO;
    pthread_mutex_lock(&RDNA4XL.mu);
    rdna4x_list_exported_symbols_fn listFn = RDNA4XL.listExportedSymbols;
    rdna4x_translate_linked_libraries_fn xlatFn = RDNA4XL.translateLinkedLibraries;
    pthread_mutex_unlock(&RDNA4XL.mu);
    if ((listOut && !listFn) || (translateOut && !xlatFn)) {
        if (err) *err = rdna4_err(42, @"libgarnetxlate.dylib lacks Metal 3 Stage 2 dynamic library / VFT entry points");
        return NO;
    }
    if (xlibOut) *xlibOut = xl;
    if (listOut) *listOut = listFn;
    if (translateOut) *translateOut = xlatFn;
    return YES;
}

GARNET_INTERNAL BOOL rdna4x_stage2_symbols_available(void) {
    const rdna4x_xlib *xl = NULL;
    rdna4x_list_exported_symbols_fn listFn = NULL;
    rdna4x_translate_linked_libraries_fn xlatFn = NULL;
    return rdna4x_ensure_stage2_xlib(&xl, &listFn, &xlatFn, NULL);
}

// Resolves garnetx_translate_stage3 from libgarnetxlate.dylib without opening RADV.
__attribute__((unused)) GARNET_INTERNAL BOOL rdna4x_ensure_stage3_xlib(
    const rdna4x_xlib **xlibOut,
    rdna4x_translate_stage3_fn *translateOut,
    NSError **err)
{
    const rdna4x_xlib *xl = rdna4x_ensure_xlib_raw(err);
    if (!xl) return NO;
    pthread_mutex_lock(&RDNA4XL.mu);
    rdna4x_translate_stage3_fn fn = RDNA4XL.translateStage3;
    pthread_mutex_unlock(&RDNA4XL.mu);
    if (translateOut && !fn) {
        if (err) *err = rdna4_err(42, @"libgarnetxlate.dylib lacks garnetx_translate_stage3 (Stage 3 RT/IFT/Mesh)");
        return NO;
    }
    if (xlibOut) *xlibOut = xl;
    if (translateOut) *translateOut = fn;
    return fn != NULL;
}

__attribute__((unused)) static BOOL rdna4x_ensure_detect_stage_xlib(
    const rdna4x_xlib **xlibOut,
    rdna4x_detect_stage_fn *detectOut,
    NSError **err)
{
    const rdna4x_xlib *xl = rdna4x_ensure_xlib_raw(err);
    if (!xl) return NO;
    pthread_mutex_lock(&RDNA4XL.mu);
    rdna4x_detect_stage_fn fn = RDNA4XL.detectStage;
    pthread_mutex_unlock(&RDNA4XL.mu);
    if (detectOut && !fn) {
        if (err) *err = rdna4_err(42, @"libgarnetxlate.dylib lacks garnetx_detect_stage");
        return NO;
    }
    if (xlibOut) *xlibOut = xl;
    if (detectOut) *detectOut = fn;
    return fn != NULL;
}

__attribute__((unused)) static BOOL rdna4x_stage3_symbols_available(void) {
    const rdna4x_xlib *xl = NULL;
    rdna4x_translate_stage3_fn xlatFn = NULL;
    rdna4x_detect_stage_fn detFn = NULL;
    return rdna4x_ensure_stage3_xlib(&xl, &xlatFn, NULL) &&
           rdna4x_ensure_detect_stage_xlib(NULL, &detFn, NULL);
}

static const rdna4x_reader *rdna4x_ensure_stage2_reader(char *errBuf, size_t errCap) {
    pthread_mutex_lock(&RDNA4XL.mu);
    if (!RDNA4XL.stage2Reader.h && !RDNA4XL.stage2ReaderTried) {
        const char *rl = rdna4_allow() ? garnet_getenv("GARNET_TEST_LLVM_LIB", "RDNA4M_TEST_LLVM_LIB") : NULL;
        char ebuf[256] = "";
        if (rdna4x_reader_load(&RDNA4XL.stage2Reader, RDNA4X_GPUC_BASE, (rl && rl[0]) ? rl : NULL, ebuf, sizeof ebuf) != 0) {
            strlcpy(RDNA4XL.stage2ReaderErr, ebuf[0] ? ebuf : "failed to load GPUCompiler LLVM bitcode reader",
                    sizeof RDNA4XL.stage2ReaderErr);
            memset(&RDNA4XL.stage2Reader, 0, sizeof RDNA4XL.stage2Reader);
        }
        RDNA4XL.stage2ReaderTried = 1;
    }
    const rdna4x_reader *out = RDNA4XL.stage2Reader.h ? &RDNA4XL.stage2Reader : NULL;
    if (!out && errBuf && errCap)
        snprintf(errBuf, errCap, "%s", RDNA4XL.stage2ReaderErr[0] ? RDNA4XL.stage2ReaderErr : "LLVM bitcode reader unavailable");
    pthread_mutex_unlock(&RDNA4XL.mu);
    return out;
}

// Run LLVM bitcode parsing / metal2vulkan Stage 2 translation on a 64 MiB stack so async GCD callers never overflow.
GARNET_INTERNAL int rdna4x_run_on_worker_stack(void *(*fn)(void *), void *arg) {
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) return -1;
    int rc = pthread_attr_setstacksize(&attr, RDNA4X_WORKER_STACK);
    pthread_t th;
    if (rc == 0) rc = pthread_create(&th, &attr, fn, arg);
    pthread_attr_destroy(&attr);
    if (rc != 0) return -1;
    return pthread_join(th, NULL) == 0 ? 0 : -1;
}

GARNET_INTERNAL int rdna4x_module_to_llvm_text(const uint8_t *bytes, size_t len, uint32_t payloadKind,
                                      char **outText, size_t *outLen, char *errBuf, size_t errCap) {
    *outText = NULL;
    *outLen = 0;
    if (!bytes || len == 0) {
        if (errBuf && errCap) snprintf(errBuf, errCap, "empty shader/library module payload");
        return RDNA4X_E_CAP;
    }
    if (payloadKind == RDNA4DL_PAYLOAD_LLVM_IR) {
        if (!rdna4x_ll_size_ok(len)) {
            if (errBuf && errCap) snprintf(errBuf, errCap, "LLVM IR module (%zu bytes) outside cap", len);
            return RDNA4X_E_CAP;
        }
        char *copy = (char *)malloc(len + 1u);
        if (!copy) {
            if (errBuf && errCap) snprintf(errBuf, errCap, "out of memory copying LLVM IR module");
            return RDNA4X_E_IO;
        }
        memcpy(copy, bytes, len);
        copy[len] = '\0';
        *outText = copy;
        *outLen = len;
        return RDNA4X_OK;
    }
    if (payloadKind != RDNA4DL_PAYLOAD_AIR_BITCODE) {
        if (errBuf && errCap) snprintf(errBuf, errCap, "unsupported dynamic library payload kind %u (expected AIR bitcode or LLVM IR)", payloadKind);
        return RDNA4X_E_REFUSED;
    }
    if (!rdna4x_air_size_ok(len)) {
        if (errBuf && errCap) snprintf(errBuf, errCap, "AIR bitcode module (%zu bytes) outside cap", len);
        return RDNA4X_E_CAP;
    }
    const rdna4x_reader *rd = rdna4x_ensure_stage2_reader(errBuf, errCap);
    if (!rd) return RDNA4X_E_UNAVAILABLE;
    if (rdna4x_reader_print(rd, bytes, len, outText, outLen, errBuf, errCap) != 0 || !*outText)
        return RDNA4X_E_READER;
    if (!rdna4x_ll_size_ok(*outLen)) {
        free(*outText); *outText = NULL; *outLen = 0;
        if (errBuf && errCap) snprintf(errBuf, errCap, "disassembled LLVM IR text outside cap");
        return RDNA4X_E_CAP;
    }
    return RDNA4X_OK;
}

typedef struct {
    const rdna4x_xlib *xlib;
    rdna4x_list_exported_symbols_fn listFn;
    const uint8_t *bytes;
    size_t len;
    uint32_t payloadKind;
    uint8_t *jsonOut;
    size_t jsonLen;
    int rc;
    char err[512];
} rdna4x_list_syms_work_t;

static void *rdna4x_list_syms_worker(void *opaque) {
    rdna4x_list_syms_work_t *w = (rdna4x_list_syms_work_t *)opaque;
    char *ll = NULL;
    size_t llLen = 0;
    int mrc = rdna4x_module_to_llvm_text(w->bytes, w->len, w->payloadKind, &ll, &llLen, w->err, sizeof w->err);
    if (mrc != RDNA4X_OK) {
        w->rc = mrc;
        return NULL;
    }
    int frc = w->listFn((const uint8_t *)ll, llLen, &w->jsonOut, &w->jsonLen, w->err, sizeof w->err);
    free(ll);
    if (frc != 0) {
        if (!w->err[0]) snprintf(w->err, sizeof w->err, "garnetx_list_exported_symbols returned %d", frc);
        w->rc = RDNA4X_E_REFUSED;
        return NULL;
    }
    w->rc = RDNA4X_OK;
    return NULL;
}

GARNET_INTERNAL BOOL rdna4x_list_exported_symbols_for_module(const uint8_t *bytes, size_t len, uint32_t payloadKind,
                                                    NSArray<NSString *> **outSymbols, NSError **err) {
    if (outSymbols) *outSymbols = nil;
    if (err) *err = nil;
    const rdna4x_xlib *xl = NULL;
    rdna4x_list_exported_symbols_fn listFn = NULL;
    if (!rdna4x_ensure_stage2_xlib(&xl, &listFn, NULL, err)) return NO;

    rdna4x_list_syms_work_t work;
    memset(&work, 0, sizeof work);
    work.xlib = xl;
    work.listFn = listFn;
    work.bytes = bytes;
    work.len = len;
    work.payloadKind = payloadKind;
    if (rdna4x_run_on_worker_stack(rdna4x_list_syms_worker, &work) != 0) {
        if (err) *err = rdna4_err(42, @"failed to spawn worker thread for dynamic library symbol enumeration");
        return NO;
    }
    if (work.rc != RDNA4X_OK || !work.jsonOut || work.jsonLen == 0) {
        if (work.jsonOut) xl->freep(work.jsonOut, work.jsonLen);
        if (err) *err = rdna4_err(42, [NSString stringWithUTF8String:work.err[0] ? work.err : "failed to enumerate dynamic library symbols"]);
        return NO;
    }
    rdna4dl_container_t parsed;
    rdna4dl_init(&parsed);
    rdna4dl_status_t pst = rdna4dl_parse_symbols_json((const char *)work.jsonOut, work.jsonLen, &parsed);
    xl->freep(work.jsonOut, work.jsonLen);
    if (pst != RDNA4DL_OK) {
        rdna4dl_destroy(&parsed);
        if (err) *err = rdna4_err(42, [NSString stringWithFormat:@"invalid exported symbols JSON: %s", rdna4dl_status_str(pst)]);
        return NO;
    }
    NSMutableArray<NSString *> *syms = [NSMutableArray arrayWithCapacity:parsed.symbol_count];
    for (uint32_t i = 0; i < parsed.symbol_count; i++) {
        NSString *s = [NSString stringWithUTF8String:parsed.symbols[i]];
        if (s) [syms addObject:s];
    }
    rdna4dl_destroy(&parsed);
    if (outSymbols) *outSymbols = syms;
    return YES;
}

__attribute__((unused)) GARNET_INTERNAL BOOL rdna4x_parse_vft_from_meta(NSDictionary *meta, rdna4vft_reflection_t *outVft, NSError **err) {
    if (!outVft) return YES;
    rdna4vft_refl_init(outVft);
    if (!meta || !meta[@"visible_function_tables"] || meta[@"visible_function_tables"] == [NSNull null])
        return YES;
    NSDictionary *wrapper = @{ @"visible_function_tables": meta[@"visible_function_tables"] };
    if (![NSJSONSerialization isValidJSONObject:wrapper]) {
        if (err) *err = rdna4_err(49, @"visible_function_tables reflection metadata is not JSON-serializable");
        return NO;
    }
    NSError *je = nil;
    NSData *raw = [NSJSONSerialization dataWithJSONObject:wrapper options:0 error:&je];
    if (!raw.length) {
        if (err) *err = rdna4_err(49, je.localizedDescription ?: @"failed to serialize visible_function_tables metadata");
        return NO;
    }
    rdna4vft_status_t st = rdna4vft_parse_reflection_json((const char *)raw.bytes, raw.length, outVft);
    if (st != RDNA4VFT_OK) {
        rdna4vft_refl_destroy(outVft);
        if (err) *err = rdna4_err(49, [NSString stringWithFormat:@"visible_function_tables metadata invalid: %s", rdna4vft_status_str(st)]);
        return NO;
    }
    return YES;
}

typedef struct {
    const rdna4x_xlib *xlib;
    rdna4x_detect_stage_fn detectFn;
    const uint8_t *bytes;
    size_t len;
    uint32_t payloadKind;
    uint32_t stageKind;
    int rc;
    char err[512];
} rdna4x_detect_stage_work_t;

static void *rdna4x_detect_stage_worker(void *opaque) {
    rdna4x_detect_stage_work_t *w = (rdna4x_detect_stage_work_t *)opaque;
    char *ll = NULL;
    size_t llLen = 0;
    int mrc = rdna4x_module_to_llvm_text(w->bytes, w->len, w->payloadKind, &ll, &llLen, w->err, sizeof w->err);
    if (mrc != RDNA4X_OK) {
        w->rc = mrc;
        return NULL;
    }
    int drc = w->detectFn((const uint8_t *)ll, llLen, &w->stageKind, w->err, sizeof w->err);
    free(ll);
    if (drc != 0) {
        if (!w->err[0]) snprintf(w->err, sizeof w->err, "garnetx_detect_stage returned %d", drc);
        w->rc = RDNA4X_E_REFUSED;
        return NULL;
    }
    w->rc = RDNA4X_OK;
    return NULL;
}

__attribute__((unused)) static BOOL rdna4x_detect_stage_for_module(const uint8_t *bytes, size_t len, uint32_t payloadKind,
                                                                   uint32_t *outStageKind, NSError **err) {
    if (outStageKind) *outStageKind = 0u;
    if (err) *err = nil;
    const rdna4x_xlib *xl = NULL;
    rdna4x_detect_stage_fn detectFn = NULL;
    if (!rdna4x_ensure_detect_stage_xlib(&xl, &detectFn, err)) return NO;

    rdna4x_detect_stage_work_t work;
    memset(&work, 0, sizeof work);
    work.xlib = xl;
    work.detectFn = detectFn;
    work.bytes = bytes;
    work.len = len;
    work.payloadKind = payloadKind;
    if (rdna4x_run_on_worker_stack(rdna4x_detect_stage_worker, &work) != 0) {
        if (err) *err = rdna4_err(42, @"failed to spawn worker thread for stage detection");
        return NO;
    }
    if (work.rc != RDNA4X_OK) {
        if (err) *err = rdna4_err(42, [NSString stringWithUTF8String:work.err[0] ? work.err : "failed to detect shader stage"]);
        return NO;
    }
    if (outStageKind) *outStageKind = work.stageKind;
    return YES;
}


typedef struct {
    const rdna4x_xlib *xlib;
    rdna4x_translate_linked_libraries_fn translateFn;
    rdna4x_translate_stage3_fn translateStage3Fn;
    const char *dir;
    const char *sha;
    const char *fname;
    const uint8_t *entryAir;
    size_t entryAirLen;
    const rdna4x_stage2_in_t *deps;
    size_t ndeps;
    const rdna4x_stage2_in_t *dynlibs;
    size_t ndynlibs;
    const rdna4x_stage2_in_t *vis;
    size_t nvis;
    const rdna4x_stage2_in_t *ift;
    size_t nift;
    int useStage3;
    rdna4x_backend_capabilities caps;
    int lowerArgs;
    uint8_t *outSpv;
    size_t outSpvLen;
    uint8_t *outMeta;
    size_t outMetaLen;
    int rc;
    char err[512];
} rdna4x_stage2_work_t;

static void *rdna4x_stage2_worker(void *opaque) {
    rdna4x_stage2_work_t *w = (rdna4x_stage2_work_t *)opaque;
    char path[RDNA4X_PATH_MAX], failPath[RDNA4X_PATH_MAX];
    path[0] = 0; failPath[0] = 0;

    if (!w->entryAir || !rdna4x_air_size_ok(w->entryAirLen)) {
        snprintf(w->err, sizeof w->err, "entry function bitcode size outside caps");
        w->rc = RDNA4X_E_CAP;
        return NULL;
    }
    if (!rdna4x_sha_ok(w->sha) || !w->dir || !w->dir[0]) {
        snprintf(w->err, sizeof w->err, "bad Stage 2 cache sha or directory");
        w->rc = RDNA4X_E_IO;
        return NULL;
    }
    if (w->ndeps > RDNA4X_MAX_DEPS || w->ndynlibs > RDNA4X_MAX_DEPS || w->nvis > RDNA4X_MAX_DEPS || w->nift > RDNA4X_MAX_DEPS) {
        snprintf(w->err, sizeof w->err, "Stage 2/3 linkage count exceeds cap (%u)", RDNA4X_MAX_DEPS);
        w->rc = RDNA4X_E_CAP;
        return NULL;
    }

    const int memOnly = rdna4x_is_mem_dir(w->dir);
    if (!memOnly) {
        switch (rdna4x_probe(w->dir, w->sha)) {
        case RDNA4X_PL_SKIP_CRASH:
        case RDNA4X_PL_PROMOTE_THEN_SKIP:
            snprintf(w->err, sizeof w->err, "a Stage 2 translation of this shader crashed a process before (.crash)");
            w->rc = RDNA4X_E_SKIP_CRASH;
            return NULL;
        case RDNA4X_PL_SKIP_FAIL:
            snprintf(w->err, sizeof w->err, "the Stage 2 translator refused this shader before (.fail)");
            w->rc = RDNA4X_E_SKIP_FAIL;
            return NULL;
        case RDNA4X_PL_SKIP_LIVE:
            snprintf(w->err, sizeof w->err, "another process is translating this Stage 2 shader");
            w->rc = RDNA4X_E_SKIP_LIVE;
            return NULL;
        default:
            break;
        }

        int mk = rdna4x_marker_create(w->dir, w->sha);
        if (mk < 0) {
            char parent[RDNA4X_PATH_MAX], key[17];
            const char *slash = strrchr(w->dir, '/');
            const size_t pl = slash ? (size_t)(slash - w->dir) : 0;
            const size_t kl = slash ? strlen(slash + 1) : 0;
            if (slash && pl > 0 && pl < sizeof parent && kl == 16 && kl < sizeof key) {
                memcpy(parent, w->dir, pl); parent[pl] = 0;
                memcpy(key, slash + 1, kl + 1);
                if (rdna4x_key_ok(key) && rdna4x_prepare_dirs(parent, w->dir, key) == 0)
                    mk = rdna4x_marker_create(w->dir, w->sha);
            }
        }
        if (mk == 1) {
            snprintf(w->err, sizeof w->err, "another process is translating this Stage 2 shader");
            w->rc = RDNA4X_E_SKIP_LIVE;
            return NULL;
        }
        if (mk != 0) {
            snprintf(w->err, sizeof w->err, "cannot create Stage 2 crash marker in %s: %s", w->dir, strerror(mk < 0 ? -mk : EIO));
            w->rc = RDNA4X_E_IO;
            return NULL;
        }
        (void)rdna4x_file_path(failPath, sizeof failPath, w->dir, w->sha, RDNA4X_EXT_FAIL);
    }

    char *entryLl = NULL; size_t entryLlLen = 0;
    char **depTexts = NULL, **libTexts = NULL, **visTexts = NULL, **iftTexts = NULL;
    rdna4x_cdep *cdeps = NULL, *cvis = NULL;
    rdna4x_cdynlib *clibs = NULL;
    rdna4x_cift_fn *cift = NULL;
    uint8_t *spv = NULL, *meta = NULL;
    size_t spvLen = 0, metaLen = 0;
    int rc = RDNA4X_OK;
    size_t totalLl = 0;

    rc = rdna4x_module_to_llvm_text(w->entryAir, w->entryAirLen, RDNA4DL_PAYLOAD_AIR_BITCODE,
                                    &entryLl, &entryLlLen, w->err, sizeof w->err);
    if (rc != RDNA4X_OK) goto refused;
    totalLl = entryLlLen;

    if (w->ndeps) {
        depTexts = (char **)calloc(w->ndeps, sizeof *depTexts);
        cdeps = (rdna4x_cdep *)calloc(w->ndeps, sizeof *cdeps);
        if (!depTexts || !cdeps) { snprintf(w->err, sizeof w->err, "out of memory"); rc = RDNA4X_E_IO; goto done; }
        for (size_t i = 0; i < w->ndeps; i++) {
            size_t tlen = 0;
            rc = rdna4x_module_to_llvm_text(w->deps[i].bytes, w->deps[i].len, w->deps[i].kind,
                                            &depTexts[i], &tlen, w->err, sizeof w->err);
            if (rc != RDNA4X_OK) goto refused;
            totalLl += tlen;
            if (totalLl > RDNA4X_MAX_LL) {
                snprintf(w->err, sizeof w->err, "Stage 2 linked LLVM IR exceeds cap");
                rc = RDNA4X_E_CAP;
                goto refused;
            }
            cdeps[i].symbol = (const uint8_t *)w->deps[i].name;
            cdeps[i].symbol_len = strlen(w->deps[i].name);
            cdeps[i].ll = (const uint8_t *)depTexts[i];
            cdeps[i].ll_len = tlen;
        }
    }
    if (w->ndynlibs) {
        libTexts = (char **)calloc(w->ndynlibs, sizeof *libTexts);
        clibs = (rdna4x_cdynlib *)calloc(w->ndynlibs, sizeof *clibs);
        if (!libTexts || !clibs) { snprintf(w->err, sizeof w->err, "out of memory"); rc = RDNA4X_E_IO; goto done; }
        for (size_t i = 0; i < w->ndynlibs; i++) {
            size_t tlen = 0;
            rc = rdna4x_module_to_llvm_text(w->dynlibs[i].bytes, w->dynlibs[i].len, w->dynlibs[i].kind,
                                            &libTexts[i], &tlen, w->err, sizeof w->err);
            if (rc != RDNA4X_OK) goto refused;
            totalLl += tlen;
            if (totalLl > RDNA4X_MAX_LL) {
                snprintf(w->err, sizeof w->err, "Stage 2 dynamic library LLVM IR exceeds cap");
                rc = RDNA4X_E_CAP;
                goto refused;
            }
            clibs[i].name = (const uint8_t *)w->dynlibs[i].name;
            clibs[i].name_len = strlen(w->dynlibs[i].name);
            clibs[i].ll = (const uint8_t *)libTexts[i];
            clibs[i].ll_len = tlen;
        }
    }
    if (w->nvis) {
        visTexts = (char **)calloc(w->nvis, sizeof *visTexts);
        cvis = (rdna4x_cdep *)calloc(w->nvis, sizeof *cvis);
        if (!visTexts || !cvis) { snprintf(w->err, sizeof w->err, "out of memory"); rc = RDNA4X_E_IO; goto done; }
        for (size_t i = 0; i < w->nvis; i++) {
            size_t tlen = 0;
            rc = rdna4x_module_to_llvm_text(w->vis[i].bytes, w->vis[i].len, w->vis[i].kind,
                                            &visTexts[i], &tlen, w->err, sizeof w->err);
            if (rc != RDNA4X_OK) goto refused;
            totalLl += tlen;
            if (totalLl > RDNA4X_MAX_LL) {
                snprintf(w->err, sizeof w->err, "Stage 2 visible function LLVM IR exceeds cap");
                rc = RDNA4X_E_CAP;
                goto refused;
            }
            cvis[i].symbol = (const uint8_t *)w->vis[i].name;
            cvis[i].symbol_len = strlen(w->vis[i].name);
            cvis[i].ll = (const uint8_t *)visTexts[i];
            cvis[i].ll_len = tlen;
        }
    }
    if (w->nift) {
        iftTexts = (char **)calloc(w->nift, sizeof *iftTexts);
        cift = (rdna4x_cift_fn *)calloc(w->nift, sizeof *cift);
        if (!iftTexts || !cift) { snprintf(w->err, sizeof w->err, "out of memory"); rc = RDNA4X_E_IO; goto done; }
        for (size_t i = 0; i < w->nift; i++) {
            size_t tlen = 0;
            rc = rdna4x_module_to_llvm_text(w->ift[i].bytes, w->ift[i].len, w->ift[i].kind,
                                            &iftTexts[i], &tlen, w->err, sizeof w->err);
            if (rc != RDNA4X_OK) goto refused;
            totalLl += tlen;
            if (totalLl > RDNA4X_MAX_LL) {
                snprintf(w->err, sizeof w->err, "Stage 3 intersection function LLVM IR exceeds cap");
                rc = RDNA4X_E_CAP;
                goto refused;
            }
            uint32_t iftKind = RDNA4RTR_IFT_KIND_BOUNDING_BOX;
            if (!rdna4rtr_infer_ift_kind_from_ir(iftTexts[i], tlen, w->ift[i].name, &iftKind)) {
                snprintf(w->err, sizeof w->err, "cannot infer intersection function kind (bounding_box vs triangle) for '%s'",
                         w->ift[i].name ? w->ift[i].name : "?");
                rc = RDNA4X_E_REFUSED;
                goto refused;
            }
            cift[i].symbol = (const uint8_t *)w->ift[i].name;
            cift[i].symbol_len = strlen(w->ift[i].name);
            cift[i].ll = (const uint8_t *)iftTexts[i];
            cift[i].ll_len = tlen;
            cift[i].kind = iftKind;
        }
    }

    {
        const uint32_t *loc = rdna4x_local_for(w->fname);
        int tr;
        if (w->useStage3 || w->nift > 0) {
            if (!w->translateStage3Fn) {
                snprintf(w->err, sizeof w->err, "libgarnetxlate.dylib lacks garnetx_translate_stage3");
                rc = RDNA4X_E_UNAVAILABLE;
                goto refused;
            }
            tr = w->translateStage3Fn((const uint8_t *)entryLl, entryLlLen, loc,
                                      cdeps, w->ndeps, clibs, w->ndynlibs, cvis, w->nvis,
                                      cift, w->nift, &w->caps, &spv, &spvLen, &meta, &metaLen, w->err, sizeof w->err);
        } else {
            tr = w->translateFn((const uint8_t *)entryLl, entryLlLen, loc,
                                cdeps, w->ndeps, clibs, w->ndynlibs, cvis, w->nvis,
                                &w->caps, &spv, &spvLen, &meta, &metaLen, w->err, sizeof w->err);
        }
        if (tr != 0) {
            if (!w->err[0]) snprintf(w->err, sizeof w->err, "%s returned %d",
                                     (w->useStage3 || w->nift > 0) ? "garnetx_translate_stage3" : "garnetx_translate_linked_libraries", tr);
            rc = RDNA4X_E_REFUSED;
            goto refused;
        }
    }
    if (!spv || !meta || !rdna4x_spv_size_ok(spvLen) || !rdna4x_meta_size_ok(metaLen)) {
        snprintf(w->err, sizeof w->err, "Stage 2/3 SPIR-V (%zu B) / meta (%zu B) outside caps", spvLen, metaLen);
        rc = RDNA4X_E_CAP;
        goto refused;
    }
    if (w->lowerArgs && w->xlib->lower_arguments_with_capabilities) {
        uint8_t *code = NULL, *json = NULL;
        size_t codeLen = 0, jsonLen = 0;
        int lr = w->xlib->lower_arguments_with_capabilities(spv, spvLen, meta, metaLen,
                                                            &code, &codeLen, &json, &jsonLen,
                                                            w->err, sizeof w->err, &w->caps);
        w->xlib->freep(spv, spvLen); w->xlib->freep(meta, metaLen);
        spv = code; spvLen = codeLen; meta = json; metaLen = jsonLen;
        if (lr != 0) {
            if (!w->err[0]) snprintf(w->err, sizeof w->err, "Stage 2 GPU argument lowering returned %d", lr);
            rc = RDNA4X_E_REFUSED;
            goto refused;
        }
        if (!spv || !meta || !rdna4x_spv_size_ok(spvLen) || !rdna4x_meta_size_ok(metaLen)) {
            snprintf(w->err, sizeof w->err, "Stage 2 argument postpass output outside caps");
            rc = RDNA4X_E_CAP;
            goto refused;
        }
    }

    // Publish .meta.json FIRST, .spv LAST (matches rdna4x_process_job contract) when disk cache is active.
    if (!memOnly) {
        if (!rdna4x_file_path(path, sizeof path, w->dir, w->sha, RDNA4X_EXT_META) ||
            rdna4x_write_atomic(path, meta, metaLen) != 0) {
            snprintf(w->err, sizeof w->err, "cannot write Stage 2 meta file");
            rc = RDNA4X_E_IO;
            goto done;
        }
        if (!rdna4x_file_path(path, sizeof path, w->dir, w->sha, RDNA4X_EXT_SPV) ||
            rdna4x_write_atomic(path, spv, spvLen) != 0) {
            snprintf(w->err, sizeof w->err, "cannot write Stage 2 spv file");
            rc = RDNA4X_E_IO;
            goto done;
        }
    }
    w->outSpv = (uint8_t *)malloc(spvLen);
    w->outMeta = (uint8_t *)malloc(metaLen);
    if (w->outSpv && w->outMeta) {
        memcpy(w->outSpv, spv, spvLen); w->outSpvLen = spvLen;
        memcpy(w->outMeta, meta, metaLen); w->outMetaLen = metaLen;
    } else {
        free(w->outSpv); w->outSpv = NULL; w->outSpvLen = 0;
        free(w->outMeta); w->outMeta = NULL; w->outMetaLen = 0;
        snprintf(w->err, sizeof w->err, "out of memory copying Stage 2 translation outputs");
        rc = RDNA4X_E_IO;
    }
    goto done;

refused:
    if (!memOnly && failPath[0] && w->err[0]) (void)rdna4x_write_atomic(failPath, w->err, strlen(w->err));
done:
    free(entryLl);
    if (depTexts) { for (size_t i = 0; i < w->ndeps; i++) free(depTexts[i]); free(depTexts); }
    if (libTexts) { for (size_t i = 0; i < w->ndynlibs; i++) free(libTexts[i]); free(libTexts); }
    if (visTexts) { for (size_t i = 0; i < w->nvis; i++) free(visTexts[i]); free(visTexts); }
    if (iftTexts) { for (size_t i = 0; i < w->nift; i++) free(iftTexts[i]); free(iftTexts); }
    free(cdeps); free(clibs); free(cvis); free(cift);
    if (spv) w->xlib->freep(spv, spvLen);
    if (meta) w->xlib->freep(meta, metaLen);
    if (!memOnly) {
        rdna4x_marker_remove(w->dir, w->sha);
        if (rc == RDNA4X_OK) (void)rdna4x_trim_dir(w->dir, RDNA4X_CACHE_CAP);
    }
    w->rc = rc;
    return NULL;
}

static BOOL rdna4x_translate_stage2_to_cache(NSString *dir, NSString *sha, NSString *name, const char *role,
                                             NSData *entryAir, RDNA4LinkCtx *lk, uint32_t rasterSamples,
                                             NSData **outSpv, NSDictionary **outMeta,
                                             rdna4vft_reflection_t *outVft, NSError **error) {
    if (outSpv) *outSpv = nil;
    if (outMeta) *outMeta = nil;
    if (outVft) rdna4vft_refl_init(outVft);
    if (error) *error = nil;
    if (lk && lk->linkageError) {
        if (error) *error = lk->linkageError;
        return NO;
    }
    if (!dir.length || !sha.length || !entryAir.length) {
        rdna4x_translation_error(error, role, name, sha, RDNA4X_E_CAP, @"missing Stage 2 translation inputs");
        return NO;
    }

    const BOOL memOnly = rdna4x_is_mem_dir(dir.fileSystemRepresentation) ? YES : NO;
    char spvp[RDNA4X_PATH_MAX], metap[RDNA4X_PATH_MAX];
    struct stat sb;
    if (memOnly) {
        NSData *cachedSpv = nil; NSDictionary *cachedMeta = nil; NSString *refused = nil;
        if (rdna4x_memcache_lookup(sha, &cachedSpv, &cachedMeta, &refused) && cachedSpv.length >= 20 && !(cachedSpv.length & 3) && cachedMeta) {
            if (outVft) {
                NSData *mdata = [NSJSONSerialization dataWithJSONObject:cachedMeta options:0 error:NULL];
                if (!mdata.length || rdna4vft_parse_reflection_json((const char *)mdata.bytes, mdata.length, outVft) != RDNA4VFT_OK) { rdna4vft_refl_destroy(outVft); rdna4x_translation_error(error, role, name, sha, RDNA4X_E_REFUSED, @"cached visible_function_tables metadata invalid"); return NO; }
            }
            if (outSpv) *outSpv = cachedSpv;
            if (outMeta) *outMeta = cachedMeta;
            return YES;
        }
        if (refused.length) {
            rdna4x_translation_error(error, role, name, sha, RDNA4X_E_SKIP_FAIL, refused);
            return NO;
        }
    } else if (rdna4x_file_path(spvp, sizeof spvp, dir.fileSystemRepresentation, sha.UTF8String, RDNA4X_EXT_SPV) &&
        stat(spvp, &sb) == 0 && S_ISREG(sb.st_mode)) {
        NSData *cachedSpv = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:spvp]];
        NSDictionary *cachedMeta = nil;
        BOOL metaReady = NO;
        if (rdna4x_file_path(metap, sizeof metap, dir.fileSystemRepresentation, sha.UTF8String, RDNA4X_EXT_META)) {
            NSData *mdata = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:metap]];
            if (mdata.length) {
                id obj = [NSJSONSerialization JSONObjectWithData:mdata options:0 error:NULL];
                if ([obj isKindOfClass:[NSDictionary class]]) {
                    cachedMeta = (NSDictionary *)obj;
                    if (outVft) {
                        rdna4vft_status_t vst = rdna4vft_parse_reflection_json((const char *)mdata.bytes, mdata.length, outVft);
                        if (vst != RDNA4VFT_OK) {
                            rdna4vft_refl_destroy(outVft);
                            rdna4x_translation_error(error, role, name, sha, RDNA4X_E_REFUSED,
                                [NSString stringWithFormat:@"cached visible_function_tables metadata invalid: %s", rdna4vft_status_str(vst)]);
                            return NO;
                        }
                    }
                    metaReady = YES;
                }
            }
        }
        if (cachedSpv.length >= 20 && !(cachedSpv.length & 3) && metaReady) {
            if (outSpv) *outSpv = cachedSpv;
            if (outMeta) *outMeta = cachedMeta;
            return YES;
        }
        if (outVft) rdna4vft_refl_destroy(outVft);
    }

    const BOOL wantStage3 = (lk && (lk->useStage3 || lk->intersectionFns.count > 0));
    const rdna4x_xlib *xl = NULL;
    rdna4x_translate_linked_libraries_fn translateFn = NULL;
    rdna4x_translate_stage3_fn translateStage3Fn = NULL;
    NSError *xerr = nil;
    if (wantStage3) {
        if (!rdna4x_ensure_stage3_xlib(&xl, &translateStage3Fn, &xerr)) {
            rdna4x_translation_error(error, role, name, sha, RDNA4X_E_UNAVAILABLE, xerr.localizedDescription);
            return NO;
        }
    } else if (!rdna4x_ensure_stage2_xlib(&xl, NULL, &translateFn, &xerr)) {
        rdna4x_translation_error(error, role, name, sha, RDNA4X_E_UNAVAILABLE, xerr.localizedDescription);
        return NO;
    }

    NSUInteger nd = lk ? lk->deps.count : 0;
    NSUInteger nl = lk ? lk->dynlibs.count : 0;
    NSUInteger nv = lk ? lk->visibleFns.count : 0;
    NSUInteger ni = lk ? lk->intersectionFns.count : 0;
    rdna4x_stage2_in_t *inDeps = nd ? (rdna4x_stage2_in_t *)calloc(nd, sizeof *inDeps) : NULL;
    rdna4x_stage2_in_t *inLibs = nl ? (rdna4x_stage2_in_t *)calloc(nl, sizeof *inLibs) : NULL;
    rdna4x_stage2_in_t *inVis  = nv ? (rdna4x_stage2_in_t *)calloc(nv, sizeof *inVis)  : NULL;
    rdna4x_stage2_in_t *inIft  = ni ? (rdna4x_stage2_in_t *)calloc(ni, sizeof *inIft)  : NULL;
    if ((nd && !inDeps) || (nl && !inLibs) || (nv && !inVis) || (ni && !inIft)) {
        free(inDeps); free(inLibs); free(inVis); free(inIft);
        rdna4x_translation_error(error, role, name, sha, RDNA4X_E_IO, @"out of memory preparing Stage 2/3 linkage");
        return NO;
    }
    for (NSUInteger i = 0; i < nd; i++) {
        NSDictionary *d = lk->deps[i];
        NSData *bc = d[@"bc"];
        inDeps[i] = (rdna4x_stage2_in_t){ [d[@"name"] UTF8String], (const uint8_t *)bc.bytes, bc.length, RDNA4DL_PAYLOAD_AIR_BITCODE };
    }
    for (NSUInteger i = 0; i < nl; i++) {
        NSDictionary *d = lk->dynlibs[i];
        NSData *mod = d[@"data"];
        inLibs[i] = (rdna4x_stage2_in_t){ [d[@"name"] UTF8String], (const uint8_t *)mod.bytes, mod.length, (uint32_t)[d[@"kind"] unsignedIntValue] };
    }
    for (NSUInteger i = 0; i < nv; i++) {
        NSDictionary *d = lk->visibleFns[i];
        NSData *bc = d[@"bc"];
        inVis[i] = (rdna4x_stage2_in_t){ [d[@"name"] UTF8String], (const uint8_t *)bc.bytes, bc.length, RDNA4DL_PAYLOAD_AIR_BITCODE };
    }
    for (NSUInteger i = 0; i < ni; i++) {
        NSDictionary *d = lk->intersectionFns[i];
        NSData *bc = d[@"bc"];
        inIft[i] = (rdna4x_stage2_in_t){ [d[@"name"] UTF8String], (const uint8_t *)bc.bytes, bc.length, RDNA4DL_PAYLOAD_AIR_BITCODE };
    }

    const uint64_t t0 = rdna4_now();
    rdna4x_stage2_work_t work;
    memset(&work, 0, sizeof work);
    work.xlib = xl;
    work.translateFn = translateFn;
    work.translateStage3Fn = translateStage3Fn;
    work.dir = dir.fileSystemRepresentation;
    work.sha = sha.UTF8String;
    work.fname = name.UTF8String;
    work.entryAir = (const uint8_t *)entryAir.bytes;
    work.entryAirLen = entryAir.length;
    work.deps = inDeps; work.ndeps = nd;
    work.dynlibs = inLibs; work.ndynlibs = nl;
    work.vis = inVis; work.nvis = nv;
    work.ift = inIft; work.nift = ni;
    work.useStage3 = wantStage3 ? 1 : 0;
    work.caps = rdna4_rt_xlate_caps();
    work.caps.flags |= garnetx_cap_raster_samples(rasterSamples);
    work.lowerArgs = RDNA4R.gpuArgumentTables ? 1 : 0;

    int rc = RDNA4X_E_IO;
    for (int attempt = 0; attempt < 2; attempt++) {
        free(work.outSpv); work.outSpv = NULL; work.outSpvLen = 0;
        free(work.outMeta); work.outMeta = NULL; work.outMetaLen = 0;
        work.err[0] = 0;
        work.dir = dir.fileSystemRepresentation;
        if (rdna4x_run_on_worker_stack(rdna4x_stage2_worker, &work) != 0) {
            snprintf(work.err, sizeof work.err, "failed to spawn Stage 2/3 translation worker thread");
            rc = RDNA4X_E_IO;
            break;
        }
        rc = work.rc;
        if (rc != RDNA4X_E_IO || !rdna4x_dir_recheck()) break;
        dir = rdna4x_user_dir() ?: (rdna4x_memcache_active() ? @RDNA4X_MEMCACHE_DIR : dir);
        RDNA4LOG("inproc Stage 2/3 RETRY %s '%s' %s: cache directory rebuilt after: %s", role, name.UTF8String, sha.UTF8String, work.err);
    }
    free(inDeps); free(inLibs); free(inVis); free(inIft);

    const double ms = (double)(rdna4_now() - t0) / 1e6;
    if (rc == RDNA4X_OK) {
        RDNA4LOG("inproc Stage %d OK %s '%s' %s: translated in %.0f ms (deps=%lu dynlibs=%lu visible=%lu intersection=%lu)",
                 wantStage3 ? 3 : 2, role, name.UTF8String, sha.UTF8String, ms, (unsigned long)nd, (unsigned long)nl, (unsigned long)nv, (unsigned long)ni);
        BOOL okOut = YES;
        NSData *spvObj = nil;
        NSDictionary *metaDict = nil;
        if (!work.outSpv || work.outSpvLen < 20 || (work.outSpvLen & 3) || !work.outMeta || !work.outMetaLen) {
            rdna4x_translation_error(error, role, name, sha, RDNA4X_E_IO, @"Stage 2 translation produced empty or unaligned outputs");
            okOut = NO;
        }
        if (okOut && outSpv) {
            spvObj = [NSData dataWithBytes:work.outSpv length:work.outSpvLen];
            if (!spvObj.length) {
                rdna4x_translation_error(error, role, name, sha, RDNA4X_E_IO, @"out of memory copying Stage 2 SPIR-V data");
                okOut = NO;
            }
        }
        if (okOut && outMeta) {
            id obj = [NSJSONSerialization JSONObjectWithData:[NSData dataWithBytesNoCopy:work.outMeta length:work.outMetaLen freeWhenDone:NO]
                                                     options:0 error:NULL];
            if (![obj isKindOfClass:[NSDictionary class]]) {
                rdna4x_translation_error(error, role, name, sha, RDNA4X_E_REFUSED, @"Stage 2 reflection metadata is not a valid JSON object");
                okOut = NO;
            } else {
                metaDict = (NSDictionary *)obj;
            }
        }
        if (okOut && outVft) {
            rdna4vft_status_t vst = rdna4vft_parse_reflection_json((const char *)work.outMeta, work.outMetaLen, outVft);
            if (vst != RDNA4VFT_OK) {
                rdna4vft_refl_destroy(outVft);
                rdna4x_translation_error(error, role, name, sha, RDNA4X_E_REFUSED,
                    [NSString stringWithFormat:@"Stage 2 visible_function_tables metadata invalid: %s", rdna4vft_status_str(vst)]);
                okOut = NO;
            }
        }
        if (okOut) {
            if (memOnly) (void)rdna4x_memcache_store(sha, work.outSpv, work.outSpvLen, work.outMeta, work.outMetaLen);
            if (outSpv) *outSpv = spvObj;
            if (outMeta) *outMeta = metaDict;
        }
        free(work.outSpv);
        free(work.outMeta);
        return okOut;
    }

    free(work.outSpv);
    free(work.outMeta);
    NSString *reason = [NSString stringWithUTF8String:work.err[0] ? work.err : "Stage 2 translation failed"];
    if (rc == RDNA4X_E_SKIP_FAIL) {
        NSString *cached = rdna4x_cached_refusal(dir, sha);
        if (cached.length) reason = [NSString stringWithFormat:@"the translator refused before: %@", cached];
    }
    RDNA4LOG_ERR("inproc Stage 2 FAIL %s '%s' %s: code %d: %s", role, name.UTF8String, sha.UTF8String, rc, reason.UTF8String);
    rdna4x_translation_error(error, role, name, sha, rc, reason);
    return NO;
}

// Both render stages share the app deadline. After timeouts, jobs still run off
// the caller's thread and publish into the user directory watched by hot-swap.
GARNET_INTERNAL BOOL rdna4x_translate_fn_with_samples(id fn, const char *role, uint64_t deadlineNs, uint32_t rasterSamples, NSError **error) {
    if (error) *error = nil;
    NSString *name = fn && [fn respondsToSelector:@selector(name)] ? [fn name] : @"?";
    RDNA4LinkCtx *lk = rdna4_lk_for(fn);   // build 18 (P3) + Metal 3 Stage 2 linkage
    if (lk && lk->linkageError) {
        if (error) *error = lk->linkageError;
        return NO;
    }
    NSString *dir = rdna4x_user_dir();
    const BOOL useMem = (!dir && rdna4x_memcache_active());
    if (useMem) dir = @RDNA4X_MEMCACHE_DIR;
    if (!dir) {
        rdna4x_translation_error(error, role, name, nil, RDNA4X_E_UNAVAILABLE,
                                 @"in-process translation cache is unavailable");
        return NO;
    }
    SEL sel = NSSelectorFromString(@"bitcodeData");
    NSData *bc = (fn && [fn respondsToSelector:sel]) ? ((NSData *(*)(id, SEL))objc_msgSend)(fn, sel) : nil;
    if (!bc.length) {
        rdna4x_translation_error(error, role, name, nil, RDNA4X_E_CAP,
                                 @"function has no bitcodeData");
        return NO;
    }
    NSString *sha = rdna4_sha256hex(bc);
    if (lk) sha = lk->lsha;
    if (rasterSamples != 0u) {
        char msSha[65];
        if (rdna4x_msaa_sha(msSha, sha.UTF8String, rasterSamples) != 0) {
            rdna4x_translation_error(error, role, name, sha, RDNA4X_E_CAP,
                                     @"invalid raster sample count for MSAA shader specialization");
            return NO;
        }
        sha = [NSString stringWithUTF8String:msSha];
    }
    if (!useMem) {
        rdna4x_capture_request(dir, sha, name, role, bc, lk ? lk->deps : nil);
        char spvp[RDNA4X_PATH_MAX]; struct stat sb;
        if (rdna4x_file_path(spvp, sizeof spvp, dir.fileSystemRepresentation, sha.UTF8String, RDNA4X_EXT_SPV) && stat(spvp, &sb) == 0 && S_ISREG(sb.st_mode)) return YES;   // installed meanwhile (another thread / process)
    } else if (rdna4x_memcache_lookup(sha, NULL, NULL, NULL)) {
        return YES;
    }
    NSError *backendError = nil;
    if (!rdna4_radv_open(&backendError)) {
        rdna4x_translation_error(error, role, name, sha, RDNA4X_E_UNAVAILABLE, backendError.localizedDescription);
        return NO;
    }
    if (lk && (lk->useStage2 || lk->useStage3) && (lk->requireStage2 || rdna4x_stage2_symbols_available())) {
        return rdna4x_translate_stage2_to_cache(dir, sha, name, role, bc, lk, rasterSamples, NULL, NULL, NULL, error);
    }
    rdna4x_engine_prepare();
    const BOOL unlimited = (deadlineNs == UINT64_MAX && !rdna4_is_ws());
    int waitMs;
    BOOL waiting = NO;
    if (unlimited) {
        waitMs = -1;
    } else {
        const uint64_t now = rdna4_now();
        const uint64_t leftMs = deadlineNs > now ? (deadlineNs - now) / 1000000ull : 0;
        waitMs = leftMs > RDNA4G_SYNC_WAIT_APP_MS ? RDNA4G_SYNC_WAIT_APP_MS : (int)leftMs;
        waiting = waitMs > 0 && rdna4_sync_begin(!rdna4_is_ws());
        if (!waiting) waitMs = 0;   // enqueue even when the strikes budget is exhausted
    }
    char err[256] = ""; const uint64_t t0 = rdna4_now();
    int rc = RDNA4X_E_IO;
    for (int attempt = 0; attempt < 2; attempt++) {
        if (lk) {
            const NSUInteger nd = lk->deps.count; rdna4x_ldep *ld = (rdna4x_ldep *)calloc(nd ? nd : 1, sizeof *ld);
            for (NSUInteger i = 0; i < nd && ld; i++) { NSDictionary *dd = lk->deps[i]; ld[i].symbol = [dd[@"name"] UTF8String]; ld[i].air = (const uint8_t *)[dd[@"bc"] bytes]; ld[i].airLen = [dd[@"bc"] length]; }
            if (ld) rc = rdna4x_engine_run_samples(&RDNA4XL.eng, dir.fileSystemRepresentation, sha.UTF8String, name.UTF8String, bc.bytes, bc.length, ld, nd, rasterSamples, waitMs, err, sizeof err);
            else { rc = RDNA4X_E_IO; strlcpy(err, "out of memory allocating linked dependencies", sizeof err); }
            free(ld);
        } else
            rc = rdna4x_engine_run_samples(&RDNA4XL.eng, dir.fileSystemRepresentation, sha.UTF8String, name.UTF8String, bc.bytes, bc.length, NULL, 0, rasterSamples, waitMs, err, sizeof err);
        if (useMem || rc != RDNA4X_E_IO || !rdna4x_dir_recheck()) break;
        dir = rdna4x_user_dir() ?: (rdna4x_memcache_active() ? @RDNA4X_MEMCACHE_DIR : dir);
        RDNA4LOG("inproc RETRY %s '%s' %s: the cache directory was rebuilt after: %s", role, name.UTF8String, sha.UTF8String, err);
    }
    if (waiting) rdna4_sync_end(rc == RDNA4X_OK, rc == RDNA4X_E_TIMEOUT);
    const double ms = (double)(rdna4_now() - t0) / 1e6;
    pthread_mutex_lock(&RDNA4XL.mu);
    if (!rdna4x_logged) rdna4x_logged = [NSMutableSet set];
    const BOOL firstLog = ![rdna4x_logged containsObject:sha]; if (firstLog) [rdna4x_logged addObject:sha];
    pthread_mutex_unlock(&RDNA4XL.mu);
    if (rc == RDNA4X_OK) RDNA4LOG("inproc OK %s '%s' %s: translated in %.0f ms%s", role, name.UTF8String, sha.UTF8String, ms, rasterSamples ? " (msaa)" : "");
    else if (rc == RDNA4X_E_TIMEOUT) RDNA4LOG_WARN("inproc TIMEOUT %s '%s' %s: compilation did not finish within %d ms", role, name.UTF8String, sha.UTF8String, waitMs);
    else if (firstLog) {
        if (!rasterSamples && rdna4x_needs_raster_sample_count(err))
            RDNA4LOG("inproc MSAA-RETRY %s '%s' %s: %s", role, name.UTF8String, sha.UTF8String, err);
        else
            RDNA4LOG_ERR("inproc FAIL %s '%s' %s: code %d: %s", role, name.UTF8String, sha.UTF8String, rc, err);
    }
    if (rc != RDNA4X_OK) {
        NSString *reason = rc == RDNA4X_E_TIMEOUT
            ? [NSString stringWithFormat:@"translation exceeded the %d ms wait deadline", waitMs]
            : [NSString stringWithUTF8String:err[0] ? err : "translation failed without an engine reason"];
        if (rc == RDNA4X_E_SKIP_FAIL) {
            NSString *cached = nil;
            if (useMem) (void)rdna4x_memcache_lookup(sha, NULL, NULL, &cached);
            else cached = rdna4x_cached_refusal(dir, sha);
            if (cached.length) reason = [NSString stringWithFormat:@"the translator refused before: %@", cached];
        }
        rdna4x_translation_error(error, role, name, sha, rc, reason);
    }
    return rc == RDNA4X_OK;
}
GARNET_INTERNAL BOOL rdna4x_translate_fn_with_error(id fn, const char *role, uint64_t deadlineNs, NSError **error) {
    return rdna4x_translate_fn_with_samples(fn, role, deadlineNs, 0u, error);
}
// Stage 2 linkage helpers for compute/vertex/fragment pipelines (MTLLinkedFunctions + preloadedLibraries + binaryFunctions).
GARNET_INTERNAL BOOL rdna4x_array_nonempty(id obj, const char *prop) {
    SEL s = sel_registerName(prop);
    if (!obj || ![obj respondsToSelector:s]) return NO;
    id v = ((id (*)(id, SEL))objc_msgSend)(obj, s);
    return [v isKindOfClass:[NSArray class]] && [(NSArray *)v count] > 0;
}
GARNET_INTERNAL id rdna4x_prop(id obj, const char *prop) {
    SEL sl = sel_registerName(prop);
    return (obj && [obj respondsToSelector:sl]) ? ((id (*)(id, SEL))objc_msgSend)(obj, sl) : nil;
}
GARNET_INTERNAL BOOL rdna4x_bool_prop(id obj, const char *prop) {
    SEL sl = sel_registerName(prop);
    return (obj && [obj respondsToSelector:sl]) ? ((BOOL (*)(id, SEL))objc_msgSend)(obj, sl) : NO;
}
static BOOL rdna4x_collect_fn_list(NSArray *fns, NSMutableArray<NSDictionary *> *out,
                                   NSMutableDictionary<NSString *, NSString *> *byName,
                                   NSUInteger *totalBytes, NSString **why) {
    SEL bcSel = NSSelectorFromString(@"bitcodeData");
    for (id f in fns) {
        NSString *nm = [rdna4x_prop(f, "name") isKindOfClass:[NSString class]] ? rdna4x_prop(f, "name") : nil;
        NSData *bc = (f && [f respondsToSelector:bcSel]) ? ((NSData *(*)(id, SEL))objc_msgSend)(f, bcSel) : nil;
        if (!nm.length || !bc.length) {
            if (why) *why = [NSString stringWithFormat:@"a linked function ('%@') has no name or no bitcodeData", nm ? nm : @"?"];
            return NO;
        }
        NSString *sha = rdna4_sha256hex(bc);
        NSString *prev = byName[nm];
        if (prev) {
            if ([prev isEqualToString:sha]) continue;
            if (why) *why = [NSString stringWithFormat:@"the name '%@' is linked to two different functions", nm];
            return NO;
        }
        byName[nm] = sha;
        *totalBytes += bc.length;
        if (out.count >= RDNA4X_MAX_DEPS || *totalBytes > RDNA4X_MAX_DEP_AIR || !rdna4x_air_size_ok(bc.length)) {
            if (why) *why = @"too many linked functions or too much bitcode";
            return NO;
        }
        [out addObject:@{ @"name": nm, @"bc": bc, @"sha": sha }];
    }
    return YES;
}
static BOOL rdna4x_is_intersection_fn(id f) {
    if (!f || ![f respondsToSelector:@selector(functionType)]) return NO;
    return ((NSUInteger (*)(id, SEL))objc_msgSend)(f, @selector(functionType)) ==
           (NSUInteger)MTLFunctionTypeIntersection;
}
static int rdna4x_stage3_link_sha(
    char out[65],
    const char *entry_sha,
    size_t ndeps, const char *const *dep_names, const char *const *dep_shas,
    size_t ndynlibs, const char *const *dynlib_names, const char *const *dynlib_shas,
    size_t nvis, const char *const *vis_names, const char *const *vis_shas,
    size_t nift, const char *const *ift_names, const char *const *ift_shas)
{
    if (!out) return -1;
    out[0] = '\0';
    if (!rdna4dl_hex_sha_ok(entry_sha)) return -1;
    if (ndeps > 4096u || ndynlibs > 4096u || nvis > 4096u || nift > 4096u) return -1;
    if ((ndeps && (!dep_names || !dep_shas)) ||
        (ndynlibs && (!dynlib_names || !dynlib_shas)) ||
        (nvis && (!vis_names || !vis_shas)) ||
        (nift && (!ift_names || !ift_shas)))
        return -1;

    size_t total = ndeps + ndynlibs + nvis + nift;
    rdna4dl_key_item_t *items = (rdna4dl_key_item_t *)calloc(total ? total : 1u, sizeof *items);
    if (!items) return -1;

    size_t pos = 0;
    for (size_t i = 0; i < ndeps; i++) {
        if (!dep_names[i] || !dep_names[i][0] || !rdna4dl_hex_sha_ok(dep_shas[i])) { free(items); return -1; }
        items[pos++] = (rdna4dl_key_item_t){ 1u, dep_names[i], dep_shas[i] };
    }
    for (size_t i = 0; i < ndynlibs; i++) {
        if (!dynlib_names[i] || !dynlib_names[i][0] || !rdna4dl_hex_sha_ok(dynlib_shas[i])) { free(items); return -1; }
        items[pos++] = (rdna4dl_key_item_t){ 2u, dynlib_names[i], dynlib_shas[i] };
    }
    for (size_t i = 0; i < nvis; i++) {
        if (!vis_names[i] || !vis_names[i][0] || !rdna4dl_hex_sha_ok(vis_shas[i])) { free(items); return -1; }
        items[pos++] = (rdna4dl_key_item_t){ 3u, vis_names[i], vis_shas[i] };
    }
    for (size_t i = 0; i < nift; i++) {
        if (!ift_names[i] || !ift_names[i][0] || !rdna4dl_hex_sha_ok(ift_shas[i])) { free(items); return -1; }
        items[pos++] = (rdna4dl_key_item_t){ 4u, ift_names[i], ift_shas[i] };
    }
    if (total > 1u) qsort(items, total, sizeof *items, rdna4dl_key_item_cmp);

    rdna4dl_sha256_ctx ctx;
    rdna4dl_sha256_init(&ctx);
    static const char tag[] = "rdna4x-stage3-v1\n";
    rdna4dl_sha256_update(&ctx, tag, sizeof tag - 1u);
    rdna4dl_sha256_update(&ctx, entry_sha, 64u);
    rdna4dl_sha256_update(&ctx, "\n", 1u);
    for (size_t i = 0; i < total; i++) {
        if (i > 0 && rdna4dl_key_item_cmp(&items[i], &items[i - 1u]) == 0) continue;
        uint8_t sec = items[i].section;
        rdna4dl_sha256_update(&ctx, &sec, 1u);
        rdna4dl_sha256_update(&ctx, items[i].name, strlen(items[i].name));
        rdna4dl_sha256_update(&ctx, "\0", 1u);
        rdna4dl_sha256_update(&ctx, items[i].sha, 64u);
        rdna4dl_sha256_update(&ctx, "\n", 1u);
    }
    free(items);

    uint8_t digest[32];
    rdna4dl_sha256_final(&ctx, digest);
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < 32u; i++) {
        out[i * 2u] = hex[digest[i] >> 4];
        out[i * 2u + 1u] = hex[digest[i] & 0x0Fu];
    }
    out[64] = '\0';
    return 0;
}
// Collects stage linkage: deps (non-intersection linked/binary/private), visibleFns (non-intersection public/groups/binary),
// intersectionFns (MTLFunctionTypeIntersection functions for Stage 3 IFT), and dynlibs (preloadedLibraries + non-intersection binaryFunctions).
static BOOL rdna4x_stage3_link_collect(id linked, id preloaded,
                                       NSArray<NSDictionary *> **depsOut,
                                       NSArray<NSDictionary *> **visibleOut,
                                       NSArray<NSDictionary *> **intersectionOut,
                                       NSArray<NSDictionary *> **dynlibsOut,
                                       NSString **why) {
    NSMutableArray<id> *pubFns = [NSMutableArray array];
    NSMutableArray<id> *privFns = [NSMutableArray array];
    NSMutableArray<id> *binFns = [NSMutableArray array];
    NSMutableArray<id> *iftRaw = [NSMutableArray array];
    {
        id v = rdna4x_prop(linked, "functions");
        if ([v isKindOfClass:[NSArray class]]) {
            for (id fnObj in (NSArray *)v) {
                if (rdna4x_is_intersection_fn(fnObj)) [iftRaw addObject:fnObj];
                else [pubFns addObject:fnObj];
            }
        }
        v = rdna4x_prop(linked, "binaryFunctions");
        if ([v isKindOfClass:[NSArray class]]) {
            for (id fnObj in (NSArray *)v) {
                if (rdna4x_is_intersection_fn(fnObj)) [iftRaw addObject:fnObj];
                else { [pubFns addObject:fnObj]; [binFns addObject:fnObj]; }
            }
        }
        v = rdna4x_prop(linked, "privateFunctions");
        if ([v isKindOfClass:[NSArray class]]) {
            for (id fnObj in (NSArray *)v) {
                if (rdna4x_is_intersection_fn(fnObj)) [iftRaw addObject:fnObj];
                else [privFns addObject:fnObj];
            }
        }
    }
    id gr = rdna4x_prop(linked, "groups");
    if ([gr isKindOfClass:[NSDictionary class]]) {
        for (id k in [(NSDictionary *)gr allKeys]) {
            id v = ((NSDictionary *)gr)[k];
            if ([v isKindOfClass:[NSArray class]]) {
                for (id fnObj in (NSArray *)v) {
                    if (rdna4x_is_intersection_fn(fnObj)) [iftRaw addObject:fnObj];
                    else [pubFns addObject:fnObj];
                }
            }
        }
    }
    if (!intersectionOut && iftRaw.count > 0) {
        if (why) *why = @"intersection functions are not permitted on this pipeline stage";
        return NO;
    }

    NSMutableArray<NSDictionary *> *visArr = [NSMutableArray array];
    NSMutableDictionary<NSString *, NSString *> *visByName = [NSMutableDictionary dictionary];
    NSUInteger visBytes = 0;
    if (!rdna4x_collect_fn_list(pubFns, visArr, visByName, &visBytes, why)) return NO;

    NSMutableArray<NSDictionary *> *depArr = [visArr mutableCopy];
    NSMutableDictionary<NSString *, NSString *> *depByName = [visByName mutableCopy];
    NSUInteger depBytes = visBytes;
    if (!rdna4x_collect_fn_list(privFns, depArr, depByName, &depBytes, why)) return NO;

    NSMutableArray<NSDictionary *> *iftArr = [NSMutableArray array];
    NSMutableDictionary<NSString *, NSString *> *iftByName = [NSMutableDictionary dictionary];
    NSUInteger iftBytes = 0;
    if (!rdna4x_collect_fn_list(iftRaw, iftArr, iftByName, &iftBytes, why)) return NO;

    NSMutableArray<NSDictionary *> *libArr = [NSMutableArray array];
    NSMutableSet<NSString *> *seenMods = [NSMutableSet set];
    NSUInteger libBytes = 0;
    if ([preloaded isKindOfClass:[NSArray class]]) {
        for (id lib in (NSArray *)preloaded) {
            if (![lib respondsToSelector:@selector(rdna4PayloadModules)] ||
                ![lib respondsToSelector:@selector(installName)]) {
                if (why) *why = @"preloadedLibraries contains an unsupported dynamic library object";
                return NO;
            }
            NSArray<NSDictionary *> *mods = [(id<RDNA4DynamicLibraryInternal>)lib rdna4PayloadModules];
            if (!mods.count) {
                if (why) *why = @"preloaded dynamic library has no AIR/LLVM payload modules";
                return NO;
            }
            for (NSDictionary *m in mods) {
                NSString *mname = m[@"name"];
                NSData *mdata = m[@"data"];
                NSString *msha = m[@"sha"];
                NSNumber *mkind = m[@"kind"];
                if (!mname.length || !mdata.length || !msha.length || !mkind) {
                    if (why) *why = @"preloaded dynamic library module entry is malformed";
                    return NO;
                }
                NSString *dedup = [NSString stringWithFormat:@"%@|%@", mname, msha];
                if ([seenMods containsObject:dedup]) continue;
                [seenMods addObject:dedup];
                if (libArr.count >= RDNA4X_MAX_DEPS || mdata.length > RDNA4DL_MAX_FILE_BYTES - libBytes) {
                    if (why) *why = @"too many preloaded dynamic library modules or total size exceeds limit";
                    return NO;
                }
                libBytes += mdata.length;
                [libArr addObject:m];
            }
        }
    }
    // Expose binaryFunctions to direct external symbol linking alongside preloadedLibraries.
    if (binFns.count) {
        SEL bcSel = NSSelectorFromString(@"bitcodeData");
        for (id bf in binFns) {
            NSString *nm = [rdna4x_prop(bf, "name") isKindOfClass:[NSString class]] ? rdna4x_prop(bf, "name") : nil;
            NSData *bc = (bf && [bf respondsToSelector:bcSel]) ? ((NSData *(*)(id, SEL))objc_msgSend)(bf, bcSel) : nil;
            if (!nm.length || !bc.length) continue;
            NSString *sha = rdna4_sha256hex(bc);
            NSString *dedup = [NSString stringWithFormat:@"binfn:%@|%@", nm, sha];
            if ([seenMods containsObject:dedup]) continue;
            [seenMods addObject:dedup];
            if (libArr.count >= RDNA4X_MAX_DEPS || bc.length > RDNA4DL_MAX_FILE_BYTES - libBytes) {
                if (why) *why = @"too many preloaded dynamic library / binaryFunction modules or total size exceeds limit";
                return NO;
            }
            libBytes += bc.length;
            [libArr addObject:@{ @"name": [NSString stringWithFormat:@"binfn:%@", nm],
                                 @"kind": @(RDNA4DL_PAYLOAD_AIR_BITCODE),
                                 @"data": bc, @"sha": sha }];
        }
    }

    if (depsOut) *depsOut = depArr;
    if (visibleOut) *visibleOut = visArr;
    if (intersectionOut) *intersectionOut = iftArr;
    if (dynlibsOut) *dynlibsOut = libArr;
    return YES;
}
GARNET_INTERNAL BOOL rdna4x_stage2_link_collect(id linked, id preloaded,
                                       NSArray<NSDictionary *> **depsOut,
                                       NSArray<NSDictionary *> **visibleOut,
                                       NSArray<NSDictionary *> **dynlibsOut,
                                       NSString **why) {
    return rdna4x_stage3_link_collect(linked, preloaded, depsOut, visibleOut, NULL, dynlibsOut, why);
}
static NSString *rdna4x_names(id arr, BOOL groups) {   // a short, bounded list of function names for the log
    NSMutableArray<NSString *> *n = [NSMutableArray array];
    if (groups && [arr isKindOfClass:[NSDictionary class]]) { for (id k in [(NSDictionary *)arr allKeys]) { NSMutableArray *m = [NSMutableArray array]; id v = ((NSDictionary *)arr)[k]; if ([v isKindOfClass:[NSArray class]]) for (id f in v) [m addObject:[rdna4x_prop(f, "name") description] ?: @"?"];
        [n addObject:[NSString stringWithFormat:@"%@=[%@]", k, [m componentsJoinedByString:@","]]]; } }
    else if ([arr isKindOfClass:[NSArray class]]) for (id f in arr) [n addObject:[rdna4x_prop(f, "name") description] ?: @"?"];
    if (n.count > 12) { NSUInteger tot = n.count; [n removeObjectsInRange:NSMakeRange(12, n.count - 12)]; [n addObject:[NSString stringWithFormat:@"... (%lu in all)", (unsigned long)tot]]; }
    return [n componentsJoinedByString:@" "];
}
// desc: an MTLRenderPipelineDescriptor or MTLComputePipelineDescriptor; properties are read by selector.
// Installs the thread-local RDNA4LinkCtx for the stage (slot 0 vertex, 1 fragment, 2 kernel), supporting
// both legacy MTLLinkedFunctions and Metal 3 Stage 2 preloadedLibraries / binaryFunctions / VFTs.
static void rdna4x_linkage_check(id desc, const char *role, const char *linkedProp, const char *preloadProp, const char *name, id fn, int slot) {
    rdna4_lk_install(slot, nil);
    id linked = rdna4x_prop(desc, linkedProp);
    id preloaded = rdna4x_prop(desc, preloadProp);
    const BOOL f = rdna4x_array_nonempty(linked, "functions"), bf = rdna4x_array_nonempty(linked, "binaryFunctions"), pf = rdna4x_array_nonempty(linked, "privateFunctions"), pl = rdna4x_array_nonempty(desc, preloadProp);
    id gr = rdna4x_prop(linked, "groups"); const BOOL gg = [gr isKindOfClass:[NSDictionary class]] && [(NSDictionary *)gr count] > 0;
    const BOOL addBin = (slot == 0 && rdna4x_bool_prop(desc, "supportAddingVertexBinaryFunctions")) ||
                        (slot == 1 && rdna4x_bool_prop(desc, "supportAddingFragmentBinaryFunctions")) ||
                        (slot == 2 && rdna4x_bool_prop(desc, "supportAddingBinaryFunctions"));
    if (!(f || bf || pf || gg || pl || addBin)) return;
    NSString *why = nil; RDNA4LinkCtx *ctx = nil;
    NSArray<NSDictionary *> *deps = nil, *visFns = nil, *iftFns = nil, *dynlibs = nil;
    if (!rdna4x_active()) {
        why = @"this process does not translate in process";
    } else if (!rdna4x_stage3_link_collect(linked, preloaded, &deps, &visFns, &iftFns, &dynlibs, &why)) {
        if (!why) why = @"invalid stage linkage";
    } else if (!deps.count && !dynlibs.count && !visFns.count && !iftFns.count && !addBin) {
        why = @"nothing to link";
    }
    if (!why && fn) {
        NSData *ebc = ((NSData *(*)(id, SEL))objc_msgSend)(fn, NSSelectorFromString(@"bitcodeData"));
        NSString *esha = ebc.length ? rdna4_sha256hex(ebc) : nil;
        if (!esha) why = @"the entry function has no bitcodeData";
        else {
            const BOOL useStage3 = (iftFns.count > 0);
            const BOOL useStage2 = (useStage3 || dynlibs.count > 0 || visFns.count > 0 || bf || pl || addBin);
            const BOOL requireStage2 = (dynlibs.count > 0 || visFns.count > 0 || deps.count > 0 || f || bf || pf || gg || pl || addBin);
            char key[65];
            int krc = -1;
            if (useStage3) {
                const char *depN[RDNA4X_MAX_DEPS], *depS[RDNA4X_MAX_DEPS];
                const char *libN[RDNA4X_MAX_DEPS], *libS[RDNA4X_MAX_DEPS];
                const char *visN[RDNA4X_MAX_DEPS], *visS[RDNA4X_MAX_DEPS];
                const char *iftN[RDNA4X_MAX_DEPS], *iftS[RDNA4X_MAX_DEPS];
                for (NSUInteger i = 0; i < deps.count; i++) { depN[i] = [deps[i][@"name"] UTF8String]; depS[i] = [deps[i][@"sha"] UTF8String]; }
                for (NSUInteger i = 0; i < dynlibs.count; i++) { libN[i] = [dynlibs[i][@"name"] UTF8String]; libS[i] = [dynlibs[i][@"sha"] UTF8String]; }
                for (NSUInteger i = 0; i < visFns.count; i++) { visN[i] = [visFns[i][@"name"] UTF8String]; visS[i] = [visFns[i][@"sha"] UTF8String]; }
                for (NSUInteger i = 0; i < iftFns.count; i++) { iftN[i] = [iftFns[i][@"name"] UTF8String]; iftS[i] = [iftFns[i][@"sha"] UTF8String]; }
                krc = rdna4x_stage3_link_sha(key, esha.UTF8String,
                                             deps.count, depN, depS,
                                             dynlibs.count, libN, libS,
                                             visFns.count, visN, visS,
                                             iftFns.count, iftN, iftS);
            } else if (useStage2) {
                const char *depN[RDNA4X_MAX_DEPS], *depS[RDNA4X_MAX_DEPS];
                const char *libN[RDNA4X_MAX_DEPS], *libS[RDNA4X_MAX_DEPS];
                const char *visN[RDNA4X_MAX_DEPS], *visS[RDNA4X_MAX_DEPS];
                for (NSUInteger i = 0; i < deps.count; i++) { depN[i] = [deps[i][@"name"] UTF8String]; depS[i] = [deps[i][@"sha"] UTF8String]; }
                for (NSUInteger i = 0; i < dynlibs.count; i++) { libN[i] = [dynlibs[i][@"name"] UTF8String]; libS[i] = [dynlibs[i][@"sha"] UTF8String]; }
                for (NSUInteger i = 0; i < visFns.count; i++) { visN[i] = [visFns[i][@"name"] UTF8String]; visS[i] = [visFns[i][@"sha"] UTF8String]; }
                krc = rdna4dl_stage2_link_sha(key, esha.UTF8String,
                                              deps.count, depN, depS,
                                              dynlibs.count, libN, libS,
                                              visFns.count, visN, visS);
            } else {
                const char *syms[RDNA4X_MAX_DEPS], *shas[RDNA4X_MAX_DEPS];
                for (NSUInteger i = 0; i < deps.count; i++) { syms[i] = [deps[i][@"name"] UTF8String]; shas[i] = [deps[i][@"sha"] UTF8String]; }
                krc = rdna4x_link_sha(key, esha.UTF8String, deps.count, syms, shas);
            }
            if (krc != 0) why = @"cannot form the linked cache key";
            else {
                ctx = [RDNA4LinkCtx new];
                ctx->fn = fn;
                ctx->lsha = [NSString stringWithUTF8String:key];
                ctx->deps = deps;
                ctx->dynlibs = dynlibs;
                ctx->visibleFns = visFns;
                ctx->intersectionFns = iftFns;
                ctx->useStage2 = useStage2;
                ctx->useStage3 = useStage3;
                ctx->requireStage2 = requireStage2 || useStage3;
            }
        }
    }
    if (!ctx && why && fn && (f || bf || pf || gg || pl || addBin) && rdna4x_active()) {
        // Preserve deterministic linkage errors for linkedFunctions/preloadedLibraries instead of silently falling back.
        ctx = [RDNA4LinkCtx new];
        ctx->fn = fn;
        ctx->lsha = @"0000000000000000000000000000000000000000000000000000000000000000";
        ctx->useStage2 = YES;
        ctx->requireStage2 = YES;
        ctx->linkageError = rdna4_err(42, [NSString stringWithFormat:@"%s '%s' linkage rejected: %@",
                                           role, name ? name : "?", why]);
    }
    static NSMutableSet<NSString *> *seen; static NSLock *lk; static dispatch_once_t once; dispatch_once(&once, ^{ seen = [NSMutableSet set]; lk = [NSLock new]; });
    NSString *tag = [NSString stringWithFormat:@"%s|%s|%@|%@", role, name ? name : "?", (ctx && ctx->lsha) ? ctx->lsha : @"-", why ? why : @""]; BOOL first;
    [lk lock]; first = ![seen containsObject:tag]; if (first && seen.count < 1024) [seen addObject:tag]; [lk unlock];
    if (first) RDNA4LOG("LINKAGE %s '%s': functions[%lu] %s | privateFunctions[%lu] %s | groups %s | binaryFunctions %d | preloadedLibraries %d%s%s",
                      role, name ? name : "?", (unsigned long)[(NSArray *)rdna4x_prop(linked, "functions") count], rdna4x_names(rdna4x_prop(linked, "functions"), NO).UTF8String, (unsigned long)[(NSArray *)rdna4x_prop(linked, "privateFunctions") count], rdna4x_names(rdna4x_prop(linked, "privateFunctions"), NO).UTF8String,
                      rdna4x_names(gr, YES).length ? rdna4x_names(gr, YES).UTF8String : "-", bf, pl, (ctx && ctx->lsha) ? " -> resolved in process, key " : "", (ctx && ctx->lsha) ? ctx->lsha.UTF8String : "");
    if (ctx) rdna4_lk_install(slot, ctx);
    if ((!ctx || ctx->linkageError) && first) RDNA4LOG_WARN("LINKAGE IGNORED %s '%s': %s%s%s%s is set - linked functions / preloaded libraries are not resolved for this pipeline (%s)", role, name ? name : "?", f ? "linkedFunctions.functions " : "", bf ? "linkedFunctions.binaryFunctions " : "", pf ? "linkedFunctions.privateFunctions " : "", pl ? "preloadedLibraries" : "", why ? why.UTF8String : "unknown");
}
GARNET_INTERNAL void rdna4x_linkage_render(id d) {
    id vf = rdna4x_prop(d, "vertexFunction"), ff = rdna4x_prop(d, "fragmentFunction");
    rdna4x_linkage_check(d, "vertex", "vertexLinkedFunctions", "vertexPreloadedLibraries", [[rdna4x_prop(vf, "name") description] UTF8String], vf, 0);
    rdna4x_linkage_check(d, "fragment", "fragmentLinkedFunctions", "fragmentPreloadedLibraries", [[rdna4x_prop(ff, "name") description] UTF8String], ff, 1);
}
GARNET_INTERNAL void rdna4x_linkage_compute(id d) {
    id cf = rdna4x_prop(d, "computeFunction");
    rdna4x_linkage_check(d, "compute", "linkedFunctions", "preloadedLibraries", [[rdna4x_prop(cf, "name") description] UTF8String], cf, 2);
}

// High-level Stage 2 compile helper for compute/vertex/fragment stages with preloadedLibraries /
// binaryFunctions / MTLLinkedFunctions, populating translated SPIR-V, metadata, and parsed VFT table.
__attribute__((unused)) static BOOL rdna4x_compile_stage_with_linkage(id desc, id fn, const char *role, int slot,
                                              NSData **outSpv, NSDictionary **outMeta,
                                              rdna4vft_reflection_t *outVft, NSError **error) {
    if (outSpv) *outSpv = nil;
    if (outMeta) *outMeta = nil;
    if (outVft) rdna4vft_refl_init(outVft);
    if (error) *error = nil;
    if (!fn) {
        rdna4x_translation_error(error, role ?: "stage", nil, nil, RDNA4X_E_CAP, @"entry function is nil");
        return NO;
    }
    const char *linkedProp = (slot == 0) ? "vertexLinkedFunctions" : (slot == 1) ? "fragmentLinkedFunctions" : "linkedFunctions";
    const char *preloadProp = (slot == 0) ? "vertexPreloadedLibraries" : (slot == 1) ? "fragmentPreloadedLibraries" : "preloadedLibraries";
    NSString *name = [fn respondsToSelector:@selector(name)] ? [fn name] : @"?";
    RDNA4_LK_SCOPE;
    if (desc) rdna4x_linkage_check(desc, role ?: "stage", linkedProp, preloadProp, name.UTF8String, fn, slot);
    RDNA4LinkCtx *lk = rdna4_lk_for(fn);
    if (lk && lk->linkageError) {
        if (error) *error = lk->linkageError;
        return NO;
    }
    SEL bcSel = NSSelectorFromString(@"bitcodeData");
    NSData *bc = [fn respondsToSelector:bcSel] ? ((NSData *(*)(id, SEL))objc_msgSend)(fn, bcSel) : nil;
    if (!bc.length) {
        rdna4x_translation_error(error, role ?: "stage", name, nil, RDNA4X_E_CAP, @"function has no bitcodeData");
        return NO;
    }
    NSString *dir = rdna4x_user_dir() ?: (rdna4x_memcache_active() ? @RDNA4X_MEMCACHE_DIR : nil);
    if (!dir) {
        rdna4x_translation_error(error, role ?: "stage", name, nil, RDNA4X_E_UNAVAILABLE, @"in-process translation cache is unavailable");
        return NO;
    }
    NSString *sha = (lk && lk->lsha.length) ? lk->lsha : rdna4_sha256hex(bc);
    return rdna4x_translate_stage2_to_cache(dir, sha, name, role ?: "stage", bc, lk, 0u, outSpv, outMeta, outVft, error);
}


#endif // !defined(GARNET_SHADER_REQUEST_CAPTURE_ONLY)
