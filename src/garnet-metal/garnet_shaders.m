// garnet_shaders.m - GarnetMetal modular compilation unit.
#if !defined(GARNET_INTERNAL) && !defined(GARNET_METAL_MODULAR_BUILD)
#define GARNET_INTERNAL static
#endif
#if !defined(GARNET_SHADER_REQUEST_CAPTURE_ONLY)
#ifdef GARNET_METAL_MODULAR_BUILD
#include "garnet_metal_internal.h"
#endif
#endif

#if !defined(GARNET_SHADER_REQUEST_CAPTURE_ONLY)
// ---------------------------------------------------------------------------------------------------------------
// AIR dump for the pipeline-creation overrides (NATIVE-S4-M10 "Shaders": cache miss -> /tmp/rdna4m/<sha256>.air).
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
// 0.0.641 (G4 review LOW) kept, bundle build 6 (C1 finding A): the dump area is /tmp/rdna4m (sticky 01777 parent, rdna4g_dumproot_ok) and every uid dumps into ITS OWN subdirectory /tmp/rdna4m/<euid> (0700, a
// real directory owned by the caller, rdna4g_dumpdir_ok). No chmod ever follows a symlink; any failed check skips the dump (and says so once per reason in the log). The translate daemon's
// account is granted list+search on the subdirectory by an ACL entry on the open directory descriptor (garnet_dumpacl.h); a failed grant is logged and the dump still happens.
GARNET_INTERNAL int rdna4_dump_dir_ready(char *sub, size_t subn) {
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
static int rdna4x_capture_request(NSString *directory, NSString *sha, NSString *name,
    const char *role, NSData *air, NSArray *dependencies) {
    struct stat flag;
    if (lstat(RDNA4_SHADER_CAPTURE_FLAG, &flag) || !S_ISREG(flag.st_mode) ||
        (flag.st_uid != 0 && flag.st_uid != geteuid()) || !directory || !sha || !air.length) return 0;
    char requested[65];
    int marker = open(RDNA4_SHADER_CAPTURE_FLAG, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
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
// Bundle 14: in-process shader translation (an internal design note). The policy, the cache, the crash markers, the reader and the engine are in garnet_xlate.h (host tests test-xlate.c and
// test-xlate-corpus.sh run that very header); this is the glue. A spvcache miss in an ADMITTED APPLICATION (or an RDNA4M_ALLOW root tool with RDNA4M_TEST_INPROC=1) is translated inside the process by the
// operating system's own LLVM (the bitcode reader, loaded on the first miss only) and Resources/libgarnetxlate.dylib (the LGPL translator), and installed in the user's cache directory
// <confstr user cache dir>/org.garnet.xlate/<K>/. WindowServer NEVER applies: it keeps the precompiled spvcache plus the daemon and loads neither library.
//   /private/tmp/rdna4m-noinproc      exists (or cannot be read) -> in-process translation OFF for the process (read once); the dump + daemon path is then exactly bundle 13's
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
            struct stat st; const int rc = stat(RDNA4X_KILL_FILE, &st), er = errno;
            RDNA4XL.killed = rdna4x_killed_from_stat(rc, er);
            RDNA4XL.envDirty = rdna4x_env_dirty(*_NSGetEnviron());
            if (RDNA4XL.killed) RDNA4LOG("inproc: kill file " RDNA4X_KILL_FILE " %s: in-process translation is OFF for this process", rc == 0 ? "exists" : "could not be read (fail closed)");
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
static NSString *rdna4x_xlib_path(void) { return [rdna4x_resources() stringByAppendingPathComponent:@RDNA4X_LIB_NAME]; }
static rdna4x_backend_capabilities rdna4_rt_xlate_caps(void) {
    rdna4x_backend_capabilities caps = rdna4_query_caps();
    if (RDNA4R.rtPositionFetch) caps.flags |= GARNETX_CAP_RT_POSITION_FETCH;
    caps.flags |= GARNETX_CAP_RT_IFT_ABI_V2;
    if (RDNA4R.accelStruct && RDNA4R.rayQuery && RDNA4R.bufferAddress)
        caps.flags |= GARNETX_CAP_RT_PRIMITIVE_DATA;
    if (RDNA4R.shaderInt64)
        caps.flags |= GARNETX_CAP_NATIVE_INT64;
    return caps;
}
// Sets up <user cache dir>/org.garnet.xlate/<K> once, after admission (never inside rdna4_spv_dirs' dispatch_once). Caller holds RDNA4XL.mu.
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
    char parent[RDNA4X_PATH_MAX];
    if (!rdna4x_parent_path(parent, sizeof parent, ucd) || !rdna4x_dir_path(RDNA4XL.dir, sizeof RDNA4XL.dir, ucd, key)) { RDNA4XL.dirState = -1; RDNA4LOG_WARN("inproc: no user cache directory: in-process translation is unavailable"); return; }
    if (rdna4x_prepare_dirs(parent, RDNA4XL.dir, key) != 0) { RDNA4XL.dirState = -1; RDNA4LOG_WARN("inproc: cannot use the cache directory %s (not a real directory of ours?): in-process translation is unavailable", RDNA4XL.dir); return; }
    RDNA4XL.dirNS = [NSString stringWithUTF8String:RDNA4XL.dir];
    RDNA4XL.dirState = 1;
    RDNA4LOG("inproc: enabled for %s (%s); cache %s (key %s); reader: the OS's GPUCompiler libLLVM, loaded on the first miss", getprogname(), rdna4_is_ws() ? "WindowServer?!" : rdna4_allow() ? "root test" : "application", RDNA4XL.dir, key);
}
// The user cache directory K, or nil (not applicable, or unavailable).
GARNET_INTERNAL NSString *rdna4x_user_dir(void) {
    if (!rdna4x_active()) return nil;
    if (!rdna4_radv_open(NULL)) return nil;
    pthread_mutex_lock(&RDNA4XL.mu);
    if (RDNA4XL.dirState == 0 ||
        (RDNA4XL.dirState == -1 && RDNA4XL.dirTries < 3 && rdna4_now() >= RDNA4XL.dirNextNs)) {
        RDNA4XL.dirTries++;
        rdna4x_setup_locked();
        if (RDNA4XL.dirState != 1) RDNA4XL.dirNextNs = rdna4_now() + 2000000000ull;
    }
    NSString *r = RDNA4XL.dirState == 1 ? RDNA4XL.dirNS : nil;
    pthread_mutex_unlock(&RDNA4XL.mu);
    return r;
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
    const BOOL ok = RDNA4XL.dirState == 1;
    pthread_mutex_unlock(&RDNA4XL.mu);
    return ok;
}
static BOOL rdna4x_sandboxed(void) {
    pthread_mutex_lock(&RDNA4XL.mu);
    if (!RDNA4XL.sandboxedDone) { RDNA4XL.sandboxed = sandbox_check(getpid(), NULL, 0) != 0; RDNA4XL.sandboxedDone = 1; }
    const BOOL r = RDNA4XL.sandboxed;
    pthread_mutex_unlock(&RDNA4XL.mu);
    return r;
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
// ---- build 18 (P3): linked functions (NATIVE-S8-APPFIX1.md section P3) ----
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
static void rdna4_sync_note_async_success(void) {
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
    if (!RDNA4XL.engineInit) {
        const char *rl = rdna4_allow() ? garnet_getenv("GARNET_TEST_LLVM_LIB", "RDNA4M_TEST_LLVM_LIB") : NULL;
        rdna4x_engine_init(&RDNA4XL.eng, NULL, NULL, rl, rdna4x_xlib_path().fileSystemRepresentation, RDNA4X_WORKER_STACK);
        RDNA4XL.eng.lowerTranslatedArguments = RDNA4R.gpuArgumentTables;
        RDNA4XL.eng.capabilities = rdna4_rt_xlate_caps();
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

__attribute__((unused)) static BOOL rdna4x_list_exported_symbols_for_module(const uint8_t *bytes, size_t len, uint32_t payloadKind,
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

    // Publish .meta.json FIRST, .spv LAST (matches rdna4x_process_job contract).
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
    if (failPath[0] && w->err[0]) (void)rdna4x_write_atomic(failPath, w->err, strlen(w->err));
done:
    free(entryLl);
    if (depTexts) { for (size_t i = 0; i < w->ndeps; i++) free(depTexts[i]); free(depTexts); }
    if (libTexts) { for (size_t i = 0; i < w->ndynlibs; i++) free(libTexts[i]); free(libTexts); }
    if (visTexts) { for (size_t i = 0; i < w->nvis; i++) free(visTexts[i]); free(visTexts); }
    if (iftTexts) { for (size_t i = 0; i < w->nift; i++) free(iftTexts[i]); free(iftTexts); }
    free(cdeps); free(clibs); free(cvis); free(cift);
    if (spv) w->xlib->freep(spv, spvLen);
    if (meta) w->xlib->freep(meta, metaLen);
    rdna4x_marker_remove(w->dir, w->sha);
    if (rc == RDNA4X_OK) (void)rdna4x_trim_dir(w->dir, RDNA4X_CACHE_CAP);
    w->rc = rc;
    return NULL;
}

static BOOL rdna4x_translate_stage2_to_cache(NSString *dir, NSString *sha, NSString *name, const char *role,
                                             NSData *entryAir, RDNA4LinkCtx *lk,
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

    char spvp[RDNA4X_PATH_MAX], metap[RDNA4X_PATH_MAX];
    struct stat sb;
    if (rdna4x_file_path(spvp, sizeof spvp, dir.fileSystemRepresentation, sha.UTF8String, RDNA4X_EXT_SPV) &&
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
        dir = rdna4x_user_dir() ?: dir;
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
GARNET_INTERNAL BOOL rdna4x_translate_fn_with_error(id fn, const char *role, uint64_t deadlineNs, NSError **error) {
    if (error) *error = nil;
    NSString *name = fn && [fn respondsToSelector:@selector(name)] ? [fn name] : @"?";
    RDNA4LinkCtx *lk = rdna4_lk_for(fn);   // build 18 (P3) + Metal 3 Stage 2 linkage
    if (lk && lk->linkageError) {
        if (error) *error = lk->linkageError;
        return NO;
    }
    NSString *dir = rdna4x_user_dir();
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
    rdna4x_capture_request(dir, sha, name, role, bc, lk ? lk->deps : nil);
    char spvp[RDNA4X_PATH_MAX]; struct stat sb;
    if (rdna4x_file_path(spvp, sizeof spvp, dir.fileSystemRepresentation, sha.UTF8String, RDNA4X_EXT_SPV) && stat(spvp, &sb) == 0 && S_ISREG(sb.st_mode)) return YES;   // installed meanwhile (another thread / process)
    NSError *backendError = nil;
    if (!rdna4_radv_open(&backendError)) {
        rdna4x_translation_error(error, role, name, sha, RDNA4X_E_UNAVAILABLE, backendError.localizedDescription);
        return NO;
    }
    if (lk && (lk->useStage2 || lk->useStage3) && (lk->requireStage2 || rdna4x_stage2_symbols_available())) {
        return rdna4x_translate_stage2_to_cache(dir, sha, name, role, bc, lk, NULL, NULL, NULL, error);
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
            if (ld) rc = rdna4x_engine_run_linked(&RDNA4XL.eng, dir.fileSystemRepresentation, sha.UTF8String, name.UTF8String, bc.bytes, bc.length, ld, nd, waitMs, err, sizeof err);
            else { rc = RDNA4X_E_IO; strlcpy(err, "out of memory allocating linked dependencies", sizeof err); }
            free(ld);
        } else
            rc = rdna4x_engine_run(&RDNA4XL.eng, dir.fileSystemRepresentation, sha.UTF8String, name.UTF8String, bc.bytes, bc.length, waitMs, err, sizeof err);
        if (rc != RDNA4X_E_IO || !rdna4x_dir_recheck()) break;
        RDNA4LOG("inproc RETRY %s '%s' %s: the cache directory was rebuilt after: %s", role, name.UTF8String, sha.UTF8String, err);
    }
    if (waiting) rdna4_sync_end(rc == RDNA4X_OK, rc == RDNA4X_E_TIMEOUT);
    const double ms = (double)(rdna4_now() - t0) / 1e6;
    pthread_mutex_lock(&RDNA4XL.mu);
    if (!rdna4x_logged) rdna4x_logged = [NSMutableSet set];
    const BOOL firstLog = ![rdna4x_logged containsObject:sha]; if (firstLog) [rdna4x_logged addObject:sha];
    pthread_mutex_unlock(&RDNA4XL.mu);
    if (rc == RDNA4X_OK) RDNA4LOG("inproc OK %s '%s' %s: translated in %.0f ms", role, name.UTF8String, sha.UTF8String, ms);
    else if (rc == RDNA4X_E_TIMEOUT) RDNA4LOG_WARN("inproc TIMEOUT %s '%s' %s: compilation did not finish within %d ms", role, name.UTF8String, sha.UTF8String, waitMs);
    else if (firstLog) RDNA4LOG_ERR("inproc FAIL %s '%s' %s: code %d: %s", role, name.UTF8String, sha.UTF8String, rc, err);
    if (rc != RDNA4X_OK) {
        NSString *reason = rc == RDNA4X_E_TIMEOUT
            ? [NSString stringWithFormat:@"translation exceeded the %d ms wait deadline", waitMs]
            : [NSString stringWithUTF8String:err[0] ? err : "translation failed without an engine reason"];
        if (rc == RDNA4X_E_SKIP_FAIL) {
            NSString *cached = rdna4x_cached_refusal(dir, sha);
            if (cached.length) reason = [NSString stringWithFormat:@"the translator refused before: %@", cached];
        }
        rdna4x_translation_error(error, role, name, sha, rc, reason);
    }
    return rc == RDNA4X_OK;
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
    NSString *dir = rdna4x_user_dir();
    if (!dir) {
        rdna4x_translation_error(error, role ?: "stage", name, nil, RDNA4X_E_UNAVAILABLE, @"in-process translation cache is unavailable");
        return NO;
    }
    NSString *sha = (lk && lk->lsha.length) ? lk->lsha : rdna4_sha256hex(bc);
    return rdna4x_translate_stage2_to_cache(dir, sha, name, role ?: "stage", bc, lk, outSpv, outMeta, outVft, error);
}

// ---- spvcache sidecars (<sha>.meta.json, add-air.py) ----
// Hot-swap H2: the lookup directories, in order: the bundle's Resources/spvcache (read-only in practice), then (bundle 14, applications that translate in process) the user cache directory K,
// then the side directory /private/var/tmp/garnet-spv (WindowServer-sandbox readable; the profile allows /private/var/tmp; the translator installs new <sha>.meta.json then
// <sha>.spv there by atomic rename, .spv LAST). A sandboxed application does not look in the side directory (the sandbox denies it and logs every try).
// Test hooks (root + GARNET_ALLOW=1): GARNET_TEST_SIDE_DIR replaces the side dir, GARNET_TEST_HIDE_BUNDLE_SPV=1 drops the bundle dir.
static NSString *rdna4_side_dir(void) {
    static NSString *d; static dispatch_once_t o;
    dispatch_once(&o, ^{ const char *t = garnet_getenv("GARNET_TEST_SIDE_DIR", "RDNA4M_TEST_SIDE_DIR"); d = (rdna4_allow() && t && *t) ? @(t) : @"/private/var/tmp/garnet-spv"; });
    return d;
}
GARNET_INTERNAL NSArray<NSString *> *rdna4_spv_dirs(void) {
    static NSArray *bundleOnly, *a[4]; static dispatch_once_t o; static NSString *userFor;
    dispatch_once(&o, ^{
        const char *h = garnet_getenv("GARNET_TEST_HIDE_BUNDLE_SPV", "RDNA4M_TEST_HIDE_BUNDLE_SPV");
        if (!(rdna4_allow() && h && !strcmp(h, "1"))) bundleOnly = @[ [[[NSBundle bundleForClass:[GarnetDevice class]] resourcePath] stringByAppendingPathComponent:@"spvcache"] ];
        else bundleOnly = @[];
    });
    NSString *u = rdna4x_user_dir();                                   // nil unless this process translates in process
    const BOOL skipSide = u && rdna4x_skip_side_dir(rdna4_is_ws(), rdna4x_sandboxed());
    const int idx = (u ? 1 : 0) | (skipSide ? 2 : 0);
    @synchronized ([GarnetDevice class]) {
        if (!a[idx] || (u && ![userFor isEqualToString:u])) {
            NSMutableArray *m = [bundleOnly mutableCopy];
            if (u) { [m addObject:u]; userFor = u; }
            if (!skipSide) [m addObject:rdna4_side_dir()];
            a[idx] = m;
        }
        return a[idx];
    }
}
GARNET_INTERNAL NSDictionary *rdna4_meta_for_spv(NSString *path) {
    NSString *metadataPath = [[path stringByDeletingPathExtension] stringByAppendingString:@".meta.json"];
    NSData *d = [NSData dataWithContentsOfFile:metadataPath];
    if (!d.length) return nil;
    id j = [NSJSONSerialization JSONObjectWithData:d options:0 error:NULL];
    return [j isKindOfClass:[NSDictionary class]] ? j : nil;
}
static int rdna4_idx(id s, const char *const *tbl) {
    if (![s isKindOfClass:[NSString class]]) return -1;
    for (int i = 0; tbl[i]; i++) if (!strcmp(tbl[i], [(NSString *)s UTF8String])) return i;
    return -1;
}
// Static (constexpr) sampler state from a reflection entry -> RDNA4SampCfg (Metal enum values).
static BOOL rdna4_static_cfg(NSDictionary *ss, RDNA4SampCfg *c, NSError **err) {
    static const char *const filt[] = { "Nearest", "Linear", NULL }, *const mip[] = { "None", "Nearest", "Linear", NULL };
    static const char *const addr[] = { "ClampToEdge", "MirrorClampToEdge", "Repeat", "MirroredRepeat", "ClampToZero", "ClampToBorder", NULL };   // index = MTLSamplerAddressMode
    static const char *const cmpf[] = { "None", "Less", "Equal", "LessEqual", "Greater", "NotEqual", "GreaterEqual", "Always", "Never", NULL };
    static const char *const bord[] = { "TransparentBlack", "OpaqueBlack", "OpaqueWhite", NULL };
    static const char *const coord[] = { "Normalized", "Pixel", NULL };
    int mf = rdna4_idx(ss[@"min_filter"], filt), gf = rdna4_idx(ss[@"mag_filter"], filt), mp = rdna4_idx(ss[@"mip_filter"], mip), a1 = rdna4_idx(ss[@"address_mode_s"], addr),
        a2 = rdna4_idx(ss[@"address_mode_t"], addr), a3 = rdna4_idx(ss[@"address_mode_r"], addr), cf = rdna4_idx(ss[@"compare_function"], cmpf), bc = rdna4_idx(ss[@"border_color"], bord), co = rdna4_idx(ss[@"coordinates"], coord);
    id lo = ss[@"lod_min_clamp"], hi = ss[@"lod_max_clamp"];
    uint64_t anisotropy = 1;
    BOOL validNumbers = lo && hi && CFGetTypeID((__bridge CFTypeRef)lo) == CFNumberGetTypeID() &&
        CFGetTypeID((__bridge CFTypeRef)hi) == CFNumberGetTypeID() &&
        (!ss[@"max_anisotropy"] || (rdna4_number_u64((__bridge CFTypeRef)ss[@"max_anisotropy"], &anisotropy) && anisotropy <= UINT_MAX));
    if (mf < 0 || gf < 0 || mp < 0 || a1 < 0 || a2 < 0 || a3 < 0 || cf < 0 || bc < 0 || co < 0 || !validNumbers) {
        if (err) *err = rdna4_err(73, [NSString stringWithFormat:@"static sampler state not understood: %@", ss]); return NO; }
    *c = (RDNA4SampCfg){ .minF = mf, .magF = gf, .mipF = mp, .as = a1, .at = a2, .ar = a3, .border = bc,
        .cmp = cf == 8 ? 0 : cf, .unnorm = co == 1, .lodMin = [lo floatValue], .lodMax = [hi floatValue],
        .anisotropy = (unsigned)anisotropy };
    // JSON compare index -> MTLCompareFunction: Less 1, Equal 2, LessEqual 3, Greater 4, NotEqual 5, GreaterEqual 6, Always 7 (== table index for 1..7)
    return YES;
}

typedef struct {
    uint32_t value[RDNA4_AS_ROOTS];
    size_t count;
    uint64_t dynamicMask;
} RDNA4ArgumentScanRoots;

GARNET_INTERNAL const char *rdna4_argument_scan_status_name(rdna4_as_status status) {
    switch (status) {
    case RDNA4_AS_OK: return "ok";
    case RDNA4_AS_INVALID: return "invalid";
    case RDNA4_AS_CAPACITY: return "capacity";
    case RDNA4_AS_OVERFLOW: return "overflow";
    case RDNA4_AS_NOT_FINALIZED: return "not-finalized";
    }
    return "unknown";
}

static BOOL rdna4_argument_scan_root_add(RDNA4ArgumentScanRoots *roots,
                                          uint64_t value,
                                          NSError **error) {
    if (value >= RDNA4_AS_ROOTS) {
        if (error) *error = rdna4_err(49,
            [NSString stringWithFormat:@"argument scan root %llu is outside 0..%u",
             (unsigned long long)value, RDNA4_AS_ROOTS - 1u]);
        return NO;
    }
    for (size_t i = 0; i < roots->count; ++i)
        if (roots->value[i] == (uint32_t)value) return YES;
    roots->value[roots->count++] = (uint32_t)value;
    return YES;
}

static void rdna4_argument_scan_field_add(NSMutableData *fields,
                                           uint32_t root, uint32_t kind,
                                           uint64_t offset, uint64_t count,
                                           uint64_t stride, uint32_t known) {
    rdna4_argument_scan_reflection_field field = {
        .root = root,
        .kind = kind,
        .count_known = known,
        .writable = 1,
        .field_offset = offset,
        .count = count,
        .stride = stride ? stride : RDNA4_AS_TOKEN_BYTES,
    };
    [fields appendBytes:&field length:sizeof field];
}

static BOOL rdna4_argument_scan_optional_u64(id value, uint64_t *out) {
    return value && value != [NSNull null] &&
        rdna4_number_u64((__bridge CFTypeRef)value, out);
}

static BOOL rdna4_argument_scan_optional_stride(id descriptor, id source,
                                                uint64_t *stride) {
    uint64_t candidate = 0;
    BOOL present = NO;
    if ([descriptor isKindOfClass:[NSDictionary class]]) {
        present = rdna4_argument_scan_optional_u64(
            descriptor[@"array_stride"] ?: descriptor[@"element_stride"] ?:
                descriptor[@"stride"],
            &candidate);
    }
    if (!present && [source isKindOfClass:[NSDictionary class]])
        present = rdna4_argument_scan_optional_u64(
            source[@"array_stride"] ?: source[@"element_stride"] ?:
                source[@"record_stride"] ?: source[@"stride"],
            &candidate);
    if (!present) return YES;
    if (candidate < RDNA4_AS_TOKEN_BYTES ||
        candidate % RDNA4_AS_TOKEN_BYTES)
        return NO;
    *stride = candidate;
    return YES;
}

static BOOL rdna4_argument_scan_collect_metadata(NSDictionary *metadata,
                                                 NSMutableData *fields,
                                                 RDNA4ArgumentScanRoots *roots,
                                                 BOOL includeFields,
                                                 NSError **error) {
    if (!metadata) return YES;
    if (![metadata isKindOfClass:[NSDictionary class]]) {
        if (error) *error = rdna4_err(49, @"argument scan metadata is not an object");
        return NO;
    }
    id bindings = metadata[@"bindings"];
    if (bindings && ![bindings isKindOfClass:[NSArray class]]) {
        if (error) *error = rdna4_err(49, @"argument scan bindings are not an array");
        return NO;
    }
    id declaredRoots = metadata[@"argument_buffer_roots"];
    if (declaredRoots) {
        uint64_t version = 0, previous = 0;
        if (![declaredRoots isKindOfClass:[NSArray class]] ||
            !rdna4_number_u64((__bridge CFTypeRef)metadata[@"trimmed_from_reflection_version"], &version) ||
            version != 66) {
            if (error) *error = rdna4_err(49, @"invalid declared argument-buffer roots");
            return NO;
        }
        BOOL first = YES;
        for (id value in declaredRoots) {
            uint64_t root = 0;
            BOOL matched = NO;
            if (!rdna4_number_u64((__bridge CFTypeRef)value, &root) ||
                root >= RDNA4_AS_ROOTS || (!first && root <= previous)) {
                if (error) *error = rdna4_err(49, @"invalid declared argument-buffer root index");
                return NO;
            }
            for (id row in bindings) {
                uint64_t index = 0, param = 0, set = 0, binding = 0, count = 0;
                if (![row isKindOfClass:[NSDictionary class]] ||
                    ![row[@"descriptor"] isKindOfClass:[NSDictionary class]]) continue;
                id descriptor = row[@"descriptor"];
                matched |= [row[@"kind"] isEqual:@"Buffer"] &&
                    rdna4_number_u64((__bridge CFTypeRef)row[@"metal_index"], &index) && index == root &&
                    rdna4_number_u64((__bridge CFTypeRef)row[@"param_index"], &param) &&
                    rdna4_number_u64((__bridge CFTypeRef)descriptor[@"set"], &set) && set == 0 &&
                    rdna4_number_u64((__bridge CFTypeRef)descriptor[@"binding"], &binding) && binding == root &&
                    rdna4_number_u64((__bridge CFTypeRef)descriptor[@"count"], &count) && count == 1;
            }
            if (!matched || !rdna4_argument_scan_root_add(roots, root, error)) {
                if (error) *error = rdna4_err(49, @"declared argument-buffer root has no matching buffer");
                return NO;
            }
            first = NO;
            previous = root;
        }
    }
    for (id binding in bindings) {
        if (![binding isKindOfClass:[NSDictionary class]]) {
            if (error) *error = rdna4_err(49, @"argument scan contains a non-object binding");
            return NO;
        }
        NSString *bindingKind = binding[@"kind"];
        if (![bindingKind isKindOfClass:[NSString class]]) continue;
        uint32_t kind = RDNA4_AS_KIND_UNKNOWN;
        if ([bindingKind isEqual:@"EmbeddedArgBufferTexture"])
            kind = RDNA4_AS_KIND_TEXTURE;
        else if ([bindingKind isEqual:@"EmbeddedArgBufferSampler"])
            kind = RDNA4_AS_KIND_SAMPLER;
        else if (![bindingKind isEqual:@"EmbeddedArgBufferBuffer"])
            continue;

        id source = binding[@"embedded_source"];
        uint64_t root = 0, offset = 0;
        if (![source isKindOfClass:[NSDictionary class]] ||
            !rdna4_number_u64((__bridge CFTypeRef)source[@"buffer_index"], &root) ||
            !rdna4_argument_scan_root_add(roots, root, error)) {
            if (error) *error = rdna4_err(49,
                [NSString stringWithFormat:@"%@ has no valid argument-buffer root",
                 bindingKind]);
            return NO;
        }
        BOOL validOffset = rdna4_number_u64((__bridge CFTypeRef)source[@"field_offset"],
                                            &offset) &&
            !(offset & (RDNA4_AS_TOKEN_BYTES - 1u));
        if (!includeFields) continue;

        id descriptor = binding[@"descriptor"];
        uint64_t count = 0, stride = RDNA4_AS_TOKEN_BYTES;
        BOOL countKnown = NO;
        if (kind == RDNA4_AS_KIND_BUFFER &&
            (![descriptor isKindOfClass:[NSDictionary class]] ||
             !rdna4_number_u64((__bridge CFTypeRef)descriptor[@"count"],
                               &count))) {
            /*
             * EmbeddedArgBufferBuffer is a single BDA field even though it
             * has no opaque-resource descriptor row. Treating its absent
             * descriptor as an unknown array used to force a whole-root scan.
             */
            count = 1;
            countKnown = YES;
        } else if ([descriptor isKindOfClass:[NSDictionary class]] &&
                   rdna4_number_u64((__bridge CFTypeRef)descriptor[@"count"],
                                    &count) && count) {
            countKnown = YES;
        }
        if (!rdna4_argument_scan_optional_stride(descriptor, source, &stride))
            countKnown = NO;
        if (validOffset && countKnown)
            rdna4_argument_scan_field_add(fields, (uint32_t)root, kind, offset,
                                          count, stride, 1);
        else
            rdna4_argument_scan_field_add(fields, (uint32_t)root, kind, 0, 0,
                                          stride, 0);
    }

    /*
     * argument_buffer_fields retains resource-handle coordinates even when a
     * descriptor has no GPU-table root entry. It also covers buffer-address
     * fields that are not opaque texture/sampler tokens, so those bytes remain
     * transport evidence rather than disappearing from the declared root.
     */
    id reflectedFields = metadata[@"argument_buffer_fields"];
    if (reflectedFields && ![reflectedFields isKindOfClass:[NSArray class]]) {
        if (error) *error = rdna4_err(49,
            @"argument_buffer_fields is not an array");
        return NO;
    }
    for (id row in reflectedFields) {
        uint64_t root = 0, offset = 0;
        if (![row isKindOfClass:[NSDictionary class]] ||
            !rdna4_number_u64((__bridge CFTypeRef)row[@"buffer_index"], &root) ||
            !rdna4_argument_scan_root_add(roots, root, error)) {
            if (error) *error = rdna4_err(49,
                @"argument_buffer_fields contains an invalid root");
            return NO;
        }
        if (!includeFields) continue;
        id resourceBufferIndex = row[@"resource_buffer_index"];
        uint64_t resourceBuffer = 0;
        BOOL bufferField = resourceBufferIndex && resourceBufferIndex != [NSNull null] &&
            rdna4_number_u64((__bridge CFTypeRef)resourceBufferIndex,
                             &resourceBuffer);
        (void)resourceBuffer;
        /*
         * argument_buffer_fields also carries scalar struct members. Opaque
         * texture/sampler coordinates come from bindings above; only a BDA
         * buffer-resource field is an additional token-bearing coordinate.
         */
        if (!bufferField) continue;
        id countValue = row[@"array_count"] ?: row[@"element_count"];
        id strideValue = row[@"array_stride"] ?: row[@"element_stride"] ?:
            row[@"record_stride"] ?: row[@"stride"];
        uint64_t count = 1, stride = RDNA4_AS_TOKEN_BYTES;
        BOOL countKnown = YES;
        if (countValue &&
            (!rdna4_argument_scan_optional_u64(countValue, &count) || !count))
            countKnown = NO;
        if (strideValue &&
            (!rdna4_argument_scan_optional_u64(strideValue, &stride) ||
             stride < RDNA4_AS_TOKEN_BYTES ||
             stride % RDNA4_AS_TOKEN_BYTES))
            countKnown = NO;
        if (rdna4_number_u64((__bridge CFTypeRef)row[@"field_offset"], &offset) &&
            !(offset & (RDNA4_AS_TOKEN_BYTES - 1u)) && countKnown)
            rdna4_argument_scan_field_add(fields, (uint32_t)root,
                                          RDNA4_AS_KIND_BUFFER,
                                          offset, count, stride, 1);
        else
            rdna4_argument_scan_field_add(fields, (uint32_t)root,
                                          RDNA4_AS_KIND_UNKNOWN, 0, 0,
                                          stride, 0);
    }

    id table = metadata[@"argument_resource_tables"];
    if (table && ![table isKindOfClass:[NSDictionary class]]) {
        if (error) *error = rdna4_err(49,
            @"argument_resource_tables is not an object");
        return NO;
    }
    id tableRoots = [table isKindOfClass:[NSDictionary class]] ? table[@"roots"] : nil;
    if (tableRoots && ![tableRoots isKindOfClass:[NSArray class]]) {
        if (error) *error = rdna4_err(49,
            @"argument_resource_tables.roots is not an array");
        return NO;
    }
    for (id row in tableRoots) {
        uint64_t root = 0;
        if (![row isKindOfClass:[NSDictionary class]] ||
            !rdna4_number_u64((__bridge CFTypeRef)row[@"buffer_index"], &root) ||
            !rdna4_argument_scan_root_add(roots, root, error)) {
            if (error) *error = rdna4_err(49,
                @"argument_resource_tables contains an invalid root");
            return NO;
        }
        roots->dynamicMask |= UINT64_C(1) << (uint32_t)root;
    }
    return YES;
}

GARNET_INTERNAL BOOL rdna4_argument_scan_plan_build(rdna4_argument_scan_reflection_plan *out,
                                           NSDictionary *beforeLowering,
                                           NSDictionary *afterLowering,
                                           const char *role,
                                           NSError **error) {
    NSMutableData *fields = [NSMutableData data];
    RDNA4ArgumentScanRoots roots = {0};
    if (!rdna4_argument_scan_collect_metadata(beforeLowering, fields, &roots, YES, error) ||
        !rdna4_argument_scan_collect_metadata(afterLowering, fields, &roots, NO, error))
        return NO;
    for (uint32_t root = 0; root < RDNA4_AS_ROOTS; ++root)
        if (roots.dynamicMask & (UINT64_C(1) << root))
            rdna4_argument_scan_field_add(fields, root, RDNA4_AS_KIND_UNKNOWN,
                                          0, 0, RDNA4_AS_TOKEN_BYTES, 0);
    size_t fieldCount = fields.length / sizeof(rdna4_argument_scan_reflection_field);
    if (fieldCount * sizeof(rdna4_argument_scan_reflection_field) != fields.length) {
        if (error) *error = rdna4_err(49, @"argument scan field storage is misaligned");
        return NO;
    }
    rdna4_as_status status = rdna4_argument_scan_reflection_plan_build(
        out, fields.length ? fields.bytes : NULL, fieldCount,
        roots.count ? roots.value : NULL, roots.count);
    if (status != RDNA4_AS_OK) {
        if (error) *error = rdna4_err(49,
            [NSString stringWithFormat:@"%s argument scan plan: %s", role,
             rdna4_argument_scan_status_name(status)]);
        return NO;
    }
    size_t fallbackRoots = 0;
    for (size_t i = 0; i < out->plan.count; ++i)
        if (out->plan.spans[i].flags & RDNA4_AS_FALLBACK) ++fallbackRoots;
    if (out->plan.count || out->plan.stats.fields_seen)
        RDNA4LOG("argscan plan %s: %llu field(s), %llu element(s), %zu span(s), %llu coalesced, %llu fallback-root candidate(s), overflow %llu, capacity %llu",
                 role, (unsigned long long)out->plan.stats.fields_seen,
                 (unsigned long long)out->plan.stats.elements_seen,
                 out->plan.count,
                 (unsigned long long)out->plan.stats.spans_coalesced,
                 (unsigned long long)fallbackRoots,
                 (unsigned long long)out->plan.stats.overflow_rejections,
                 (unsigned long long)out->plan.stats.capacity_rejections);
    return YES;
}

// One descriptor binding of a stage, from SPIR-V reflection (+ the meta sidecar for static samplers).


GARNET_INTERNAL BOOL rdna4_coordinate_band_index(const RDNA4PB *source, uint32_t element,
                                        uint64_t *index) {
    if (!source || source->type != VK_DESCRIPTOR_TYPE_SAMPLER ||
        element >= source->count) return NO;
    uint32_t base;
    if (source->binding >= 160 && source->binding < 192) base = 160;
    else if (source->binding >= 736 && source->binding < 768) base = 736;
    else return NO;
    uint64_t value = (uint64_t)(source->binding - base) + element;
    if (value >= 32) return NO;
    *index = value;
    return YES;
}

GARNET_INTERNAL RDNA4CoordinateFlagResult rdna4_coordinate_flag_merge(
        uint32_t flags[32], BOOL assigned[32], uint64_t index, BOOL pixel) {
    if (index >= 32) return RDNA4CoordinateFlagRange;
    uint32_t value = pixel ? 1u : 0u;
    if (assigned[index] && flags[index] != value)
        return RDNA4CoordinateFlagConflict;
    assigned[index] = YES;
    flags[index] = value;
    return RDNA4CoordinateFlagOK;
}

@class RDNA4ArgumentWriteIntent;

















// Rewrites every `OpDecorate <id> DescriptorSet 0` of a module to `set` (see the binding-model comment above).
GARNET_INTERNAL NSData *rdna4_spv_set(NSData *spv, uint32_t set) {
    NSMutableData *m = [spv mutableCopy]; uint32_t *w = m.mutableBytes; size_t nw = m.length / 4, p = 5;
    while (p < nw) {
        uint32_t op = w[p] & 0xffff, wc = w[p] >> 16; if (!wc || p + wc > nw) break;
        if (op == 71 && wc >= 4 && w[p + 2] == 34 && w[p + 3] == 0) w[p + 3] = set;
        p += wc;
    }
    return m;
}

// A fragment module starts at set 0 like every independently translated stage;
// relocate stage-local direct sampler metadata with the same 0 -> 1 rule as the SPIR-V.
static BOOL rdna4_stage_meta_sampler_row(NSDictionary *row) {
    NSString *kind = row[@"kind"];
    if (![kind isKindOfClass:[NSString class]] ||
        !([kind isEqual:@"Sampler"] || [kind isEqual:@"StaticSampler"] ||
          [kind isEqual:@"SynthesizedReadSampler"] ||
          [kind isEqual:@"EmbeddedArgBufferSampler"])) return NO;
    id descriptor = row[@"descriptor"];
    uint64_t binding = 0;
    return [descriptor isKindOfClass:[NSDictionary class]] &&
        rdna4_number_u64((__bridge CFTypeRef)descriptor[@"binding"], &binding) &&
        binding >= 160 && binding < 192;
}

static NSDictionary *rdna4_stage_meta_relocate(NSDictionary *meta, int set) {
    if (!meta || set == 0) return meta;
    id bindings = meta[@"bindings"];
    if (![bindings isKindOfClass:[NSArray class]]) return meta;
    NSMutableDictionary *out = [meta mutableCopy];
    NSMutableArray *rows = [NSMutableArray arrayWithCapacity:[bindings count]];
    for (id row in bindings) {
        if (![row isKindOfClass:[NSDictionary class]]) {
            [rows addObject:row];
            continue;
        }
        NSMutableDictionary *relocated = [row mutableCopy];
        BOOL coordinate = [relocated[@"kind"] isEqual:@"SamplerCoordinateFlags"] ||
            [relocated[@"kind"] isEqual:@"StageResourceMetadata"] ||
            [relocated[@"kind"] isEqual:@"RTPrimDataTable"];
        BOOL sampler = rdna4_stage_meta_sampler_row(relocated);
        if (!coordinate && !sampler) {
            [rows addObject:relocated];
            continue;
        }
        for (NSString *key in @[ @"descriptor", @"comparison_descriptor" ]) {
            id descriptor = relocated[key];
            if (![descriptor isKindOfClass:[NSDictionary class]]) continue;
            uint64_t descriptorSet = 0;
            if (rdna4_number_u64((__bridge CFTypeRef)descriptor[@"set"],
                                 &descriptorSet) && descriptorSet == 0) {
                NSMutableDictionary *copy = [descriptor mutableCopy];
                copy[@"set"] = @(set);
                relocated[key] = copy;
            }
        }
        [rows addObject:relocated];
    }
    out[@"bindings"] = rows;
    return out;
}

// One stage's descriptor-set layout for set `set` from its reflection; pb/npb = the stage's binding list (malloc'd). r may be NULL (no stage).
GARNET_INTERNAL BOOL rdna4_stage_dsl(const RDNA4sRefl *r, int set, VkShaderStageFlags stage, NSDictionary *meta, VkDescriptorSetLayout *dsl, RDNA4PB **pbOut, uint32_t *npb,
                          NSMutableArray *ownedSamplers, NSError **err) {
    return rdna4_stage_dsl_ex(r, set, stage, meta, dsl, pbOut, npb, ownedSamplers, NO, NULL, err);
}
GARNET_INTERNAL BOOL rdna4_stage_dsl_ex(const RDNA4sRefl *r, int set, VkShaderStageFlags stage, NSDictionary *meta, VkDescriptorSetLayout *dsl, RDNA4PB **pbOut, uint32_t *npb,
                          NSMutableArray *ownedSamplers, BOOL allowPush, BOOL *usedPush, NSError **err) {
    *pbOut = NULL; *npb = 0; if (usedPush) *usedPush = NO;
    if (r && (r->descriptor_features & RDNA4S_IMAGE_GATHER_EXTENDED) &&
        !rdna4_device_gather_caps().supported) {
        if (err) *err = rdna4_err(49,
            @"shader requires shaderImageGatherExtended unavailable on this backend");
        return NO;
    }
    uint32_t descriptorFeatures = (RDNA4R.nonUniformSampledImages ? RDNA4S_NONUNIFORM_SAMPLED : 0u) |
        (RDNA4R.nonUniformStorageImages ? RDNA4S_NONUNIFORM_STORAGE : 0u) |
        (RDNA4R.nonUniformBuffers ? RDNA4S_NONUNIFORM_BUFFERS : 0u) |
        (RDNA4R.gpuArgumentTables ? RDNA4S_RUNTIME_ARRAY : 0u) |
        (rdna4_device_gather_caps().supported ? RDNA4S_IMAGE_GATHER_EXTENDED : 0u) |
        (RDNA4R.uniformTexelBuffers ? RDNA4S_NONUNIFORM_UNIFORM_TEXEL : 0u) |
        (RDNA4R.storageTexelBuffers ? RDNA4S_NONUNIFORM_STORAGE_TEXEL : 0u);
    if (r && (r->descriptor_features & ~descriptorFeatures)) {
        if (err) *err = rdna4_err(49, @"shader requires non-uniform descriptor indexing unavailable on this backend");
        return NO;
    }
    if (r && r->bind[2][1].used && !RDNA4R.uniformTexelBuffers) {
        if (err) *err = rdna4_err(49,
            @"shader requires uniform texel-buffer arrays unavailable on this backend");
        return NO;
    }
    if (r && r->bind[3][1].used && !RDNA4R.storageTexelBuffers) {
        if (err) *err = rdna4_err(49,
            @"shader requires storage texel-buffer arrays unavailable on this backend");
        return NO;
    }
    meta = rdna4_stage_meta_relocate(meta, set);
    NSArray<NSDictionary *> *bindingMetadata = meta[@"bindings"];
    BOOL gpuArguments = meta[@"argument_resource_tables"] != nil;
    BOOL normalizedCoordinates = NO;
    if (!rdna4_argument_normalized_metadata(meta, &normalizedCoordinates, err))
        return NO;
    if (bindingMetadata && ![bindingMetadata isKindOfClass:[NSArray class]]) {
        if (err) *err = rdna4_err(49, @"shader binding metadata must be an array");
        return NO;
    }
    BOOL coordinateFlagsMetadata = NO;
    for (NSDictionary *binding in bindingMetadata) {
        if (![binding isKindOfClass:[NSDictionary class]] || ![binding[@"kind"] isKindOfClass:[NSString class]]) {
            if (err) *err = rdna4_err(49, @"shader binding metadata contains an invalid entry");
            return NO;
        }
        NSString *kind = binding[@"kind"];
        if ([kind isEqual:@"SynthesizedReadSampler"]) {
            NSDictionary *descriptor = binding[@"descriptor"];
            uint64_t descriptorSet = 0, slot = 0, count = 0;
            BOOL valid = [descriptor isKindOfClass:[NSDictionary class]] &&
                rdna4_number_u64((__bridge CFTypeRef)descriptor[@"set"], &descriptorSet) &&
                descriptorSet == (uint64_t)set &&
                rdna4_number_u64((__bridge CFTypeRef)descriptor[@"binding"], &slot) &&
                slot >= 160 && slot < 192 &&
                rdna4_number_u64((__bridge CFTypeRef)descriptor[@"count"], &count) && count == 1 &&
                [binding[@"static_sampler"] isKindOfClass:[NSDictionary class]] &&
                !binding[@"comparison_descriptor"] && !binding[@"embedded_source"] && r &&
                r->bind[set][slot].used && r->bind[set][slot].type == VK_DESCRIPTOR_TYPE_SAMPLER &&
                r->bind[set][slot].count == 1;
            if (!valid) {
                if (err) *err = rdna4_err(49, @"synthesized read sampler state or descriptor is invalid");
                return NO;
            }
        }
        if ([kind isEqual:@"SamplerCoordinateFlags"] || [kind isEqual:@"StageResourceMetadata"]) {
            BOOL stageMetadata = [kind isEqual:@"StageResourceMetadata"];
            BOOL versionPresent = NO;
            uint64_t reflectionVersion = 0;
            if (!rdna4_argument_reflection_version(meta, &versionPresent, &reflectionVersion)) return NO;
            NSUInteger expectedBytes = stageMetadata ? 384 : 128;
            NSDictionary *descriptor = binding[@"descriptor"];
            NSDictionary *extent = binding[@"extent"];
            uint64_t metalIndex = 0, descriptorSet = 0, slot = 0, count = 0,
                declared = 0, bytes = 0;
            NSSet *bindingKeys = [NSSet setWithObjects:@"kind", @"metal_index",
                @"descriptor", @"declared_size", @"extent", @"access", nil];
            NSSet *descriptorKeys = [NSSet setWithObjects:@"set", @"binding",
                @"count", nil];
            NSSet *extentKeys = [NSSet setWithObject:@"Object"];
            NSSet *objectKeys = [NSSet setWithObject:@"bytes"];
            BOOL exact = (stageMetadata || normalizedCoordinates) && r && versionPresent &&
                (stageMetadata ? (reflectionVersion == 65 || reflectionVersion == 66) && rdna4_argument_table_uses_shape_registry(meta)
                               : !gpuArguments && reflectionVersion == 64) &&
                [descriptor isKindOfClass:[NSDictionary class]] &&
                [extent isKindOfClass:[NSDictionary class]] &&
                [extent[@"Object"] isKindOfClass:[NSDictionary class]] &&
                [[NSSet setWithArray:binding.allKeys]
                    isEqualToSet:bindingKeys] &&
                [[NSSet setWithArray:descriptor.allKeys]
                    isEqualToSet:descriptorKeys] &&
                [[NSSet setWithArray:extent.allKeys]
                    isEqualToSet:extentKeys] &&
                [[NSSet setWithArray:[extent[@"Object"] allKeys]]
                    isEqualToSet:objectKeys] &&
                [binding[@"access"] isEqual:@"ReadOnly"] &&
                rdna4_number_u64((__bridge CFTypeRef)binding[@"metal_index"], &metalIndex) &&
                metalIndex == 0 &&
                rdna4_number_u64((__bridge CFTypeRef)descriptor[@"set"], &descriptorSet) &&
                descriptorSet == (uint64_t)set &&
                rdna4_number_u64((__bridge CFTypeRef)descriptor[@"binding"], &slot) &&
                slot == 674 &&
                rdna4_number_u64((__bridge CFTypeRef)descriptor[@"count"], &count) &&
                count == 1 &&
                rdna4_number_u64((__bridge CFTypeRef)binding[@"declared_size"], &declared) &&
                declared == expectedBytes &&
                rdna4_number_u64((__bridge CFTypeRef)extent[@"Object"][@"bytes"], &bytes) &&
                bytes == expectedBytes && !binding[@"comparison_descriptor"] &&
                !binding[@"embedded_source"] && !binding[@"texture_shape"];
            if (!r || !exact || coordinateFlagsMetadata) {
                if (err) *err = rdna4_err(49,
                    @"SamplerCoordinateFlags metadata does not match binding 674 reflection");
                return NO;
            }
            const RDNA4sBind *flags = &r->bind[set][674];
            if (!flags->used ||
                flags->type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
                flags->count != 1 || flags->runtime) {
                if (err) *err = rdna4_err(49,
                    @"SamplerCoordinateFlags metadata does not match binding 674 reflection");
                return NO;
            }
            coordinateFlagsMetadata = YES;
            continue;
        }
        id paired = binding[@"comparison_descriptor"];
        if (paired) {
            // Table samplers share one runtime array; its logical pair is checked with the table ABI below.
            if (gpuArguments && [binding[@"kind"] isEqual:@"EmbeddedArgBufferSampler"]) continue;
            id normal = binding[@"descriptor"];
            uint64_t nset = 0, cset = 0, nb = 0, cb = 0, nc = 0, cc = 0;
            BOOL sampler = [binding[@"kind"] isEqual:@"Sampler"] || [binding[@"kind"] isEqual:@"StaticSampler"] ||
                [binding[@"kind"] isEqual:@"EmbeddedArgBufferSampler"];
            if (!sampler || ![normal isKindOfClass:[NSDictionary class]] || ![paired isKindOfClass:[NSDictionary class]] ||
                !rdna4_number_u64((__bridge CFTypeRef)normal[@"set"], &nset) ||
                !rdna4_number_u64((__bridge CFTypeRef)paired[@"set"], &cset) || nset != cset || nset > 1 ||
                !rdna4_number_u64((__bridge CFTypeRef)normal[@"binding"], &nb) || nb < 160 || nb >= 192 ||
                !rdna4_number_u64((__bridge CFTypeRef)paired[@"binding"], &cb) || cb != nb + 576 ||
                !rdna4_number_u64((__bridge CFTypeRef)normal[@"count"], &nc) || !nc || nc > 192 - nb ||
                !rdna4_number_u64((__bridge CFTypeRef)paired[@"count"], &cc) || cc != nc || !r ||
                !r->bind[set][nb].used || !r->bind[set][cb].used ||
                r->bind[set][nb].type != VK_DESCRIPTOR_TYPE_SAMPLER || r->bind[set][cb].type != VK_DESCRIPTOR_TYPE_SAMPLER ||
                r->bind[set][nb].count != nc || r->bind[set][cb].count != nc) {
                if (err) *err = rdna4_err(49, @"comparison sampler metadata does not match the reflected stage");
                return NO;
            }
        }
    }
    const RDNA4sBind *coordinateFlags = r ? &r->bind[set][674] : NULL;
    BOOL reflectedCoordinateFlags = coordinateFlags && coordinateFlags->used;
    if (reflectedCoordinateFlags != coordinateFlagsMetadata) {
        if (err) *err = rdna4_err(49,
            @"binding 674 and SamplerCoordinateFlags metadata must appear together");
        return NO;
    }
    if (!rdna4_argument_table_metadata(r, set, meta, err)) return NO;
    VkDescriptorSetLayoutBinding *bnd = calloc(RDNA4S_MAXBIND, sizeof *bnd); RDNA4PB *pb = calloc(RDNA4S_MAXBIND, sizeof *pb); uint32_t nb = 0;
    if (!bnd || !pb) {
        free(bnd); free(pb);
        if (err) *err = rdna4_err(49, @"cannot allocate shader binding layout");
        return NO;
    }
    if (r) {
        if (r->maxset > set && !gpuArguments) { free(bnd); free(pb); if (err) *err = rdna4_err(49, [NSString stringWithFormat:@"stage uses descriptor set %d (> %d)", r->maxset, set]); return NO; }
        for (int b = 0; b < RDNA4S_MAXBIND; b++) {
            const RDNA4sBind *a = &r->bind[set][b]; if (!a->used) continue;
            if (a->runtime) {
                free(bnd); free(pb);
                if (err) *err = rdna4_err(49, @"runtime descriptor arrays require the GPU argument table layout");
                return NO;
            }
            bnd[nb] = (VkDescriptorSetLayoutBinding){ (uint32_t)b, a->type, a->count, stage, NULL };
            pb[nb] = (RDNA4PB){ .binding = (uint32_t)b, .type = a->type, .count = a->count };
            pb[nb].imageSampleKinds = a->imageSampleKinds;
            pb[nb].texelComponentKinds = a->texelComponentKinds;
            pb[nb].comparisonSampler = a->type == VK_DESCRIPTOR_TYPE_SAMPLER && b >= 736 && b < 768;
            pb[nb].gpuArguments = gpuArguments;
            pb[nb].argumentTable = gpuArguments && (b == 672 || b == 673);
            pb[nb].argumentRoot = gpuArguments && b >= 704 && b < 735;
            pb[nb].normalizedCoordinates = normalizedCoordinates &&
                a->type == VK_DESCRIPTOR_TYPE_SAMPLER;
            pb[nb].samplerCoordinateFlags = b == 674;
            pb[nb].rtPrimDataTable = (b == (int)RDNA4RT_PRIM_DATA_BINDING &&
                a->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER && a->count == 1 && !a->runtime);
            if (pb[nb].rtPrimDataTable && !RDNA4R.bufferAddress) {
                free(bnd); free(pb);
                if (err) *err = rdna4_err(49, @"RT primitive data table requires Vulkan buffer device address support");
                return NO;
            }
            if (b == 674 && rdna4_argument_table_uses_shape_registry(meta)) {
                pb[nb].stageResourceMetadata = YES;
                pb[nb].stageTokenTable = r->bind[set][672].used;
                NSArray *shapes = meta[@"argument_resource_tables"][@"null_shapes"];
                pb[nb].nullShapeCount = (uint16_t)shapes.count;
                for (NSUInteger i = 0; i < shapes.count; ++i)
                    pb[nb].nullShapeRemap[i] = [shapes[i] unsignedIntValue];
            }
            if (pb[nb].argumentRoot) pb[nb].embeddedBuffer = (uint32_t)b - 704;
            nb++;
        }
    }
    for (NSDictionary *bd in bindingMetadata) {
        if (gpuArguments) continue;
        if (![bd isKindOfClass:[NSDictionary class]]) continue;
        BOOL embeddedTexture = [bd[@"kind"] isEqual:@"EmbeddedArgBufferTexture"];
        if (!embeddedTexture && ![bd[@"kind"] isEqual:@"EmbeddedArgBufferSampler"]) continue;
        NSDictionary *source = bd[@"embedded_source"];
        NSDictionary *descriptor = bd[@"descriptor"];
        NSNumber *binding = [descriptor isKindOfClass:[NSDictionary class]] ? descriptor[@"binding"] : nil;
        NSNumber *count = [descriptor isKindOfClass:[NSDictionary class]] ? descriptor[@"count"] : nil;
        NSDictionary *comparisonDescriptor = bd[@"comparison_descriptor"];
        uint64_t comparisonBinding = comparisonDescriptor ? [comparisonDescriptor[@"binding"] unsignedLongLongValue] : UINT64_MAX;
        NSNumber *buffer = [source isKindOfClass:[NSDictionary class]] ? source[@"buffer_index"] : nil;
        NSNumber *offset = [source isKindOfClass:[NSDictionary class]] ? source[@"field_offset"] : nil;
        BOOL matched = NO;
        uint64_t bindingValue = 0, countValue = 0, bufferValue = 0, offsetValue = 0;
        if (rdna4_number_u64((__bridge CFTypeRef)binding, &bindingValue) && bindingValue < RDNA4S_MAXBIND &&
            rdna4_number_u64((__bridge CFTypeRef)count, &countValue) && countValue > 0 && countValue <= UINT32_MAX &&
            rdna4_number_u64((__bridge CFTypeRef)buffer, &bufferValue) && bufferValue < 31 &&
            rdna4_number_u64((__bridge CFTypeRef)offset, &offsetValue) && offsetValue <= UINT32_MAX &&
            !(offsetValue & (sizeof(uint64_t) - 1))) {
            for (uint32_t i = 0; i < nb; ++i) {
                if (pb[i].binding != bindingValue && pb[i].binding != comparisonBinding) continue;
                BOOL typeMatches = embeddedTexture
                    ? (pb[i].type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || pb[i].type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
                       pb[i].type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || pb[i].type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER)
                    : pb[i].type == VK_DESCRIPTOR_TYPE_SAMPLER;
                if (!typeMatches || pb[i].count != countValue || pb[i].embedded) break;
                pb[i].embedded = YES;
                pb[i].embeddedBuffer = (uint32_t)bufferValue;
                pb[i].embeddedOffset = (uint32_t)offsetValue;
                matched = YES;
            }
        }
        if (!matched) {
            free(bnd); free(pb);
            if (err) *err = rdna4_err(49, @"embedded image/sampler metadata does not match a supported descriptor");
            return NO;
        }
    }
    for (NSDictionary *bd in bindingMetadata) {
        if (![bd[@"kind"] isEqual:@"BufferAddressTable"]) continue;
        NSDictionary *descriptor = [bd[@"descriptor"] isKindOfClass:[NSDictionary class]] ? bd[@"descriptor"] : nil;
        uint64_t binding = 0, count = 0;
        BOOL found = NO;
        if (RDNA4R.bufferAddress && rdna4_number_u64((__bridge CFTypeRef)descriptor[@"binding"], &binding) &&
            rdna4_number_u64((__bridge CFTypeRef)descriptor[@"count"], &count) && count == 1 &&
            binding >= 640 && binding < 672) {
            for (uint32_t i = 0; i < nb; ++i)
                if (pb[i].binding == binding && !pb[i].addressTable && pb[i].count == 1 &&
                    pb[i].type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
                    pb[i].addressTable = YES;
                    id version = meta[@"buffer_address_table_version"], bytes = meta[@"buffer_address_table_bytes"];
                    uint64_t versionNumber = 0, tableBytes = 0;
                    id stride = meta[@"buffer_address_table_row_stride_bytes"];
                    id dynamic = meta[@"buffer_address_table_dynamic_extent"];
                    uint64_t rowStride = 0;
                    BOOL numeric = rdna4_number_u64((__bridge CFTypeRef)version, &versionNumber) &&
                        rdna4_number_u64((__bridge CFTypeRef)bytes, &tableBytes);
                    BOOL directLayout = numeric && versionNumber == RDNA4_BUFFER_TABLE_VERSION &&
                        tableBytes == sizeof(rdna4_buffer_table) && !stride && !dynamic;
                    BOOL registryLayout = numeric && versionNumber == RDNA4_BUFFER_REGISTRY_VERSION &&
                        tableBytes == RDNA4_BUFFER_REGISTRY_MIN_BYTES &&
                        rdna4_number_u64((__bridge CFTypeRef)stride, &rowStride) &&
                        rowStride == RDNA4_BUFFER_REGISTRY_ROW_BYTES && dynamic &&
                        CFGetTypeID((__bridge CFTypeRef)dynamic) == CFBooleanGetTypeID() &&
                        [dynamic boolValue];
                    if ((version || bytes || stride || dynamic) && !directLayout && !registryLayout) {
                        free(bnd); free(pb);
                        if (err) *err = rdna4_err(49, @"unsupported buffer-address table layout");
                        return NO;
                    }
                    pb[i].addressTableVersion = version ? (uint32_t)versionNumber : 1;
                    found = YES;
                }
        }
        if (!found) {
            free(bnd); free(pb);
            if (err) *err = rdna4_err(49, @"buffer-address table metadata or Vulkan device-address support is invalid");
            return NO;
        }
    }
    for (NSDictionary *bd in bindingMetadata) {
        if (![bd[@"kind"] isEqual:@"RTPrimDataTable"]) continue;
        NSDictionary *descriptor = [bd[@"descriptor"] isKindOfClass:[NSDictionary class]] ? bd[@"descriptor"] : nil;
        uint64_t descriptorSet = (uint64_t)set, binding = 0, count = 0;
        BOOL setOK = !descriptor[@"set"] ||
            (rdna4_number_u64((__bridge CFTypeRef)descriptor[@"set"], &descriptorSet) &&
             descriptorSet == (uint64_t)set);
        BOOL found = NO;
        if (RDNA4R.bufferAddress && setOK &&
            rdna4_number_u64((__bridge CFTypeRef)descriptor[@"binding"], &binding) &&
            binding == RDNA4RT_PRIM_DATA_BINDING &&
            rdna4_number_u64((__bridge CFTypeRef)descriptor[@"count"], &count) && count == 1) {
            for (uint32_t i = 0; i < nb; ++i) {
                if (pb[i].binding == RDNA4RT_PRIM_DATA_BINDING && pb[i].count == 1 &&
                    pb[i].type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
                    pb[i].rtPrimDataTable = YES;
                    found = YES;
                    break;
                }
            }
        }
        if (!found) {
            free(bnd); free(pb);
            if (err) *err = rdna4_err(49, @"RT primitive data table metadata or Vulkan device-address support is invalid");
            return NO;
        }
    }
    for (NSDictionary *bd in bindingMetadata) {
        BOOL staticSampler = [bd[@"kind"] isEqual:@"StaticSampler"] ||
            [bd[@"kind"] isEqual:@"SynthesizedReadSampler"];
        if (!staticSampler || ![bd[@"static_sampler"] isKindOfClass:[NSDictionary class]]) continue;
        uint32_t bn = (uint32_t)[bd[@"descriptor"][@"binding"] unsignedIntValue];
        NSDictionary *comparison = bd[@"comparison_descriptor"];
        uint64_t comparisonBinding = comparison ? [comparison[@"binding"] unsignedLongLongValue] : UINT64_MAX;
        for (uint32_t i = 0; i < nb; i++) if ((pb[i].binding == bn || pb[i].binding == comparisonBinding) && pb[i].type == VK_DESCRIPTOR_TYPE_SAMPLER) {
            RDNA4SampCfg c; NSError *e = nil;
            if (!rdna4_static_cfg(bd[@"static_sampler"], &c, &e)) { free(bnd); free(pb); if (err) *err = e; return NO; }
            VkSampler s = rdna4_make_sampler(&c, pb[i].comparisonSampler,
                                             normalizedCoordinates, NULL, &e);
            if (!s) { free(bnd); free(pb); if (err) *err = e; return NO; }
            pb[i].stat = s; [ownedSamplers addObject:[NSValue valueWithPointer:(void *)s]];
            pb[i].pixelCoordinates = c.unnorm != 0;
            RDNA4LOG("static sampler at binding %u: min %d mag %d mip %d addr %d/%d/%d %s", bn, c.minF, c.magF, c.mipF, c.as, c.at, c.ar, c.unnorm ? "unnormalized (coord::pixel)" : "normalized");
        }
    }
    BOOL usePush = NO;
    if (allowPush && RDNA4DO.on && RDNA4R.pushDescExt && RDNA4R.cmdPushDescSet &&
        !RDNA4P.auditSubmit && RDNA4R.realAllocDesc && vkAllocateDescriptorSets == RDNA4R.realAllocDesc &&
        nb > 0 && nb <= 16u) {
        uint32_t totalDesc = 0u;
        BOOL okPush = YES;
        for (uint32_t i = 0; i < nb; i++) {
            const RDNA4PB *p = &pb[i];
            if (p->addressTable || p->samplerCoordinateFlags || p->argumentTable || p->rtPrimDataTable ||
                (p->type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER && p->type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER &&
                 p->type != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE && p->type != VK_DESCRIPTOR_TYPE_SAMPLER &&
                 p->type != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE && p->type != VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT)) {
                okPush = NO; break;
            }
            totalDesc += p->count;
        }
        if (okPush && totalDesc > 0u && totalDesc <= RDNA4R.maxPushDesc && totalDesc <= 24u)
            usePush = YES;
    }
    VkDescriptorSetLayoutCreateInfo dc = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .flags = usePush ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR : 0u,
        .bindingCount = nb, .pBindings = bnd };
    VkResult vr = vkCreateDescriptorSetLayout(RDNA4R.dev, &dc, NULL, dsl);
    free(bnd);
    if (vr != VK_SUCCESS) { free(pb); if (err) *err = rdna4_err(50, [NSString stringWithFormat:@"vkCreateDescriptorSetLayout = %d", vr]); return NO; }
    if (usedPush) *usedPush = usePush;
    if (nb) { *pbOut = realloc(pb, nb * sizeof *pb); *npb = nb; } else free(pb);
    return YES;
}

// MTLVertexFormat -> VkFormat (0 = unsupported).
GARNET_INTERNAL VkFormat rdna4_vkvfmt(MTLVertexFormat f) {
    switch (f) {
    case MTLVertexFormatUChar: return VK_FORMAT_R8_UINT; case MTLVertexFormatUChar2: return VK_FORMAT_R8G8_UINT; case MTLVertexFormatUChar3: return VK_FORMAT_R8G8B8_UINT; case MTLVertexFormatUChar4: return VK_FORMAT_R8G8B8A8_UINT;
    case MTLVertexFormatChar: return VK_FORMAT_R8_SINT; case MTLVertexFormatChar2: return VK_FORMAT_R8G8_SINT; case MTLVertexFormatChar3: return VK_FORMAT_R8G8B8_SINT; case MTLVertexFormatChar4: return VK_FORMAT_R8G8B8A8_SINT;
    case MTLVertexFormatUCharNormalized: return VK_FORMAT_R8_UNORM; case MTLVertexFormatUChar2Normalized: return VK_FORMAT_R8G8_UNORM; case MTLVertexFormatUChar3Normalized: return VK_FORMAT_R8G8B8_UNORM; case MTLVertexFormatUChar4Normalized: return VK_FORMAT_R8G8B8A8_UNORM;
    case MTLVertexFormatCharNormalized: return VK_FORMAT_R8_SNORM; case MTLVertexFormatChar2Normalized: return VK_FORMAT_R8G8_SNORM; case MTLVertexFormatChar3Normalized: return VK_FORMAT_R8G8B8_SNORM; case MTLVertexFormatChar4Normalized: return VK_FORMAT_R8G8B8A8_SNORM;
    case MTLVertexFormatUChar4Normalized_BGRA: return VK_FORMAT_B8G8R8A8_UNORM;
    case MTLVertexFormatUShort: return VK_FORMAT_R16_UINT; case MTLVertexFormatUShort2: return VK_FORMAT_R16G16_UINT; case MTLVertexFormatUShort3: return VK_FORMAT_R16G16B16_UINT; case MTLVertexFormatUShort4: return VK_FORMAT_R16G16B16A16_UINT;
    case MTLVertexFormatShort: return VK_FORMAT_R16_SINT; case MTLVertexFormatShort2: return VK_FORMAT_R16G16_SINT; case MTLVertexFormatShort3: return VK_FORMAT_R16G16B16_SINT; case MTLVertexFormatShort4: return VK_FORMAT_R16G16B16A16_SINT;
    case MTLVertexFormatUShortNormalized: return VK_FORMAT_R16_UNORM; case MTLVertexFormatUShort2Normalized: return VK_FORMAT_R16G16_UNORM; case MTLVertexFormatUShort3Normalized: return VK_FORMAT_R16G16B16_UNORM; case MTLVertexFormatUShort4Normalized: return VK_FORMAT_R16G16B16A16_UNORM;
    case MTLVertexFormatShortNormalized: return VK_FORMAT_R16_SNORM; case MTLVertexFormatShort2Normalized: return VK_FORMAT_R16G16_SNORM; case MTLVertexFormatShort3Normalized: return VK_FORMAT_R16G16B16_SNORM; case MTLVertexFormatShort4Normalized: return VK_FORMAT_R16G16B16A16_SNORM;
    case MTLVertexFormatHalf: return VK_FORMAT_R16_SFLOAT; case MTLVertexFormatHalf2: return VK_FORMAT_R16G16_SFLOAT; case MTLVertexFormatHalf3: return VK_FORMAT_R16G16B16_SFLOAT; case MTLVertexFormatHalf4: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case MTLVertexFormatFloat: return VK_FORMAT_R32_SFLOAT; case MTLVertexFormatFloat2: return VK_FORMAT_R32G32_SFLOAT; case MTLVertexFormatFloat3: return VK_FORMAT_R32G32B32_SFLOAT; case MTLVertexFormatFloat4: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case MTLVertexFormatInt: return VK_FORMAT_R32_SINT; case MTLVertexFormatInt2: return VK_FORMAT_R32G32_SINT; case MTLVertexFormatInt3: return VK_FORMAT_R32G32B32_SINT; case MTLVertexFormatInt4: return VK_FORMAT_R32G32B32A32_SINT;
    case MTLVertexFormatUInt: return VK_FORMAT_R32_UINT; case MTLVertexFormatUInt2: return VK_FORMAT_R32G32_UINT; case MTLVertexFormatUInt3: return VK_FORMAT_R32G32B32_UINT; case MTLVertexFormatUInt4: return VK_FORMAT_R32G32B32A32_UINT;
    case MTLVertexFormatInt1010102Normalized: return VK_FORMAT_A2B10G10R10_SNORM_PACK32; case MTLVertexFormatUInt1010102Normalized: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    default: return VK_FORMAT_UNDEFINED;
    }
}
GARNET_INTERNAL VkBlendFactor rdna4_bf(MTLBlendFactor f, BOOL *bad) {
    switch (f) {
    case MTLBlendFactorZero: return VK_BLEND_FACTOR_ZERO; case MTLBlendFactorOne: return VK_BLEND_FACTOR_ONE;
    case MTLBlendFactorSourceColor: return VK_BLEND_FACTOR_SRC_COLOR; case MTLBlendFactorOneMinusSourceColor: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case MTLBlendFactorSourceAlpha: return VK_BLEND_FACTOR_SRC_ALPHA; case MTLBlendFactorOneMinusSourceAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case MTLBlendFactorDestinationColor: return VK_BLEND_FACTOR_DST_COLOR; case MTLBlendFactorOneMinusDestinationColor: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case MTLBlendFactorDestinationAlpha: return VK_BLEND_FACTOR_DST_ALPHA; case MTLBlendFactorOneMinusDestinationAlpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case MTLBlendFactorSourceAlphaSaturated: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case MTLBlendFactorBlendColor: return VK_BLEND_FACTOR_CONSTANT_COLOR; case MTLBlendFactorOneMinusBlendColor: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case MTLBlendFactorBlendAlpha: return VK_BLEND_FACTOR_CONSTANT_ALPHA; case MTLBlendFactorOneMinusBlendAlpha: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    case MTLBlendFactorSource1Color: if (!RDNA4R.dualSrcBlend) *bad = YES; return VK_BLEND_FACTOR_SRC1_COLOR;
    case MTLBlendFactorOneMinusSource1Color: if (!RDNA4R.dualSrcBlend) *bad = YES; return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case MTLBlendFactorSource1Alpha: if (!RDNA4R.dualSrcBlend) *bad = YES; return VK_BLEND_FACTOR_SRC1_ALPHA;
    case MTLBlendFactorOneMinusSource1Alpha: if (!RDNA4R.dualSrcBlend) *bad = YES; return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    default: *bad = YES; return VK_BLEND_FACTOR_ZERO;
    }
}
GARNET_INTERNAL VkBlendOp rdna4_bo(MTLBlendOperation o, BOOL *bad) {
    switch (o) { case MTLBlendOperationAdd: return VK_BLEND_OP_ADD; case MTLBlendOperationSubtract: return VK_BLEND_OP_SUBTRACT;
        case MTLBlendOperationReverseSubtract: return VK_BLEND_OP_REVERSE_SUBTRACT; case MTLBlendOperationMin: return VK_BLEND_OP_MIN;
        case MTLBlendOperationMax: return VK_BLEND_OP_MAX; default: *bad = YES; return VK_BLEND_OP_ADD; }
}

// ---------------------------------------------------------------------------------------------------------------
// Pipeline HOT-SWAP (native #12): a FALLBACK pipeline object (spvcache miss in WindowServer; render/compute = no-write placeholder) registers
// its spvcache keys here. One watcher per process (a dispatch timer on a private serial queue that exists only while an entry is pending)
// stats <sha>.spv / <sha>.meta.json in the lookup directories; when the state machine (garnet_hotswap.h) says the files are complete and stable it
// calls the object's builder on the watcher queue (NEVER the render thread), which builds a NEW pipeline object through the normal init path and
// publishes it with one atomic store into the fallback object (rdna4hs_install). Encoders resolve the pointer at the start of every draw / dispatch.
// ---------------------------------------------------------------------------------------------------------------
@interface RDNA4HSEntry : NSObject { @public __weak id obj; rdna4hs_entry st; NSArray<NSString *> *shas; NSString *name; BOOL (^build)(id); } @end
@implementation RDNA4HSEntry @end
static NSMutableArray<RDNA4HSEntry *> *rdna4hs_reg; static NSLock *rdna4hs_lock; static dispatch_queue_t rdna4hs_q; static dispatch_source_t rdna4hs_timer;

GARNET_INTERNAL NSString *rdna4_fn_sha(id fn) {   // sha256(bitcodeData) of a function, nil when it has none; build 18 (P3): the linked key when the function is a linked stage of the pipeline being created
    RDNA4LinkCtx *lk_ = rdna4_lk_for(fn); if (lk_) return lk_->lsha;
    SEL sel = NSSelectorFromString(@"bitcodeData");
    NSData *bc = (fn && [fn respondsToSelector:sel]) ? ((NSData *(*)(id, SEL))objc_msgSend)(fn, sel) : nil;
    return bc.length ? rdna4_sha256hex(bc) : nil;
}
static int64_t rdna4hs_wall_ns(void) { struct timespec t; clock_gettime(CLOCK_REALTIME, &t); return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec; }
// First lookup directory holding a usable file (same order and sanity as the creation-time lookup).
static rdna4hs_obs rdna4hs_stat_path(NSString *path) {
    struct stat fileStat;
    if (stat(path.fileSystemRepresentation, &fileStat) || !S_ISREG(fileStat.st_mode) || fileStat.st_size <= 0)
        return (rdna4hs_obs){0};
    return (rdna4hs_obs){1, (uint64_t)fileStat.st_size,
        (int64_t)fileStat.st_mtimespec.tv_sec * 1000000000LL + fileStat.st_mtimespec.tv_nsec};
}
static rdna4hs_key_obs rdna4hs_stat_pair(NSString *sha, BOOL needMeta) {
    for (NSString *dir in rdna4_spv_dirs()) {
        rdna4hs_obs selected = rdna4hs_stat_path([dir stringByAppendingPathComponent:[sha stringByAppendingString:@".spv"]]);
        if (!rdna4hs_spv_ok(&selected)) continue;
        rdna4hs_key_obs pair = { .spv = selected };
        if (needMeta) pair.meta = rdna4hs_stat_path([dir stringByAppendingPathComponent:[sha stringByAppendingString:@".meta.json"]]);
        return pair;
    }
    return (rdna4hs_key_obs){0};
}
static void rdna4hs_drop(RDNA4HSEntry *e) {
    [rdna4hs_lock lock]; [rdna4hs_reg removeObjectIdenticalTo:e];
    if (!rdna4hs_reg.count && rdna4hs_timer) { dispatch_source_cancel(rdna4hs_timer); rdna4hs_timer = nil; }
    [rdna4hs_lock unlock];
}
static void rdna4hs_tick_all(void) {
    @autoreleasepool {
        [rdna4hs_lock lock]; NSArray<RDNA4HSEntry *> *snap = [rdna4hs_reg copy]; [rdna4hs_lock unlock];
        for (RDNA4HSEntry *e in snap) {
            id o = e->obj;
            if (!o || e->st.st == RDNA4HS_SWAPPED || e->st.st == RDNA4HS_GAVEUP) { rdna4hs_drop(e); continue; }
            rdna4hs_key_obs cur[RDNA4HS_MAXKEY] = {{{0},{0}}};
            for (int k = 0; k < e->st.nkeys; k++) cur[k] = rdna4hs_stat_pair(e->shas[(NSUInteger)k], YES);
            if (rdna4hs_tick(&e->st, rdna4hs_wall_ns(), cur) != RDNA4HS_BUILD) continue;
            RDNA4LOG("HOT-SWAP %s: spvcache complete and stable (attempt %d), building the real pipeline off the render thread", e->name.UTF8String, e->st.attempts);
            uint64_t t0 = rdna4_now(); BOOL ok = e->build(o); double ms = (double)(rdna4_now() - t0) / 1e6;
            if (rdna4hs_build_done(&e->st, ok, rdna4hs_wall_ns())) {
                rdna4_sync_note_async_success();
                RDNA4LOG("HOT-SWAP %s: fallback -> real (built in %.3f ms)", e->name.UTF8String, ms);
                rdna4hs_drop(e);
            }
            else if (e->st.st == RDNA4HS_GAVEUP) { RDNA4LOG_ERR("HOT-SWAP %s: GAVE UP after %d failed builds (the fallback stays)", e->name.UTF8String, e->st.attempts); rdna4hs_drop(e); }
            else RDNA4LOG_WARN("HOT-SWAP %s: build failed (attempt %d), will retry", e->name.UTF8String, e->st.attempts);
        }
    }
}
// shas: 2 (vertex, fragment) or 1 (kernel). obj is held weakly; a deallocated object drops out at the next tick.
GARNET_INTERNAL void rdna4hs_register(id obj, NSArray<NSString *> *shas, BOOL metaRequired, NSString *name, BOOL (^build)(id)) {
    if (rdna4_force_fallback() || !shas.count || shas.count > RDNA4HS_MAXKEY) return;   // forced misses (r1 test) must stay fallbacks
    static dispatch_once_t once;
    dispatch_once(&once, ^{ rdna4hs_lock = [NSLock new]; rdna4hs_reg = [NSMutableArray array]; rdna4hs_q = dispatch_queue_create("rdna4.hotswap", DISPATCH_QUEUE_SERIAL); });
    RDNA4HSEntry *e = [RDNA4HSEntry new]; e->obj = obj; e->shas = [shas copy]; e->name = [name copy]; e->build = [build copy];
    rdna4hs_init(&e->st, (int)shas.count, metaRequired ? 1 : 0);
    [rdna4hs_lock lock];
    [rdna4hs_reg addObject:e];
    if (!rdna4hs_timer) {
        rdna4hs_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, rdna4hs_q);
        dispatch_source_set_timer(rdna4hs_timer, dispatch_time(DISPATCH_TIME_NOW, RDNA4HS_POLL_NS), RDNA4HS_POLL_NS, RDNA4HS_POLL_NS / 5);
        dispatch_source_set_event_handler(rdna4hs_timer, ^{ rdna4hs_tick_all(); });
        dispatch_resume(rdna4hs_timer);
    }
    [rdna4hs_lock unlock];
    RDNA4LOG("HOT-SWAP %s: registered (%lu key(s)%s)", name.UTF8String, (unsigned long)shas.count, metaRequired ? ", meta required" : "");
}

// Subscribe before checking files so atomic publication cannot be missed. WS gets
// only 250 ms, apps 3 s, then temporary placeholders; three timeouts skip waiting.
GARNET_INTERNAL BOOL rdna4_sync_wait(const char *what, NSArray<NSString *> *shas, BOOL needMeta) {
    if (!rdna4g_syncwait_applies(rdna4_is_ws(), atomic_load(&rdna4_app_admitted), rdna4_force_fallback(), rdna4_test_fb_as_ws()) || !shas.count) return NO;
    if (!rdna4_sync_begin(NO)) return NO;
    const uint64_t t0 = rdna4_now(), deadline = t0 + (uint64_t)rdna4g_sync_wait_ms(rdna4_is_ws()) * 1000000ull;
    const int queue = kqueue();
    if (queue < 0) { rdna4_sync_end(NO, NO); return NO; }
    NSMutableArray<NSNumber *> *fds = [NSMutableArray array];
    for (NSString *dir in rdna4_spv_dirs()) {
        int fd = open(dir.fileSystemRepresentation, O_EVTONLY | O_CLOEXEC);
        if (fd < 0) continue;
        struct kevent change;
        EV_SET(&change, fd, EVFILT_VNODE, EV_ADD | EV_CLEAR, NOTE_WRITE | NOTE_RENAME | NOTE_DELETE, 0, NULL);
        if (kevent(queue, &change, 1, NULL, 0, NULL) != 0) { close(fd); continue; }
        [fds addObject:@(fd)];
    }
    BOOL found = NO;
    for (;;) {
        BOOL all = YES;
        for (NSString *sha in shas) {
            rdna4hs_key_obs pair = rdna4hs_stat_pair(sha, needMeta);
            if (!pair.spv.present || (needMeta && !pair.meta.present)) { all = NO; break; }
        }
        found = all;
        uint64_t now = rdna4_now();
        if (found || !fds.count || now >= deadline) break;
        uint64_t remaining = deadline - now;
        struct timespec timeout = { (time_t)(remaining / NSEC_PER_SEC), (long)(remaining % NSEC_PER_SEC) };
        struct kevent event;
        int result = kevent(queue, NULL, 0, &event, 1, &timeout);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0 || (event.flags & EV_ERROR) || (event.fflags & (NOTE_RENAME | NOTE_DELETE))) break;
    }
    for (NSNumber *fd in fds) close(fd.intValue);
    close(queue);
    rdna4_sync_end(found, !found && rdna4_now() >= deadline);
    const double ms = (double)(rdna4_now() - t0) / 1e6;
    RDNA4LOG("SYNC-WAIT %s: translation %s after %.0f ms", what, found ? "ready" : "unavailable", ms);
    return found;
}

#endif // !defined(GARNET_SHADER_REQUEST_CAPTURE_ONLY)
#if !defined(GARNET_SHADER_REQUEST_CAPTURE_ONLY)
// ---------------------------------------------------------------------------------------------------------------
// RDNA4BinaryArchive (Task 3.1, Metal 3 MTLBinaryArchive):
// Versioned Garnet binary archive ('GRBA') persisting translated SPIR-V, reflection metadata JSON, and
// RADV VkPipelineCache blobs indexed by canonical SHA-256 + 64-bit pipeline descriptor keys.
//
// Wave 2B integration checklist (GarnetDevice.m + rdna4_render_pipeline_impl.inc):
//   1. GarnetDevice.m (right after #include "garnet_icb_abi.h", before #include "rdna4_render_pipeline_impl.m"):
//        #include "rdna4_binary_archive_impl.m"
//   2. GarnetDevice.m +load:
//        { const char *n[] = { "MTLBinaryArchive" }; rdna4_add_protocols([RDNA4BinaryArchive class], n, 1, "RDNA4BinaryArchive"); }
//   3. GarnetDevice.m -newBinaryArchiveWithDescriptor:error: (keep -supportsBinaryArchives NO until Task 8.2):
//        - (id<MTLBinaryArchive>)newBinaryArchiveWithDescriptor:(MTLBinaryArchiveDescriptor *)descriptor error:(NSError **)error {
//            return [[RDNA4BinaryArchive alloc] initWithDevice:self descriptor:descriptor error:error];
//        }
//   4. GarnetDevice.m compute descriptor methods (newComputePipelineStateWithDescriptor:...):
//        Forward `d` and `options` into [[RDNA4ComputePipelineState alloc] initWithDevice:self descriptor:d options:o error:&e]
//        (and return nil immediately without inproc/fallback when `o & MTLPipelineOptionFailOnBinaryArchiveMiss` fails).
//   5. rdna4_render_pipeline_impl.inc (render pipeline hook, ~8 lines):
//        Before rdna4_spv_lookup(), call:
//          BOOL baHit = NO; NSData *baVk = nil; rdna4ba_fingerprint_t baFp = {0}; NSError *baErr = nil;
//          if (!rdna4_ba_lookup_render(dev, d, options, &vsd, &mv_, &fsd, &mf_, &baVk, &baFp, &baHit, &baErr)) {
//              if (err) *err = baErr; return nil;
//          }
//        When baHit && baVk.length, wrap vkCreateGraphicsPipelines (line 862, using rdna4_ba_vkcache())
//        with rdna4_ba_vkcache_scope_begin(&baScope, baVk, &baFp) / rdna4_ba_vkcache_scope_end(&baScope).
// ---------------------------------------------------------------------------------------------------------------



// Forward declarations for pipeline helpers defined later in the same translation unit.




static NSError *rdna4_ba_err(MTLBinaryArchiveError code, NSString *msg, NSError *underlying) {
    NSMutableDictionary *info = [NSMutableDictionary dictionaryWithObject:(msg ?: @"Garnet binary archive error")
                                                                   forKey:NSLocalizedDescriptionKey];
    if (underlying) info[NSUnderlyingErrorKey] = underlying;
    return [NSError errorWithDomain:MTLBinaryArchiveDomain code:(NSInteger)code userInfo:info];
}

// ---- Vulkan pipeline-cache helpers (lazy proc lookup; no backend header edits needed) ----
static PFN_vkDestroyPipelineCache rdna4_ba_vkDestroyPipelineCache;

static void rdna4_ba_resolve_vk_procs(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        if (RDNA4R.ok && RDNA4R.dev && vkGetDeviceProcAddr)
            rdna4_ba_vkDestroyPipelineCache = (PFN_vkDestroyPipelineCache)vkGetDeviceProcAddr(RDNA4R.dev, "vkDestroyPipelineCache");
    });
}

static __thread VkPipelineCache rdna4_tl_ba_vkcache;

GARNET_INTERNAL VkPipelineCache rdna4_ba_vkcache(void) {
    return rdna4_tl_ba_vkcache ? rdna4_tl_ba_vkcache : RDNA4R.pc;
}


static BOOL rdna4_ba_device_fingerprint(rdna4ba_fingerprint_t *out, NSError **err) {
    memset(out, 0, sizeof *out);
    NSError *re = nil;
    if (!rdna4_radv_open(&re)) {
        if (err) *err = rdna4_ba_err(MTLBinaryArchiveErrorInvalidFile,
            @"Cannot open RADV backend for binary archive device fingerprint", re);
        return NO;
    }
    VkPhysicalDeviceProperties pp;
    memset(&pp, 0, sizeof pp);
    vkGetPhysicalDeviceProperties(RDNA4R.pd, &pp);
    out->garnet_abi_version = RDNA4BA_ABI_VERSION;
    out->translator_abi_version = RDNA4X_ABI_VERSION;
    out->vendor_id = pp.vendorID;
    out->device_id = pp.deviceID;
    out->driver_version = pp.driverVersion;
    out->flags = RDNA4R.gpuArgumentTables ? 1u : 0u;
    memcpy(out->pipeline_cache_uuid, pp.pipelineCacheUUID, sizeof out->pipeline_cache_uuid);

    uint8_t xlUuid[16] = {0};
    (void)rdna4x_file_uuid(rdna4x_xlib_path().fileSystemRepresentation, xlUuid);
    rdna4x_backend_capabilities qcaps = rdna4_rt_xlate_caps();
    rdna4ba_sha256_ctx sc;
    rdna4ba_sha256_init(&sc);
    rdna4ba_sha256_update(&sc, RDNA4X_KEY_TAG, sizeof RDNA4X_KEY_TAG);
    rdna4ba_sha256_update(&sc, xlUuid, sizeof xlUuid);
    rdna4ba_sha256_update_u32(&sc, RDNA4X_QUERY_POLICY_REVISION);
    rdna4ba_sha256_update_u32(&sc, qcaps.flags);
    rdna4ba_sha256_update_u32(&sc, qcaps.subgroup_size);
    rdna4ba_sha256_update_u32(&sc, qcaps.subgroup_supported_stages);
    rdna4ba_sha256_update_u32(&sc, qcaps.subgroup_supported_operations);
    rdna4ba_sha256_update_u32(&sc, out->flags);
    rdna4ba_sha256_final(&sc, out->translator_hash);
    return YES;
}

GARNET_INTERNAL void rdna4_ba_vkcache_scope_begin(rdna4_ba_vkcache_scope_t *sc, NSData *initialData,
                                         const rdna4ba_fingerprint_t *fp) {
    sc->owned = VK_NULL_HANDLE;
    sc->prev = rdna4_tl_ba_vkcache;
    if (!RDNA4R.ok || !vkCreatePipelineCache) return;
    rdna4_ba_resolve_vk_procs();
    if (!rdna4_ba_vkDestroyPipelineCache) return;

    const void *initPtr = NULL;
    size_t initLen = 0;
    if (initialData.length >= 32 && initialData.length <= RDNA4BA_MAX_ENTRY_PAYLOAD && fp &&
        rdna4pc_vk_header_ok(initialData.bytes, initialData.length,
                             fp->pipeline_cache_uuid, fp->vendor_id, fp->device_id)) {
        initPtr = initialData.bytes;
        initLen = initialData.length;
    }

    VkPipelineCacheCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
        .initialDataSize = initLen,
        .pInitialData = initPtr,
    };
    VkResult vr = vkCreatePipelineCache(RDNA4R.dev, &ci, NULL, &sc->owned);
    if (vr != VK_SUCCESS && initPtr) {
        ci.initialDataSize = 0;
        ci.pInitialData = NULL;
        vr = vkCreatePipelineCache(RDNA4R.dev, &ci, NULL, &sc->owned);
    }
    if (vr != VK_SUCCESS) {
        sc->owned = VK_NULL_HANDLE;
        return;
    }
    rdna4_tl_ba_vkcache = sc->owned;
}

static NSData *rdna4_ba_vkcache_scope_extract(const rdna4_ba_vkcache_scope_t *sc,
                                              const rdna4ba_fingerprint_t *fp) {
    if (!sc->owned || !RDNA4R.ok || !vkGetPipelineCacheData || !fp) return nil;
    size_t sz = 0;
    if (vkGetPipelineCacheData(RDNA4R.dev, sc->owned, &sz, NULL) != VK_SUCCESS ||
        sz < 32 || sz > RDNA4BA_MAX_ENTRY_PAYLOAD)
        return nil;
    NSMutableData *data = [NSMutableData dataWithLength:sz];
    if (!data) return nil;
    if (vkGetPipelineCacheData(RDNA4R.dev, sc->owned, &sz, data.mutableBytes) != VK_SUCCESS ||
        sz < 32 || sz > data.length ||
        !rdna4pc_vk_header_ok(data.bytes, sz, fp->pipeline_cache_uuid, fp->vendor_id, fp->device_id))
        return nil;
    if (sz < data.length) data.length = sz;
    return data;
}

GARNET_INTERNAL void rdna4_ba_vkcache_scope_end(rdna4_ba_vkcache_scope_t *sc) {
    rdna4_tl_ba_vkcache = sc->prev;
    if (sc->owned != VK_NULL_HANDLE && RDNA4R.ok && rdna4_ba_vkDestroyPipelineCache)
        rdna4_ba_vkDestroyPipelineCache(RDNA4R.dev, sc->owned, NULL);
    sc->owned = VK_NULL_HANDLE;
}

// ---- Canonical descriptor key builders ----
static void rdna4_ba_hash_str(rdna4ba_sha256_ctx *c, NSString *s) {
    const char *u = s.UTF8String ?: "";
    size_t n = strlen(u);
    rdna4ba_sha256_update_u64(c, (uint64_t)n);
    if (n) rdna4ba_sha256_update(c, u, n);
}

static void rdna4_ba_hash_stage_input(rdna4ba_sha256_ctx *c, MTLStageInputOutputDescriptor *sd) {
    rdna4ba_sha256_update_u32(c, sd ? 1u : 0u);
    if (!sd) return;
    rdna4ba_sha256_update_u64(c, (uint64_t)sd.indexType);
    rdna4ba_sha256_update_u64(c, (uint64_t)sd.indexBufferIndex);
    for (NSUInteger i = 0; i < 31; i++) {
        MTLAttributeDescriptor *a = sd.attributes[i];
        rdna4ba_sha256_update_u64(c, a ? (uint64_t)a.format : 0u);
        rdna4ba_sha256_update_u64(c, a ? (uint64_t)a.offset : 0u);
        rdna4ba_sha256_update_u64(c, a ? (uint64_t)a.bufferIndex : 0u);
    }
    for (NSUInteger i = 0; i < 31; i++) {
        MTLBufferLayoutDescriptor *l = sd.layouts[i];
        rdna4ba_sha256_update_u64(c, l ? (uint64_t)l.stride : 0u);
        rdna4ba_sha256_update_u64(c, l ? (uint64_t)l.stepFunction : 0u);
        rdna4ba_sha256_update_u64(c, l ? (uint64_t)l.stepRate : 0u);
    }
}

static void rdna4_ba_hash_vertex_desc(rdna4ba_sha256_ctx *c, MTLVertexDescriptor *vd) {
    rdna4ba_sha256_update_u32(c, vd ? 1u : 0u);
    if (!vd) return;
    for (NSUInteger i = 0; i < 31; i++) {
        MTLVertexAttributeDescriptor *a = vd.attributes[i];
        rdna4ba_sha256_update_u64(c, a ? (uint64_t)a.format : 0u);
        rdna4ba_sha256_update_u64(c, a ? (uint64_t)a.offset : 0u);
        rdna4ba_sha256_update_u64(c, a ? (uint64_t)a.bufferIndex : 0u);
    }
    for (NSUInteger i = 0; i < 31; i++) {
        MTLVertexBufferLayoutDescriptor *l = vd.layouts[i];
        rdna4ba_sha256_update_u64(c, l ? (uint64_t)l.stride : 0u);
        rdna4ba_sha256_update_u64(c, l ? (uint64_t)l.stepFunction : 0u);
        rdna4ba_sha256_update_u64(c, l ? (uint64_t)l.stepRate : 0u);
    }
}

static BOOL rdna4_ba_compute_key(MTLComputePipelineDescriptor *d, uint8_t keySha[32],
                                 uint64_t *keyU64, NSError **err) {
    if (!d || !d.computeFunction) {
        if (err) *err = rdna4_ba_err(MTLBinaryArchiveErrorUnexpectedElement,
            @"Compute pipeline descriptor has no computeFunction", nil);
        return NO;
    }
    NSString *fnSha = rdna4_fn_sha(d.computeFunction);
    if (!fnSha.length) {
        if (err) *err = rdna4_ba_err(MTLBinaryArchiveErrorCompilationFailure,
            @"Compute function has no AIR bitcodeData for binary archive key", nil);
        return NO;
    }
    static const char tag[] = "grba-compute-v1";
    rdna4ba_sha256_ctx c;
    rdna4ba_sha256_init(&c);
    rdna4ba_sha256_update(&c, tag, sizeof tag);
    rdna4_ba_hash_str(&c, fnSha);
    rdna4ba_sha256_update_u32(&c, d.threadGroupSizeIsMultipleOfThreadExecutionWidth ? 1u : 0u);
    rdna4ba_sha256_update_u64(&c, (uint64_t)d.maxTotalThreadsPerThreadgroup);
    BOOL icb = [d respondsToSelector:@selector(supportIndirectCommandBuffers)] && d.supportIndirectCommandBuffers;
    rdna4ba_sha256_update_u32(&c, icb ? 1u : 0u);
    NSUInteger stackDepth = [d respondsToSelector:@selector(maxCallStackDepth)] ? d.maxCallStackDepth : 0;
    rdna4ba_sha256_update_u64(&c, (uint64_t)stackDepth);
    rdna4_ba_hash_stage_input(&c, d.stageInputDescriptor);
    rdna4ba_sha256_final(&c, keySha);
    if (keyU64) *keyU64 = rdna4ba_key_u64_from_sha(keySha);
    return YES;
}

static BOOL rdna4_ba_render_key(MTLRenderPipelineDescriptor *d, uint8_t keySha[32],
                                uint64_t *keyU64, NSError **err) {
    if (!d || !d.vertexFunction) {
        if (err) *err = rdna4_ba_err(MTLBinaryArchiveErrorUnexpectedElement,
            @"Render pipeline descriptor has no vertexFunction", nil);
        return NO;
    }
    NSString *vSha = rdna4_fn_sha(d.vertexFunction);
    if (!vSha.length) {
        if (err) *err = rdna4_ba_err(MTLBinaryArchiveErrorCompilationFailure,
            @"Vertex function has no AIR bitcodeData for binary archive key", nil);
        return NO;
    }
    NSString *fSha = nil;
    if (d.fragmentFunction) {
        fSha = rdna4_fn_sha(d.fragmentFunction);
        if (!fSha.length) {
            if (err) *err = rdna4_ba_err(MTLBinaryArchiveErrorCompilationFailure,
                @"Fragment function has no AIR bitcodeData for binary archive key", nil);
            return NO;
        }
    }
    static const char tag[] = "grba-render-v1";
    rdna4ba_sha256_ctx c;
    rdna4ba_sha256_init(&c);
    rdna4ba_sha256_update(&c, tag, sizeof tag);
    rdna4_ba_hash_str(&c, vSha);
    rdna4ba_sha256_update_u32(&c, d.fragmentFunction ? 1u : 0u);
    rdna4_ba_hash_str(&c, fSha ?: @"");
    rdna4ba_sha256_update_u64(&c, (uint64_t)d.rasterSampleCount);
    rdna4ba_sha256_update_u32(&c, d.isAlphaToCoverageEnabled ? 1u : 0u);
    rdna4ba_sha256_update_u32(&c, d.isAlphaToOneEnabled ? 1u : 0u);
    rdna4ba_sha256_update_u32(&c, d.isRasterizationEnabled ? 1u : 0u);
    rdna4ba_sha256_update_u64(&c, (uint64_t)d.inputPrimitiveTopology);
    rdna4ba_sha256_update_u64(&c, (uint64_t)d.depthAttachmentPixelFormat);
    rdna4ba_sha256_update_u64(&c, (uint64_t)d.stencilAttachmentPixelFormat);
    rdna4ba_sha256_update_u32(&c, d.supportIndirectCommandBuffers ? 1u : 0u);
    rdna4ba_sha256_update_u64(&c, (uint64_t)d.maxVertexAmplificationCount);
    rdna4ba_sha256_update_u64(&c, (uint64_t)d.maxTessellationFactor);
    rdna4ba_sha256_update_u32(&c, d.isTessellationFactorScaleEnabled ? 1u : 0u);
    rdna4ba_sha256_update_u64(&c, (uint64_t)d.tessellationFactorFormat);
    rdna4ba_sha256_update_u64(&c, (uint64_t)d.tessellationControlPointIndexType);
    rdna4ba_sha256_update_u64(&c, (uint64_t)d.tessellationFactorStepFunction);
    rdna4ba_sha256_update_u64(&c, (uint64_t)d.tessellationOutputWindingOrder);
    rdna4ba_sha256_update_u64(&c, (uint64_t)d.tessellationPartitionMode);
    for (NSUInteger i = 0; i < 8; i++) {
        MTLRenderPipelineColorAttachmentDescriptor *ca = d.colorAttachments[i];
        rdna4ba_sha256_update_u64(&c, ca ? (uint64_t)ca.pixelFormat : 0u);
        rdna4ba_sha256_update_u64(&c, ca ? (uint64_t)ca.writeMask : 0u);
        rdna4ba_sha256_update_u32(&c, (ca && ca.isBlendingEnabled) ? 1u : 0u);
        rdna4ba_sha256_update_u64(&c, ca ? (uint64_t)ca.rgbBlendOperation : 0u);
        rdna4ba_sha256_update_u64(&c, ca ? (uint64_t)ca.alphaBlendOperation : 0u);
        rdna4ba_sha256_update_u64(&c, ca ? (uint64_t)ca.sourceRGBBlendFactor : 0u);
        rdna4ba_sha256_update_u64(&c, ca ? (uint64_t)ca.destinationRGBBlendFactor : 0u);
        rdna4ba_sha256_update_u64(&c, ca ? (uint64_t)ca.sourceAlphaBlendFactor : 0u);
        rdna4ba_sha256_update_u64(&c, ca ? (uint64_t)ca.destinationAlphaBlendFactor : 0u);
    }
    rdna4_ba_hash_vertex_desc(&c, d.vertexDescriptor);
    rdna4ba_sha256_final(&c, keySha);
    if (keyU64) *keyU64 = rdna4ba_key_u64_from_sha(keySha);
    return YES;
}

static BOOL rdna4_ba_function_key(id<MTLFunction> fn, MTLFunctionDescriptor *fd,
                                  uint8_t keySha[32], uint64_t *keyU64, NSError **err) {
    if (!fn) {
        if (err) *err = rdna4_ba_err(MTLBinaryArchiveErrorUnexpectedElement, @"Function is nil", nil);
        return NO;
    }
    NSString *fnSha = rdna4_fn_sha(fn);
    if (!fnSha.length) {
        if (err) *err = rdna4_ba_err(MTLBinaryArchiveErrorCompilationFailure,
            @"Function has no AIR bitcodeData for binary archive key", nil);
        return NO;
    }
    static const char tag[] = "grba-func-v1";
    rdna4ba_sha256_ctx c;
    rdna4ba_sha256_init(&c);
    rdna4ba_sha256_update(&c, tag, sizeof tag);
    rdna4_ba_hash_str(&c, fnSha);
    rdna4ba_sha256_update_u64(&c, (uint64_t)fn.functionType);
    NSString *nm = (fd && fd.specializedName.length) ? fd.specializedName :
                   ((fd && fd.name.length) ? fd.name : (fn.name ?: @""));
    rdna4_ba_hash_str(&c, nm);
    rdna4ba_sha256_update_u64(&c, fd ? (uint64_t)fd.options : 0u);
    rdna4ba_sha256_final(&c, keySha);
    if (keyU64) *keyU64 = rdna4ba_key_u64_from_sha(keySha);
    return YES;
}

// ---- Stage SPIR-V / reflection JSON helpers ----
static BOOL rdna4_ba_resolve_stage(id fn, const char *role, NSData **spvOut, NSDictionary **metaOut,
                                   NSString **fnameOut, NSError **err) {
    NSString *fname = nil;
    NSDictionary *meta = nil;
    NSError *e = nil, *xe = nil;
    NSData *spv = rdna4_spv_lookup(fn, role, &e, &fname, &meta);
    if (!spv && e.code == 41 && rdna4x_active()) {
        const BOOL isCompute = (role && strcmp(role, "kernel") == 0);
        const uint64_t dl = (isCompute && !rdna4_is_ws())
            ? UINT64_MAX
            : (rdna4_now() + (uint64_t)RDNA4G_SYNC_WAIT_APP_MS * 1000000ull);
        if (rdna4x_translate_fn_with_error(fn, role, dl, &xe)) {
            e = nil; meta = nil;
            spv = rdna4_spv_lookup(fn, role, &e, &fname, &meta);
        }
    }
    if (!spv) {
        if (err) *err = rdna4_ba_err(MTLBinaryArchiveErrorCompilationFailure,
            [NSString stringWithFormat:@"Failed to resolve %s '%@' SPIR-V for binary archive", role, fname ?: @"?"],
            xe ?: e);
        return NO;
    }
    if (!rdna4ba_spv_header_ok(spv.bytes, spv.length)) {
        if (err) *err = rdna4_ba_err(MTLBinaryArchiveErrorCompilationFailure,
            [NSString stringWithFormat:@"Invalid SPIR-V header for %s '%@'", role, fname ?: @"?"], nil);
        return NO;
    }
    if (spvOut) *spvOut = spv;
    if (metaOut) *metaOut = meta;
    if (fnameOut) *fnameOut = fname;
    return YES;
}

static BOOL rdna4_ba_encode_meta(NSDictionary *meta, BOOL required, NSData **outData, NSError **err) {
    if (outData) *outData = nil;
    if (!meta) {
        if (required) {
            if (err) *err = rdna4_ba_err(MTLBinaryArchiveErrorCompilationFailure,
                @"Shader reflection metadata (.meta.json) is required for binary archive entry", nil);
            return NO;
        }
        return YES;
    }
    if (![NSJSONSerialization isValidJSONObject:meta]) {
        if (err) *err = rdna4_ba_err(MTLBinaryArchiveErrorCompilationFailure,
            @"Shader reflection metadata is not JSON-serializable", nil);
        return NO;
    }
    NSError *je = nil;
    NSData *d = [NSJSONSerialization dataWithJSONObject:meta options:NSJSONWritingSortedKeys error:&je];
    if (!d) {
        if (err) *err = rdna4_ba_err(MTLBinaryArchiveErrorCompilationFailure,
            @"Failed to serialize shader reflection metadata", je);
        return NO;
    }
    if (outData) *outData = d;
    return YES;
}

static BOOL rdna4_ba_decode_meta(NSData *raw, NSDictionary **outMeta, NSError **err) {
    if (outMeta) *outMeta = nil;
    if (!raw.length) return YES;
    NSError *je = nil;
    id obj = [NSJSONSerialization JSONObjectWithData:raw options:0 error:&je];
    if (![obj isKindOfClass:[NSDictionary class]]) {
        if (err) *err = rdna4_ba_err(MTLBinaryArchiveErrorInvalidFile,
            @"Corrupt reflection metadata JSON inside binary archive entry", je);
        return NO;
    }
    if (outMeta) *outMeta = (NSDictionary *)obj;
    return YES;
}

// ---- RDNA4BinaryArchive implementation ----

@implementation RDNA4BinaryArchive
- (instancetype)initWithDevice:(id)dev descriptor:(MTLBinaryArchiveDescriptor *)descriptor error:(NSError **)error {
    self = [super init];
    if (!self) return nil;
    if (!dev || !descriptor) {
        if (error) *error = rdna4_ba_err(MTLBinaryArchiveErrorInvalidFile,
            @"Binary archive requires a non-nil device and MTLBinaryArchiveDescriptor", nil);
        return nil;
    }
    _dev = dev;
    _lock = [NSLock new];
    static dispatch_once_t crcOnce;
    dispatch_once(&crcOnce, ^{ (void)rdna4crc32(0, "", 0); });
    if (!rdna4_ba_device_fingerprint(&_fp, error)) return nil;
    rdna4ba_init(&_archive, &_fp);

    if (descriptor.url) {
        if (!descriptor.url.isFileURL || !descriptor.url.path.length) {
            if (error) *error = rdna4_ba_err(MTLBinaryArchiveErrorInvalidFile,
                @"Binary archive URL must be a non-empty file URL", nil);
            return nil;
        }
        rdna4ba_archive_t loaded;
        int ioErr = 0;
        rdna4ba_status_t st = rdna4ba_read_file(descriptor.url.fileSystemRepresentation, &_fp, &loaded, &ioErr);
        if (st != RDNA4BA_OK) {
            NSString *msg = ioErr
                ? [NSString stringWithFormat:@"Failed to load binary archive '%@': %s (%s)",
                   descriptor.url.path, rdna4ba_status_str(st), strerror(ioErr)]
                : [NSString stringWithFormat:@"Rejected binary archive '%@': %s",
                   descriptor.url.path, rdna4ba_status_str(st)];
            RDNA4LOG("RDNA4BinaryArchive load rejected: %s", msg.UTF8String);
            if (error) *error = rdna4_ba_err(MTLBinaryArchiveErrorInvalidFile, msg, nil);
            return nil;
        }
        rdna4ba_destroy(&_archive);
        _archive = loaded;
        RDNA4LOG("RDNA4BinaryArchive %p loaded '%s': %u entries (vendor 0x%04x device 0x%04x)",
                 (__bridge void *)self, descriptor.url.fileSystemRepresentation,
                 _archive.count, _fp.vendor_id, _fp.device_id);
    }
    return self;
}

- (void)dealloc {
    rdna4ba_destroy(&_archive);
}

RDNA4_DNR(RDNA4BinaryArchive)

- (id)device { return _dev; }

- (NSString *)label {
    [_lock lock];
    NSString *l = _lbl;
    [_lock unlock];
    return l;
}

- (void)setLabel:(NSString *)label {
    NSString *copied = [label copy];
    [_lock lock];
    _lbl = copied;
    [_lock unlock];
}

- (NSUInteger)rdna4EntryCount {
    [_lock lock];
    NSUInteger n = (NSUInteger)_archive.count;
    [_lock unlock];
    return n;
}

- (const rdna4ba_fingerprint_t *)rdna4Fingerprint {
    return &_fp;
}

- (BOOL)rdna4StoreEntry:(const rdna4ba_entry_view_t *)view error:(NSError **)error {
    [_lock lock];
    rdna4ba_status_t st = rdna4ba_put(&_archive, view);
    [_lock unlock];
    if (st != RDNA4BA_OK) {
        if (error) *error = rdna4_ba_err(
            st == RDNA4BA_ERR_INVALID ? MTLBinaryArchiveErrorUnexpectedElement : MTLBinaryArchiveErrorCompilationFailure,
            [NSString stringWithFormat:@"Failed to store binary archive entry: %s", rdna4ba_status_str(st)], nil);
        return NO;
    }
    return YES;
}

- (BOOL)rdna4LookupKind:(uint32_t)kind keyU64:(uint64_t)keyU64 keySha:(const uint8_t[32])keySha
                   spv0:(NSData **)spv0 meta0:(NSData **)meta0
                   spv1:(NSData **)spv1 meta1:(NSData **)meta1
                vkCache:(NSData **)vkCache {
    if (spv0) *spv0 = nil;
    if (meta0) *meta0 = nil;
    if (spv1) *spv1 = nil;
    if (meta1) *meta1 = nil;
    if (vkCache) *vkCache = nil;
    [_lock lock];
    const rdna4ba_entry_t *e = rdna4ba_find(&_archive, kind, keyU64, keySha);
    BOOL found = NO;
    if (e) {
        rdna4ba_entry_view_t v = rdna4ba_entry_view(e);
        if (spv0 && v.spv0_size) *spv0 = [NSData dataWithBytes:v.spv0 length:v.spv0_size];
        if (meta0 && v.meta0_size) *meta0 = [NSData dataWithBytes:v.meta0 length:v.meta0_size];
        if (spv1 && v.spv1_size) *spv1 = [NSData dataWithBytes:v.spv1 length:v.spv1_size];
        if (meta1 && v.meta1_size) *meta1 = [NSData dataWithBytes:v.meta1 length:v.meta1_size];
        if (vkCache) {
            if (v.vk_cache_size) *vkCache = [NSData dataWithBytes:v.vk_cache length:v.vk_cache_size];
            else if (_archive.vk_cache_size) *vkCache = [NSData dataWithBytes:_archive.vk_cache length:_archive.vk_cache_size];
        }
        found = YES;
    }
    [_lock unlock];
    return found;
}

- (BOOL)addComputePipelineFunctionsWithDescriptor:(MTLComputePipelineDescriptor *)descriptor error:(NSError **)error {
    if (!descriptor || !descriptor.computeFunction) {
        if (error) *error = rdna4_ba_err(MTLBinaryArchiveErrorUnexpectedElement,
            @"addComputePipelineFunctionsWithDescriptor: requires a descriptor with computeFunction", nil);
        return NO;
    }
    RDNA4_LK_SCOPE;
    rdna4x_linkage_compute(descriptor);

    uint8_t keySha[32];
    uint64_t keyU64 = 0;
    if (!rdna4_ba_compute_key(descriptor, keySha, &keyU64, error)) return NO;

    NSData *spv = nil; NSDictionary *meta = nil; NSString *fname = nil;
    if (!rdna4_ba_resolve_stage(descriptor.computeFunction, "kernel", &spv, &meta, &fname, error)) return NO;

    NSData *metaData = nil;
    if (!rdna4_ba_encode_meta(meta, YES, &metaData, error)) return NO;

    rdna4_ba_vkcache_scope_t vcScope;
    rdna4_ba_vkcache_scope_begin(&vcScope, nil, &_fp);
    NSError *perr = nil;
    id pso = rdna4_ba_compile_compute_state(_dev, descriptor.computeFunction, spv, meta, fname, &perr);
    NSData *vkData = pso ? rdna4_ba_vkcache_scope_extract(&vcScope, &_fp) : nil;
    rdna4_ba_vkcache_scope_end(&vcScope);
    if (!pso) {
        if (error) *error = rdna4_ba_err(MTLBinaryArchiveErrorCompilationFailure,
            [NSString stringWithFormat:@"Failed to compile compute pipeline '%@' for binary archive", fname ?: @"?"], perr);
        return NO;
    }

    if (spv.length > RDNA4BA_MAX_ENTRY_PAYLOAD || metaData.length > RDNA4BA_MAX_ENTRY_PAYLOAD ||
        vkData.length > RDNA4BA_MAX_ENTRY_PAYLOAD) {
        if (error) *error = rdna4_ba_err(MTLBinaryArchiveErrorCompilationFailure,
            @"Compute pipeline archive payload exceeds entry size limit", nil);
        return NO;
    }

    rdna4ba_entry_view_t view;
    memset(&view, 0, sizeof view);
    view.kind = RDNA4BA_KIND_COMPUTE;
    view.key_u64 = keyU64;
    memcpy(view.key_sha, keySha, 32);
    view.spv0 = spv.bytes; view.spv0_size = (uint32_t)spv.length;
    view.meta0 = metaData.bytes; view.meta0_size = (uint32_t)metaData.length;
    if (vkData.length) {
        view.vk_cache = vkData.bytes;
        view.vk_cache_size = (uint32_t)vkData.length;
    }
    if (![self rdna4StoreEntry:&view error:error]) return NO;
    RDNA4LOG("RDNA4BinaryArchive %p added compute '%s' (spv %u B, meta %u B, vkCache %u B)",
             (__bridge void *)self, fname.UTF8String ?: "?", view.spv0_size, view.meta0_size, view.vk_cache_size);
    return YES;
}

- (BOOL)addRenderPipelineFunctionsWithDescriptor:(MTLRenderPipelineDescriptor *)descriptor error:(NSError **)error {
    if (!descriptor || !descriptor.vertexFunction) {
        if (error) *error = rdna4_ba_err(MTLBinaryArchiveErrorUnexpectedElement,
            @"addRenderPipelineFunctionsWithDescriptor: requires a descriptor with vertexFunction", nil);
        return NO;
    }
    RDNA4_LK_SCOPE;
    rdna4x_linkage_render(descriptor);

    uint8_t keySha[32];
    uint64_t keyU64 = 0;
    if (!rdna4_ba_render_key(descriptor, keySha, &keyU64, error)) return NO;

    NSData *vsSpv = nil; NSDictionary *vsMeta = nil; NSString *vname = nil;
    if (!rdna4_ba_resolve_stage(descriptor.vertexFunction, "vertex", &vsSpv, &vsMeta, &vname, error)) return NO;

    NSData *fsSpv = nil; NSDictionary *fsMeta = nil; NSString *fname = nil;
    if (descriptor.fragmentFunction) {
        if (!rdna4_ba_resolve_stage(descriptor.fragmentFunction, "fragment", &fsSpv, &fsMeta, &fname, error)) return NO;
    }

    NSData *vsMetaData = nil, *fsMetaData = nil;
    if (!rdna4_ba_encode_meta(vsMeta, NO, &vsMetaData, error)) return NO;
    if (!rdna4_ba_encode_meta(fsMeta, NO, &fsMetaData, error)) return NO;

    MTLRenderPipelineDescriptor *cleanDesc = [descriptor copy];
    if ([cleanDesc respondsToSelector:@selector(setBinaryArchives:)]) cleanDesc.binaryArchives = nil;

    rdna4_ba_vkcache_scope_t vcScope;
    rdna4_ba_vkcache_scope_begin(&vcScope, nil, &_fp);
    NSError *perr = nil;
    Class rpCls = NSClassFromString(@"RDNA4RenderPipelineState");
    id<RDNA4RenderPipelineArchiveInit> pso = rpCls
        ? [(id<RDNA4RenderPipelineArchiveInit>)[rpCls alloc] initWithDevice:_dev descriptor:cleanDesc noFallback:YES error:&perr]
        : nil;
    VkPipeline vkp = pso ? [pso vkPipeline] : VK_NULL_HANDLE;
    NSData *vkData = (pso && vkp) ? rdna4_ba_vkcache_scope_extract(&vcScope, &_fp) : nil;
    rdna4_ba_vkcache_scope_end(&vcScope);
    if (!pso || !vkp) {
        if (error) *error = rdna4_ba_err(MTLBinaryArchiveErrorCompilationFailure,
            [NSString stringWithFormat:@"Failed to compile render pipeline '%@/%@' for binary archive",
             vname ?: @"?", fname ?: @"(none)"], perr);
        return NO;
    }

    if (vsSpv.length > RDNA4BA_MAX_ENTRY_PAYLOAD || vsMetaData.length > RDNA4BA_MAX_ENTRY_PAYLOAD ||
        fsSpv.length > RDNA4BA_MAX_ENTRY_PAYLOAD || fsMetaData.length > RDNA4BA_MAX_ENTRY_PAYLOAD ||
        vkData.length > RDNA4BA_MAX_ENTRY_PAYLOAD) {
        if (error) *error = rdna4_ba_err(MTLBinaryArchiveErrorCompilationFailure,
            @"Render pipeline archive payload exceeds entry size limit", nil);
        return NO;
    }

    rdna4ba_entry_view_t view;
    memset(&view, 0, sizeof view);
    view.kind = RDNA4BA_KIND_RENDER;
    view.key_u64 = keyU64;
    memcpy(view.key_sha, keySha, 32);
    view.spv0 = vsSpv.bytes; view.spv0_size = (uint32_t)vsSpv.length;
    if (vsMetaData.length) { view.meta0 = vsMetaData.bytes; view.meta0_size = (uint32_t)vsMetaData.length; }
    if (fsSpv.length)      { view.spv1 = fsSpv.bytes; view.spv1_size = (uint32_t)fsSpv.length; }
    if (fsMetaData.length) { view.meta1 = fsMetaData.bytes; view.meta1_size = (uint32_t)fsMetaData.length; }
    if (vkData.length)     { view.vk_cache = vkData.bytes; view.vk_cache_size = (uint32_t)vkData.length; }
    if (![self rdna4StoreEntry:&view error:error]) return NO;
    RDNA4LOG("RDNA4BinaryArchive %p added render '%s/%s' (vs %u B, fs %u B, vkCache %u B)",
             (__bridge void *)self, vname.UTF8String ?: "?", fname.UTF8String ?: "(none)",
             view.spv0_size, view.spv1_size, view.vk_cache_size);
    return YES;
}

- (BOOL)addTileRenderPipelineFunctionsWithDescriptor:(MTLTileRenderPipelineDescriptor *)descriptor error:(NSError **)error {
    (void)descriptor;
    if (error) *error = rdna4_ba_err(MTLBinaryArchiveErrorCompilationFailure,
        @"Tile render pipelines are not supported on discrete AMD RDNA 4 GPUs (Apple Silicon TBDR-only)", nil);
    return NO;
}

- (BOOL)addMeshRenderPipelineFunctionsWithDescriptor:(id)descriptor error:(NSError **)error {
    (void)descriptor;
    if (error) *error = rdna4_ba_err(MTLBinaryArchiveErrorCompilationFailure,
        @"Mesh render pipeline binary archive entries are not supported yet", nil);
    return NO;
}

- (BOOL)addFunctionWithDescriptor:(MTLFunctionDescriptor *)descriptor library:(id<MTLLibrary>)library error:(NSError **)error {
    if (!descriptor || !library || !descriptor.name.length) {
        if (error) *error = rdna4_ba_err(MTLBinaryArchiveErrorUnexpectedElement,
            @"addFunctionWithDescriptor:library:error: requires a non-nil library and named MTLFunctionDescriptor", nil);
        return NO;
    }
    NSError *ferr = nil;
    id<MTLFunction> fn = [library newFunctionWithDescriptor:descriptor error:&ferr];
    if (!fn) {
        if (error) *error = rdna4_ba_err(MTLBinaryArchiveErrorCompilationFailure,
            [NSString stringWithFormat:@"Failed to resolve function '%@' from library", descriptor.name], ferr);
        return NO;
    }
    uint8_t keySha[32];
    uint64_t keyU64 = 0;
    if (!rdna4_ba_function_key(fn, descriptor, keySha, &keyU64, error)) return NO;

    const char *role = fn.functionType == MTLFunctionTypeVertex ? "vertex" :
                       fn.functionType == MTLFunctionTypeFragment ? "fragment" :
                       fn.functionType == MTLFunctionTypeKernel ? "kernel" : "function";
    NSData *spv = nil; NSDictionary *meta = nil; NSString *fname = nil;
    if (!rdna4_ba_resolve_stage(fn, role, &spv, &meta, &fname, error)) return NO;

    NSData *metaData = nil;
    if (!rdna4_ba_encode_meta(meta, NO, &metaData, error)) return NO;

    if (spv.length > RDNA4BA_MAX_ENTRY_PAYLOAD || metaData.length > RDNA4BA_MAX_ENTRY_PAYLOAD) {
        if (error) *error = rdna4_ba_err(MTLBinaryArchiveErrorCompilationFailure,
            @"Function binary archive payload exceeds entry size limit", nil);
        return NO;
    }

    rdna4ba_entry_view_t view;
    memset(&view, 0, sizeof view);
    view.kind = RDNA4BA_KIND_FUNCTION;
    view.key_u64 = keyU64;
    memcpy(view.key_sha, keySha, 32);
    view.spv0 = spv.bytes; view.spv0_size = (uint32_t)spv.length;
    if (metaData.length) { view.meta0 = metaData.bytes; view.meta0_size = (uint32_t)metaData.length; }
    if (![self rdna4StoreEntry:&view error:error]) return NO;
    RDNA4LOG("RDNA4BinaryArchive %p added function '%s' (%s, spv %u B, meta %u B)",
             (__bridge void *)self, fname.UTF8String ?: "?", role, view.spv0_size, view.meta0_size);
    return YES;
}

- (BOOL)serializeToURL:(NSURL *)url error:(NSError **)error {
    if (!url || !url.isFileURL || !url.path.length) {
        if (error) *error = rdna4_ba_err(MTLBinaryArchiveErrorInvalidFile,
            @"serializeToURL: requires a non-empty file URL", nil);
        return NO;
    }
    int ioErr = 0;
    [_lock lock];
    rdna4ba_status_t st = rdna4ba_write_file_atomic(&_archive, url.fileSystemRepresentation, &ioErr);
    uint32_t count = _archive.count;
    [_lock unlock];
    if (st != RDNA4BA_OK) {
        NSString *msg = ioErr
            ? [NSString stringWithFormat:@"Failed to serialize binary archive to '%@': %s (%s)",
               url.path, rdna4ba_status_str(st), strerror(ioErr)]
            : [NSString stringWithFormat:@"Failed to serialize binary archive to '%@': %s",
               url.path, rdna4ba_status_str(st)];
        if (error) *error = rdna4_ba_err(MTLBinaryArchiveErrorInvalidFile, msg, nil);
        return NO;
    }
    RDNA4LOG("RDNA4BinaryArchive %p serialized %u entries to '%s'",
             (__bridge void *)self, count, url.fileSystemRepresentation);
    return YES;
}
@end

// ---- Pipeline lookup helpers (compute + render + standalone function) ----
GARNET_INTERNAL BOOL rdna4_ba_lookup_compute(id dev, MTLComputePipelineDescriptor *d, NSUInteger options,
                                    NSData **spvOut, NSDictionary **metaOut, NSData **vkCacheOut,
                                    rdna4ba_fingerprint_t *fpOut, BOOL *hitOut, NSError **err) {
    if (spvOut) *spvOut = nil;
    if (metaOut) *metaOut = nil;
    if (vkCacheOut) *vkCacheOut = nil;
    if (hitOut) *hitOut = NO;

    NSArray *archives = (d && [d respondsToSelector:@selector(binaryArchives)]) ? d.binaryArchives : nil;
    const BOOL failOnMiss = (options & RDNA4_PIPELINE_OPT_FAIL_ON_ARCHIVE_MISS) != 0;
    if (!archives.count) {
        if (failOnMiss) {
            if (err) *err = rdna4_ba_err(MTLBinaryArchiveErrorCompilationFailure,
                @"Compute pipeline missed binary archive: MTLPipelineOptionFailOnBinaryArchiveMiss set with no binaryArchives", nil);
            return NO;
        }
        return YES;
    }

    uint8_t keySha[32];
    uint64_t keyU64 = 0;
    NSError *kerr = nil;
    if (!rdna4_ba_compute_key(d, keySha, &keyU64, &kerr)) {
        if (failOnMiss) { if (err) *err = kerr; return NO; }
        return YES;
    }

    for (id obj in archives) {
        if (![obj isKindOfClass:[RDNA4BinaryArchive class]]) {
            if (err) *err = rdna4_ba_err(MTLBinaryArchiveErrorUnexpectedElement,
                @"binaryArchives contains an object that is not a Garnet MTLBinaryArchive", nil);
            return NO;
        }
        RDNA4BinaryArchive *ba = (RDNA4BinaryArchive *)obj;
        if ([ba device] != dev) {
            if (err) *err = rdna4_ba_err(MTLBinaryArchiveErrorUnexpectedElement,
                @"binaryArchives contains an archive created for a different MTLDevice", nil);
            return NO;
        }
        NSData *spv = nil, *metaRaw = nil, *vk = nil;
        if (![ba rdna4LookupKind:RDNA4BA_KIND_COMPUTE keyU64:keyU64 keySha:keySha
                            spv0:&spv meta0:&metaRaw spv1:NULL meta1:NULL vkCache:&vk])
            continue;
        if (!rdna4ba_spv_header_ok(spv.bytes, spv.length)) {
            if (err) *err = rdna4_ba_err(MTLBinaryArchiveErrorInvalidFile,
                @"Corrupt compute SPIR-V payload inside binary archive", nil);
            return NO;
        }
        NSDictionary *meta = nil;
        if (!rdna4_ba_decode_meta(metaRaw, &meta, err)) return NO;
        if (spvOut) *spvOut = spv;
        if (metaOut) *metaOut = meta;
        if (vkCacheOut) *vkCacheOut = vk;
        if (fpOut) *fpOut = *[ba rdna4Fingerprint];
        if (hitOut) *hitOut = YES;
        RDNA4LOG("binaryArchive HIT compute '%s' (spv %lu B, vkCache %lu B)",
                 ([d.computeFunction respondsToSelector:@selector(name)] ? d.computeFunction.name.UTF8String : "?") ?: "?",
                 (unsigned long)spv.length, (unsigned long)vk.length);
        return YES;
    }

    if (failOnMiss) {
        NSString *nm = [d.computeFunction respondsToSelector:@selector(name)] ? d.computeFunction.name : @"?";
        if (err) *err = rdna4_ba_err(MTLBinaryArchiveErrorCompilationFailure,
            [NSString stringWithFormat:@"Compute pipeline '%@' not found in binaryArchives (MTLPipelineOptionFailOnBinaryArchiveMiss)", nm ?: @"?"],
            nil);
        return NO;
    }
    return YES;
}

__attribute__((unused))
GARNET_INTERNAL BOOL rdna4_ba_lookup_render(id dev, MTLRenderPipelineDescriptor *d, NSUInteger options,
                                   NSData **vsSpvOut, NSDictionary **vsMetaOut,
                                   NSData **fsSpvOut, NSDictionary **fsMetaOut,
                                   NSData **vkCacheOut, rdna4ba_fingerprint_t *fpOut,
                                   BOOL *hitOut, NSError **err) {
    if (vsSpvOut) *vsSpvOut = nil;
    if (vsMetaOut) *vsMetaOut = nil;
    if (fsSpvOut) *fsSpvOut = nil;
    if (fsMetaOut) *fsMetaOut = nil;
    if (vkCacheOut) *vkCacheOut = nil;
    if (hitOut) *hitOut = NO;

    NSArray *archives = (d && [d respondsToSelector:@selector(binaryArchives)]) ? d.binaryArchives : nil;
    const BOOL failOnMiss = (options & RDNA4_PIPELINE_OPT_FAIL_ON_ARCHIVE_MISS) != 0;
    if (!archives.count) {
        if (failOnMiss) {
            if (err) *err = rdna4_ba_err(MTLBinaryArchiveErrorCompilationFailure,
                @"Render pipeline missed binary archive: MTLPipelineOptionFailOnBinaryArchiveMiss set with no binaryArchives", nil);
            return NO;
        }
        return YES;
    }

    uint8_t keySha[32];
    uint64_t keyU64 = 0;
    NSError *kerr = nil;
    if (!rdna4_ba_render_key(d, keySha, &keyU64, &kerr)) {
        if (failOnMiss) { if (err) *err = kerr; return NO; }
        return YES;
    }

    const BOOL hasFrag = d.fragmentFunction != nil;
    for (id obj in archives) {
        if (![obj isKindOfClass:[RDNA4BinaryArchive class]]) {
            if (err) *err = rdna4_ba_err(MTLBinaryArchiveErrorUnexpectedElement,
                @"binaryArchives contains an object that is not a Garnet MTLBinaryArchive", nil);
            return NO;
        }
        RDNA4BinaryArchive *ba = (RDNA4BinaryArchive *)obj;
        if ([ba device] != dev) {
            if (err) *err = rdna4_ba_err(MTLBinaryArchiveErrorUnexpectedElement,
                @"binaryArchives contains an archive created for a different MTLDevice", nil);
            return NO;
        }
        NSData *vsSpv = nil, *vsMetaRaw = nil, *fsSpv = nil, *fsMetaRaw = nil, *vk = nil;
        if (![ba rdna4LookupKind:RDNA4BA_KIND_RENDER keyU64:keyU64 keySha:keySha
                            spv0:&vsSpv meta0:&vsMetaRaw spv1:&fsSpv meta1:&fsMetaRaw vkCache:&vk])
            continue;
        if (!rdna4ba_spv_header_ok(vsSpv.bytes, vsSpv.length) ||
            (hasFrag && !rdna4ba_spv_header_ok(fsSpv.bytes, fsSpv.length)) ||
            (!hasFrag && fsSpv.length)) {
            if (err) *err = rdna4_ba_err(MTLBinaryArchiveErrorInvalidFile,
                @"Corrupt render SPIR-V payload inside binary archive", nil);
            return NO;
        }
        NSDictionary *vsMeta = nil, *fsMeta = nil;
        if (!rdna4_ba_decode_meta(vsMetaRaw, &vsMeta, err) ||
            !rdna4_ba_decode_meta(fsMetaRaw, &fsMeta, err))
            return NO;
        if (vsSpvOut) *vsSpvOut = vsSpv;
        if (vsMetaOut) *vsMetaOut = vsMeta;
        if (fsSpvOut) *fsSpvOut = fsSpv;
        if (fsMetaOut) *fsMetaOut = fsMeta;
        if (vkCacheOut) *vkCacheOut = vk;
        if (fpOut) *fpOut = *[ba rdna4Fingerprint];
        if (hitOut) *hitOut = YES;
        RDNA4LOG("binaryArchive HIT render '%s/%s' (vs %lu B, fs %lu B, vkCache %lu B)",
                 ([d.vertexFunction respondsToSelector:@selector(name)] ? d.vertexFunction.name.UTF8String : "?") ?: "?",
                 (hasFrag && [d.fragmentFunction respondsToSelector:@selector(name)] ? d.fragmentFunction.name.UTF8String : "(none)") ?: "(none)",
                 (unsigned long)vsSpv.length, (unsigned long)fsSpv.length, (unsigned long)vk.length);
        return YES;
    }

    if (failOnMiss) {
        NSString *vn = [d.vertexFunction respondsToSelector:@selector(name)] ? d.vertexFunction.name : @"?";
        NSString *fn = hasFrag && [d.fragmentFunction respondsToSelector:@selector(name)] ? d.fragmentFunction.name : @"(none)";
        if (err) *err = rdna4_ba_err(MTLBinaryArchiveErrorCompilationFailure,
            [NSString stringWithFormat:@"Render pipeline '%@/%@' not found in binaryArchives (MTLPipelineOptionFailOnBinaryArchiveMiss)",
             vn ?: @"?", fn ?: @"(none)"], nil);
        return NO;
    }
    return YES;
}

__attribute__((unused))
static BOOL rdna4_ba_lookup_function(id dev, id<MTLFunction> fn, MTLFunctionDescriptor *fd,
                                     NSArray *archives, NSData **spvOut, NSDictionary **metaOut,
                                     BOOL *hitOut, NSError **err) {
    if (spvOut) *spvOut = nil;
    if (metaOut) *metaOut = nil;
    if (hitOut) *hitOut = NO;
    if (!fn || !archives.count) return YES;

    uint8_t keySha[32];
    uint64_t keyU64 = 0;
    if (!rdna4_ba_function_key(fn, fd, keySha, &keyU64, NULL)) return YES;

    for (id obj in archives) {
        if (![obj isKindOfClass:[RDNA4BinaryArchive class]]) continue;
        RDNA4BinaryArchive *ba = (RDNA4BinaryArchive *)obj;
        if ([ba device] != dev) continue;
        NSData *spv = nil, *metaRaw = nil;
        if (![ba rdna4LookupKind:RDNA4BA_KIND_FUNCTION keyU64:keyU64 keySha:keySha
                            spv0:&spv meta0:&metaRaw spv1:NULL meta1:NULL vkCache:NULL])
            continue;
        if (!rdna4ba_spv_header_ok(spv.bytes, spv.length)) {
            if (err) *err = rdna4_ba_err(MTLBinaryArchiveErrorInvalidFile,
                @"Corrupt function SPIR-V payload inside binary archive", nil);
            return NO;
        }
        NSDictionary *meta = nil;
        if (!rdna4_ba_decode_meta(metaRaw, &meta, err)) return NO;
        if (spvOut) *spvOut = spv;
        if (metaOut) *metaOut = meta;
        if (hitOut) *hitOut = YES;
        return YES;
    }
    return YES;
}

#endif // !defined(GARNET_SHADER_REQUEST_CAPTURE_ONLY)
#if !defined(GARNET_SHADER_REQUEST_CAPTURE_ONLY)
// ---------------------------------------------------------------------------------------------------------------
// RDNA4DynamicLibrary (Task 3.2, Metal 3 MTLDynamicLibrary) and
// RDNA4FunctionHandle / RDNA4VisibleFunctionTable (Task 3.3, Metal 3 MTLVisibleFunctionTable).
// Backed by '.grdl' container codec (garnet_dynlib_vft.h), garnetxlate Stage 2 FFI (0073/0074),
// and host-coherent Shared RDNA4Buffer storage matching 0074 (`word[0]=slot_count, word[1+slot]=handle_id`).
//
// Wave 3B integration checklist:
//   1. GarnetDevice.m:
//      a) Place right after `#include "rdna4_binary_archive_impl.m"` (before render/compute pipelines):
//           #include "rdna4_dynlib_vft_impl.m"
//      b) Register protocols in +[GarnetDevice load]:
//           { const char *n[] = { "MTLDynamicLibrary" };
//             rdna4_add_protocols([RDNA4DynamicLibrary class], n, 1, "RDNA4DynamicLibrary"); }
//           { const char *n[] = { "MTLFunctionHandle" };
//             rdna4_add_protocols([RDNA4FunctionHandle class], n, 1, "RDNA4FunctionHandle"); }
//           { const char *n[] = { "MTLVisibleFunctionTable", "MTLResource" };
//             rdna4_add_protocols([RDNA4VisibleFunctionTable class], n, 2, "RDNA4VisibleFunctionTable"); }
//      c) Wire MTLDevice dynamic library factories in @implementation GarnetDevice (keep
//         supportsDynamicLibraries / supportsRenderDynamicLibraries / supportsFunctionPointers /
//         supportsFunctionPointersFromRender returning NO until live verification / Task 8.2):
//           - (id<MTLDynamicLibrary>)newDynamicLibrary:(id<MTLLibrary>)library error:(NSError **)error {
//               return (id<MTLDynamicLibrary>)[[RDNA4DynamicLibrary alloc] initWithDevice:self library:library error:error];
//           }
//           - (id<MTLDynamicLibrary>)newDynamicLibraryWithURL:(NSURL *)url error:(NSError **)error {
//               return (id<MTLDynamicLibrary>)[[RDNA4DynamicLibrary alloc] initWithDevice:self url:url error:error];
//           }
//   2. rdna4_compute_pipeline_impl.inc (RDNA4ComputePipelineState):
//      a) Add ivars: `uint64_t _vftOwnerToken; rdna4vft_reflection_t _vft;`
//      b) In initWithDevice:descriptor:options:error:, check `NSError *lkErr = rdna4_lk_error(fn);`
//         and fail immediately if non-nil.
//      c) In initWithDevice:function:spv:meta:fname:error:, initialize `_vftOwnerToken = rdna4_vft_next_owner_token();`
//         and call `if (!rdna4x_parse_vft_from_meta(meta, &_vft, err)) return nil;`.
//      d) In dealloc: `rdna4vft_refl_destroy(&_vft);`.
//      e) Add selectors:
//           - (id<MTLFunctionHandle>)functionHandleWithFunction:(id<MTLFunction>)function {
//               return rdna4_vft_pipeline_handle(self, _dev, _vftOwnerToken, &_vft, function, 0);
//           }
//           - (id<MTLVisibleFunctionTable>)newVisibleFunctionTableWithDescriptor:(MTLVisibleFunctionTableDescriptor *)d {
//               return rdna4_vft_pipeline_new_table(self, _dev, _vftOwnerToken, &_vft, d, 0);
//           }
//   3. rdna4_render_pipeline_impl.inc (RDNA4RenderPipelineState):
//      a) Add ivars: `uint64_t _vftOwnerToken; rdna4vft_reflection_t _vftV, _vftF;`
//      b) In initWithDevice:descriptor:options:noFallback:error:, call
//         `if (!rdna4_vft_validate_render_footnote10(d, NO, NO, NO, err)) return nil;`
//         and check `rdna4_lk_error(d.vertexFunction)` / `rdna4_lk_error(d.fragmentFunction)`.
//      c) After resolving mv_ / mf_, initialize `_vftOwnerToken = rdna4_vft_next_owner_token();`
//         and parse `_vftV` / `_vftF` via `rdna4x_parse_vft_from_meta(...)`.
//      d) In dealloc: `rdna4vft_refl_destroy(&_vftV); rdna4vft_refl_destroy(&_vftF);`.
//      e) Add selectors:
//           - (id<MTLFunctionHandle>)functionHandleWithFunction:(id<MTLFunction>)function stage:(MTLRenderStages)stage {
//               const rdna4vft_reflection_t *r = (stage == MTLRenderStageVertex) ? &_vftV :
//                   (stage == MTLRenderStageFragment) ? &_vftF : NULL;
//               return rdna4_vft_pipeline_handle(self, _dev, _vftOwnerToken, r, function, stage);
//           }
//           - (id<MTLVisibleFunctionTable>)newVisibleFunctionTableWithDescriptor:(MTLVisibleFunctionTableDescriptor *)d stage:(MTLRenderStages)stage {
//               const rdna4vft_reflection_t *r = (stage == MTLRenderStageVertex) ? &_vftV :
//                   (stage == MTLRenderStageFragment) ? &_vftF : NULL;
//               return rdna4_vft_pipeline_new_table(self, _dev, _vftOwnerToken, r, d, stage);
//           }
//   4. rdna4_compute_encoder_impl.inc & rdna4_render_encoder_impl.inc:
//      a) Compute:
//           - (void)setVisibleFunctionTable:(id<MTLVisibleFunctionTable>)t atBufferIndex:(NSUInteger)i {
//               if (!t) { [self setBuffer:nil offset:0 atIndex:i]; return; }
//               if (![t isKindOfClass:[RDNA4VisibleFunctionTable class]]) {
//                   [_cb rdna4Fail:@"compute setVisibleFunctionTable: not an RDNA4VisibleFunctionTable"]; return;
//               }
//               RDNA4VisibleFunctionTable *vft = (RDNA4VisibleFunctionTable *)t;
//               [vft rdna4RetainIntoCommandBuffer:_cb];
//               [self setBuffer:[vft rdna4BackingBuffer] offset:0 atIndex:i];
//           }
//           - (void)setVisibleFunctionTables:(const id<MTLVisibleFunctionTable> __unsafe_unretained *)t withBufferRange:(NSRange)r {
//               for (NSUInteger k = 0; k < r.length; k++) [self setVisibleFunctionTable:t[k] atBufferIndex:r.location + k];
//           }
//      b) Render (vertex & fragment): same pattern routing to `rdna4SetBuffer:[vft rdna4BackingBuffer] offset:0 index:i frag:(NO|YES)`.
// ---------------------------------------------------------------------------------------------------------------
#ifndef RDNA4_DYNLIB_VFT_IMPL_INC
#define RDNA4_DYNLIB_VFT_IMPL_INC


static NSError *rdna4_dl_err(MTLDynamicLibraryError code, NSString *msg, NSError *underlying) {
    NSMutableDictionary *info = [NSMutableDictionary dictionaryWithObject:(msg ?: @"Garnet dynamic library error")
                                                                   forKey:NSLocalizedDescriptionKey];
    if (underlying) info[NSUnderlyingErrorKey] = underlying;
    return [NSError errorWithDomain:MTLDynamicLibraryDomain code:(NSInteger)code userInfo:info];
}

// Guard private/SPI selector probes by verifying the runtime method actually returns an ObjC object ('@').
static id rdna4_dl_objc_prop(id obj, const char *selName) {
    if (!obj || !selName) return nil;
    SEL sel = sel_registerName(selName);
    Method m = class_getInstanceMethod(object_getClass(obj), sel);
    if (!m) return nil;
    char rt[8] = "";
    method_getReturnType(m, rt, sizeof rt);
    if (rt[0] != '@') return nil;
    return ((id (*)(id, SEL))objc_msgSend)(obj, sel);
}

// For Cocoa +1 ownership selectors (`new*`/`copy*`), transfer ownership via CFBridgingRelease so ARC does not leak.
static id rdna4_dl_objc_new1(id obj, const char *selName, id arg) {
    if (!obj || !selName) return nil;
    SEL sel = sel_registerName(selName);
    Method m = class_getInstanceMethod(object_getClass(obj), sel);
    if (!m) return nil;
    char rt[8] = "";
    method_getReturnType(m, rt, sizeof rt);
    if (rt[0] != '@') return nil;
    void *raw = ((void *(*)(id, SEL, id))objc_msgSend)(obj, sel, arg);
    return raw ? CFBridgingRelease(raw) : nil;
}

static uint32_t rdna4_dl_detect_payload_kind(NSData *data) {
    if (!data.length) return 0u;
    if (rdna4dl_is_bitcode_magic(data.bytes, data.length)) return RDNA4DL_PAYLOAD_AIR_BITCODE;
    if (rdna4dl_is_metallib_magic(data.bytes, data.length)) return RDNA4DL_PAYLOAD_METALLIB;
    if (memchr(data.bytes, '\0', data.length) == NULL) {
        NSString *txt = [[NSString alloc] initWithBytes:data.bytes length:data.length encoding:NSUTF8StringEncoding];
        if (txt && ([txt containsString:@"define "] || [txt containsString:@"target "] || [txt containsString:@"; ModuleID"]))
            return RDNA4DL_PAYLOAD_LLVM_IR;
    }
    return 0u;
}

static BOOL rdna4_dl_append_unique_module(rdna4dl_container_t *container,
                                          NSMutableSet<NSString *> *seenShas,
                                          NSData *payload,
                                          uint32_t kind,
                                          NSError **err) {
    if (!payload.length || !rdna4dl_payload_kind_ok(kind)) return YES;
    NSString *sha = rdna4_sha256hex(payload);
    if ([seenShas containsObject:sha]) return YES;
    rdna4dl_status_t st = rdna4dl_add_module(container, kind, payload.bytes, payload.length);
    if (st != RDNA4DL_OK) {
        if (err) *err = rdna4_dl_err(MTLDynamicLibraryErrorCompilationFailure,
            [NSString stringWithFormat:@"Failed to add dynamic library payload module: %s", rdna4dl_status_str(st)], nil);
        return NO;
    }
    [seenShas addObject:sha];
    return YES;
}

// =================================================================================================
// RDNA4DynamicLibrary (MTLDynamicLibrary)
// =================================================================================================

@implementation RDNA4DynamicLibrary

- (void)rdna4RebuildCachesLocked {
    NSMutableArray<NSDictionary *> *mods = [NSMutableArray arrayWithCapacity:_container.module_count];
    for (uint32_t i = 0; i < _container.module_count; i++) {
        const rdna4dl_module_t *m = &_container.modules[i];
        NSData *d = [NSData dataWithBytes:m->data length:m->size];
        if (!d) continue;
        NSString *sha = rdna4_sha256hex(d);
        NSString *mname = (_container.module_count == 1u)
            ? (_installName ?: @"dylib")
            : [NSString stringWithFormat:@"%@#%u", _installName ?: @"dylib", i];
        [mods addObject:@{ @"name": mname, @"kind": @(m->kind), @"data": d, @"sha": sha }];
    }
    _modulesCache = [mods copy];

    NSMutableArray<NSString *> *syms = [NSMutableArray arrayWithCapacity:_container.symbol_count];
    for (uint32_t i = 0; i < _container.symbol_count; i++) {
        NSString *s = [NSString stringWithUTF8String:_container.symbols[i]];
        if (s) [syms addObject:s];
    }
    _symbolsCache = [syms copy];
}

- (instancetype)initWithDevice:(id)dev library:(id<MTLLibrary>)library error:(NSError **)error {
    if (error) *error = nil;
    if (!dev || !library) {
        if (error) *error = rdna4_dl_err(MTLDynamicLibraryErrorInvalidFile,
            @"newDynamicLibrary:error: requires a non-nil device and MTLLibrary", nil);
        return nil;
    }
    if ([library respondsToSelector:@selector(device)]) {
        id libDev = [library device];
        if (libDev && libDev != dev) {
            if (error) *error = rdna4_dl_err(MTLDynamicLibraryErrorCompilationFailure,
                @"MTLLibrary belongs to a different MTLDevice", nil);
            return nil;
        }
    }
    if ([library respondsToSelector:@selector(type)]) {
        MTLLibraryType ltype = [library type];
        if (ltype != MTLLibraryTypeDynamic) {
            if (error) *error = rdna4_dl_err(MTLDynamicLibraryErrorCompilationFailure,
                @"MTLDynamicLibrary requires an MTLLibrary compiled with MTLLibraryTypeDynamic", nil);
            return nil;
        }
    }
    NSString *iname = [library respondsToSelector:@selector(installName)] ? [library installName] : nil;
    if (![iname isKindOfClass:[NSString class]] || !iname.length) {
        if (error) *error = rdna4_dl_err(MTLDynamicLibraryErrorUnresolvedInstallName,
            @"MTLLibrary has no installName for dynamic library creation", nil);
        return nil;
    }

    self = [super init];
    if (!self) return nil;
    _dev = dev;
    _lock = [NSLock new];
    _installName = [iname copy];
    if ([library respondsToSelector:@selector(label)] && [library label])
        _lbl = [[library label] copy];
    rdna4dl_init(&_container);

    const char *inameUtf8 = _installName.UTF8String;
    rdna4dl_status_t st = rdna4dl_set_install_name(&_container, inameUtf8 ?: "", inameUtf8 ? strlen(inameUtf8) : 0u);
    if (st != RDNA4DL_OK) {
        if (error) *error = rdna4_dl_err(MTLDynamicLibraryErrorUnresolvedInstallName,
            [NSString stringWithFormat:@"Invalid dynamic library installName: %s", rdna4dl_status_str(st)], nil);
        return nil;
    }

    NSMutableArray<NSString *> *externNames = [NSMutableArray array];
    NSMutableArray<NSString *> *funcNames = [NSMutableArray array];
    NSMutableArray<NSString *> *allNames = [NSMutableArray array];
    NSMutableSet<NSString *> *seenNames = [NSMutableSet set];
    {
        id extArr = rdna4_dl_objc_prop(library, "externFunctionNames");
        if ([extArr isKindOfClass:[NSArray class]]) {
            for (id item in (NSArray *)extArr) {
                if ([item isKindOfClass:[NSString class]] && [(NSString *)item length] && ![seenNames containsObject:item]) {
                    [seenNames addObject:item];
                    [externNames addObject:item];
                    [allNames addObject:item];
                }
            }
        }
        id fnArr = rdna4_dl_objc_prop(library, "functionNames");
        if ([fnArr isKindOfClass:[NSArray class]]) {
            for (id item in (NSArray *)fnArr) {
                if ([item isKindOfClass:[NSString class]] && [(NSString *)item length] && ![seenNames containsObject:item]) {
                    [seenNames addObject:item];
                    [funcNames addObject:item];
                    [allNames addObject:item];
                }
            }
        }
    }

    NSMutableSet<NSString *> *seenShas = [NSMutableSet set];
    NSData *metallibFallback = nil;

    // Extract whole-library bitcodeData FIRST: on macOS 26+, MTLLibraryTypeDynamic carries the full
    // AIR bitcode here while calling newExternFunctionWithName: on a [[visible]] functionNames entry
    // aborts inside Metal.framework (validateMTLFunctionType).
    id libBcObj = rdna4_dl_objc_prop(library, "bitcodeData");
    if ([libBcObj isKindOfClass:[NSData class]]) {
        NSData *libBc = (NSData *)libBcObj;
        uint32_t kind = rdna4_dl_detect_payload_kind(libBc);
        if (kind == RDNA4DL_PAYLOAD_AIR_BITCODE || kind == RDNA4DL_PAYLOAD_LLVM_IR) {
            if (!rdna4_dl_append_unique_module(&_container, seenShas, libBc, kind, error)) return nil;
        } else if (kind == RDNA4DL_PAYLOAD_METALLIB) {
            metallibFallback = libBc;
        }
    }

    // Only probe per-function bitcode when whole-library AIR/IR bitcode was not already extracted.
    // Call newExternFunctionWithName: strictly on externFunctionNames (NEVER on functionNames).
    if (_container.module_count == 0u) {
        for (NSString *fnName in externNames) {
            id fn = rdna4_dl_objc_new1(library, "newExternFunctionWithName:", fnName);
            id bcObj = rdna4_dl_objc_prop(fn, "bitcodeData");
            if (![bcObj isKindOfClass:[NSData class]]) continue;
            NSData *bc = (NSData *)bcObj;
            uint32_t kind = rdna4_dl_detect_payload_kind(bc);
            if (kind == RDNA4DL_PAYLOAD_AIR_BITCODE || kind == RDNA4DL_PAYLOAD_LLVM_IR) {
                if (!rdna4_dl_append_unique_module(&_container, seenShas, bc, kind, error)) return nil;
            }
        }
        if ([library respondsToSelector:@selector(newFunctionWithName:)]) {
            for (NSString *fnName in funcNames) {
                id fn = [library newFunctionWithName:fnName];
                id bcObj = rdna4_dl_objc_prop(fn, "bitcodeData");
                if (![bcObj isKindOfClass:[NSData class]]) continue;
                NSData *bc = (NSData *)bcObj;
                uint32_t kind = rdna4_dl_detect_payload_kind(bc);
                if (kind == RDNA4DL_PAYLOAD_AIR_BITCODE || kind == RDNA4DL_PAYLOAD_LLVM_IR) {
                    if (!rdna4_dl_append_unique_module(&_container, seenShas, bc, kind, error)) return nil;
                }
            }
        }
    }

    if (_container.module_count == 0u && metallibFallback) {
        if (!rdna4_dl_append_unique_module(&_container, seenShas, metallibFallback, RDNA4DL_PAYLOAD_METALLIB, error)) return nil;
    }

    if (_container.module_count == 0u) {
        if (error) *error = rdna4_dl_err(MTLDynamicLibraryErrorCompilationFailure,
            [NSString stringWithFormat:@"Dynamic library '%@' has no extractable AIR/LLVM bitcode modules", _installName], nil);
        return nil;
    }

    // Enumerate exported symbols via garnetx_list_exported_symbols when the translator is available.
    BOOL translatorEnumerated = NO;
    for (uint32_t i = 0; i < _container.module_count; i++) {
        const rdna4dl_module_t *m = &_container.modules[i];
        if (m->kind != RDNA4DL_PAYLOAD_AIR_BITCODE && m->kind != RDNA4DL_PAYLOAD_LLVM_IR) continue;
        NSArray<NSString *> *syms = nil;
        NSError *symErr = nil;
        if (rdna4x_list_exported_symbols_for_module(m->data, m->size, m->kind, &syms, &symErr)) {
            translatorEnumerated = YES;
            for (NSString *s in syms) {
                const char *u = s.UTF8String;
                if (!u || !u[0]) continue;
                rdna4dl_status_t ast = rdna4dl_add_symbol_len(&_container, u, strlen(u), 0);
                if (ast != RDNA4DL_OK) {
                    if (error) *error = rdna4_dl_err(MTLDynamicLibraryErrorCompilationFailure,
                        [NSString stringWithFormat:@"Exported symbol table overflow: %s", rdna4dl_status_str(ast)], nil);
                    return nil;
                }
            }
        } else if (rdna4x_stage2_symbols_available()) {
            // Translator is loaded and explicitly refused the module's AIR/IR.
            if (error) *error = rdna4_dl_err(MTLDynamicLibraryErrorCompilationFailure,
                [NSString stringWithFormat:@"Dynamic library '%@' failed symbol enumeration: %@",
                 _installName, symErr.localizedDescription ?: @"unknown error"], symErr);
            return nil;
        }
    }
    if (!translatorEnumerated) {
        for (NSString *nm in allNames) {
            const char *u = nm.UTF8String;
            if (u && u[0]) (void)rdna4dl_add_symbol_len(&_container, u, strlen(u), 0);
        }
    }

    [self rdna4RebuildCachesLocked];
    RDNA4LOG("RDNA4DynamicLibrary %p '%s': %u module(s), %u exported symbol(s)",
             (__bridge void *)self, _installName.UTF8String, _container.module_count, _container.symbol_count);
    return self;
}

- (instancetype)initWithDevice:(id)dev url:(NSURL *)url error:(NSError **)error {
    if (error) *error = nil;
    if (!dev || !url || ![url isFileURL] || !url.path.length) {
        if (error) *error = rdna4_dl_err(MTLDynamicLibraryErrorInvalidFile,
            @"newDynamicLibraryWithURL:error: requires a valid file URL", nil);
        return nil;
    }
    self = [super init];
    if (!self) return nil;
    _dev = dev;
    _lock = [NSLock new];
    rdna4dl_init(&_container);

    int ioErr = 0;
    rdna4dl_status_t st = rdna4dl_read_file(url.fileSystemRepresentation, &_container, &ioErr);
    if (st != RDNA4DL_OK) {
        NSError *under = ioErr ? [NSError errorWithDomain:NSPOSIXErrorDomain code:ioErr userInfo:nil] : nil;
        if (error) *error = rdna4_dl_err(MTLDynamicLibraryErrorInvalidFile,
            [NSString stringWithFormat:@"Failed to load Garnet GRDL dynamic library '%@': %s",
             url.path, rdna4dl_status_str(st)], under);
        return nil;
    }
    _installName = [NSString stringWithUTF8String:_container.install_name];
    if (!_installName.length) {
        if (error) *error = rdna4_dl_err(MTLDynamicLibraryErrorUnresolvedInstallName,
            @"Loaded GRDL dynamic library has invalid UTF-8 installName", nil);
        return nil;
    }
    [self rdna4RebuildCachesLocked];
    RDNA4LOG("RDNA4DynamicLibrary %p loaded '%s' from %s (%u module(s), %u symbol(s))",
             (__bridge void *)self, _installName.UTF8String, url.fileSystemRepresentation,
             _container.module_count, _container.symbol_count);
    return self;
}

- (void)dealloc {
    rdna4dl_destroy(&_container);
}

RDNA4_DNR(RDNA4DynamicLibrary)

- (id)device { return _dev; }
- (NSString *)installName { return _installName; }
- (NSString *)label { [_lock lock]; NSString *l = _lbl; [_lock unlock]; return l; }
- (void)setLabel:(NSString *)label { [_lock lock]; _lbl = [label copy]; [_lock unlock]; }
- (NSArray<NSDictionary *> *)rdna4PayloadModules { return _modulesCache ?: @[]; }
- (NSArray<NSString *> *)rdna4ExportedSymbols { return _symbolsCache ?: @[]; }

- (BOOL)rdna4HasExportedSymbol:(NSString *)symbol {
    if (!symbol.length) return NO;
    [_lock lock];
    BOOL found = rdna4dl_has_symbol(&_container, symbol.UTF8String) ? YES : NO;
    [_lock unlock];
    return found;
}

- (BOOL)serializeToURL:(NSURL *)url error:(NSError **)error {
    if (error) *error = nil;
    if (!url || ![url isFileURL] || !url.path.length) {
        if (error) *error = rdna4_dl_err(MTLDynamicLibraryErrorInvalidFile,
            @"serializeToURL:error: requires a valid file URL", nil);
        return NO;
    }
    [_lock lock];
    int ioErr = 0;
    rdna4dl_status_t st = rdna4dl_write_file_atomic(&_container, url.fileSystemRepresentation, &ioErr);
    [_lock unlock];
    if (st != RDNA4DL_OK) {
        NSError *under = ioErr ? [NSError errorWithDomain:NSPOSIXErrorDomain code:ioErr userInfo:nil] : nil;
        if (error) *error = rdna4_dl_err(MTLDynamicLibraryErrorInvalidFile,
            [NSString stringWithFormat:@"Failed to serialize GRDL dynamic library '%@' to '%@': %s",
             _installName, url.path, rdna4dl_status_str(st)], under);
        return NO;
    }
    return YES;
}

@end

// =================================================================================================
// RDNA4FunctionHandle (MTLFunctionHandle)
// =================================================================================================

@implementation RDNA4FunctionHandle

- (instancetype)initWithDevice:(id)dev
                 ownerPipeline:(id)pso
                    ownerToken:(uint64_t)token
                         stage:(NSUInteger)stage
                  functionType:(MTLFunctionType)ftype
                          name:(NSString *)name
                      handleId:(uint32_t)handleId {
    if (!dev || !token || !name.length || handleId == RDNA4VFT_HANDLE_NULL) return nil;
    self = [super init];
    if (!self) return nil;
    _dev = dev;
    _ownerPipeline = pso;
    _ownerToken = token;
    _stage = stage;
    _functionType = ftype;
    _name = [name copy];
    _handleId = handleId;
    return self;
}

RDNA4_DNR(RDNA4FunctionHandle)

- (id)device { return _dev; }
- (MTLFunctionType)functionType { return _functionType; }
- (NSString *)name { return _name; }
- (uint32_t)rdna4HandleId { return _handleId; }
- (uint64_t)rdna4OwnerToken { return _ownerToken; }
- (NSUInteger)rdna4Stage { return _stage; }
- (id)rdna4OwnerPipeline { return _ownerPipeline; }

@end

// =================================================================================================
// RDNA4VisibleFunctionTable (MTLVisibleFunctionTable + MTLResource)
// =================================================================================================

@implementation RDNA4VisibleFunctionTable

- (BOOL)rdna4SyncBackingLocked {
    void *ptr = _backing ? [_backing contents] : NULL;
    if (!ptr) return NO;
    size_t capWords = [_backing length] / sizeof(uint32_t);
    size_t written = 0;
    return rdna4vft_encode_buffer(&_table, (uint32_t *)ptr, capWords, &written) == RDNA4VFT_OK;
}

- (instancetype)initWithDevice:(id)dev
                    ownerToken:(uint64_t)token
                         stage:(NSUInteger)stage
                 functionCount:(NSUInteger)count
                   maxHandleId:(uint32_t)maxHandleId
                         error:(NSError **)error {
    if (error) *error = nil;
    if (!dev || count > RDNA4VFT_MAX_SLOTS) {
        if (error) *error = rdna4_err(49, @"visible function table descriptor has invalid device or functionCount");
        return nil;
    }
    self = [super init];
    if (!self) return nil;
    _dev = dev;
    _ownerToken = token;
    _stage = stage;
    _lock = [NSLock new];

    rdna4vft_status_t st = rdna4vft_init(&_table, (uint32_t)count, maxHandleId);
    if (st != RDNA4VFT_OK) {
        if (error) *error = rdna4_err(49, [NSString stringWithFormat:@"visible function table init failed: %s", rdna4vft_status_str(st)]);
        return nil;
    }

    // Backing Shared RDNA4Buffer holds [slot_count, handle_id_0..handle_id_{N-1}] matching 0074 (>= 16 B aligned).
    size_t needBytes = rdna4vft_buffer_bytes((uint32_t)count);
    size_t allocBytes = (needBytes + 15u) & ~(size_t)15u;
    if (allocBytes < 16u) allocBytes = 16u;
    _backing = [[RDNA4Buffer alloc] initWithDevice:dev length:allocBytes options:MTLResourceStorageModeShared error:error];
    void *ptr = _backing ? [_backing contents] : NULL;
    if (!ptr) {
        rdna4vft_destroy(&_table);
        return nil;
    }
    memset(ptr, 0, allocBytes);
    if (![self rdna4SyncBackingLocked]) {
        rdna4vft_destroy(&_table);
        if (error) *error = rdna4_err(49, @"failed to initialize visible function table backing buffer");
        return nil;
    }
    RDNA4LOGR("RDNA4VisibleFunctionTable %p: slots=%lu maxHandle=%u buf=%lu B",
              (__bridge void *)self, (unsigned long)count, maxHandleId, (unsigned long)allocBytes);
    return self;
}

- (void)dealloc {
    rdna4vft_destroy(&_table);
}

RDNA4_DNR(RDNA4VisibleFunctionTable)

- (BOOL)rdna4ValidateHandleLocked:(id)function handleIdOut:(uint32_t *)hidOut {
    *hidOut = RDNA4VFT_HANDLE_NULL;
    if (!function) return YES;
    if (![function isKindOfClass:[RDNA4FunctionHandle class]]) {
        RDNA4LOG_WARN("RDNA4VisibleFunctionTable: rejected non-RDNA4FunctionHandle object");
        return NO;
    }
    RDNA4FunctionHandle *fh = (RDNA4FunctionHandle *)function;
    if ([fh device] != _dev || [fh rdna4OwnerToken] != _ownerToken || [fh rdna4Stage] != _stage) {
        RDNA4LOG_WARN("RDNA4VisibleFunctionTable: rejected handle '%s' from mismatched pipeline/stage/device",
                      [fh name].UTF8String ?: "?");
        return NO;
    }
    uint32_t hid = [fh rdna4HandleId];
    if (hid == RDNA4VFT_HANDLE_NULL || hid > _table.max_handle_id) {
        RDNA4LOG_WARN("RDNA4VisibleFunctionTable: rejected out-of-range handle id %u (max %u)", hid, _table.max_handle_id);
        return NO;
    }
    *hidOut = hid;
    return YES;
}

- (void)setFunction:(id<MTLFunctionHandle>)function atIndex:(NSUInteger)index {
    [_lock lock];
    if (index >= _table.slot_count) {
        RDNA4LOG_WARN("RDNA4VisibleFunctionTable setFunction:atIndex:%lu out of bounds (count=%u)",
                      (unsigned long)index, _table.slot_count);
        [_lock unlock];
        return;
    }
    uint32_t hid = RDNA4VFT_HANDLE_NULL;
    if ([self rdna4ValidateHandleLocked:function handleIdOut:&hid]) {
        if (rdna4vft_set_slot(&_table, (uint32_t)index, hid) == RDNA4VFT_OK)
            (void)[self rdna4SyncBackingLocked];
    }
    [_lock unlock];
}

- (void)setFunctions:(const id<MTLFunctionHandle> __unsafe_unretained *)functions withRange:(NSRange)range {
    if (range.length == 0) return;
    if (!functions) {
        RDNA4LOG_WARN("RDNA4VisibleFunctionTable setFunctions:withRange: NULL array");
        return;
    }
    [_lock lock];
    if (range.location > _table.slot_count || range.length > _table.slot_count - range.location) {
        RDNA4LOG_WARN("RDNA4VisibleFunctionTable setFunctions:withRange:[%lu,+%lu) out of bounds (count=%u)",
                      (unsigned long)range.location, (unsigned long)range.length, _table.slot_count);
        [_lock unlock];
        return;
    }
    uint32_t *ids = (uint32_t *)malloc(range.length * sizeof(uint32_t));
    if (!ids) { [_lock unlock]; return; }
    BOOL valid = YES;
    for (NSUInteger i = 0; i < range.length; i++) {
        if (![self rdna4ValidateHandleLocked:functions[i] handleIdOut:&ids[i]]) {
            valid = NO;
            break;
        }
    }
    if (valid && rdna4vft_set_range(&_table, (uint32_t)range.location, (uint32_t)range.length, ids) == RDNA4VFT_OK)
        (void)[self rdna4SyncBackingLocked];
    free(ids);
    [_lock unlock];
}

- (void)setBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index {
    if (index >= RDNA4VFT_MAX_ATTACH_BUFFERS) {
        RDNA4LOG_WARN("RDNA4VisibleFunctionTable setBuffer:atIndex:%lu out of bounds", (unsigned long)index);
        return;
    }
    if (buffer && (![buffer isKindOfClass:[RDNA4Buffer class]] || [(RDNA4Buffer *)buffer device] != _dev)) {
        RDNA4LOG_WARN("RDNA4VisibleFunctionTable setBuffer: invalid buffer class or device");
        return;
    }
    [_lock lock];
    _attachedBuffers[index] = (RDNA4Buffer *)buffer;
    _attachedBufferOffsets[index] = buffer ? offset : 0u;
    [_lock unlock];
}

- (void)setBuffers:(const id<MTLBuffer> __unsafe_unretained *)buffers
           offsets:(const NSUInteger *)offsets
         withRange:(NSRange)range {
    if (range.length == 0) return;
    if (!buffers || !offsets ||
        range.location > RDNA4VFT_MAX_ATTACH_BUFFERS ||
        range.length > RDNA4VFT_MAX_ATTACH_BUFFERS - range.location) {
        RDNA4LOG_WARN("RDNA4VisibleFunctionTable setBuffers:offsets:withRange: invalid range or NULL array");
        return;
    }
    for (NSUInteger i = 0; i < range.length; i++)
        [self setBuffer:buffers[i] offset:offsets[i] atIndex:range.location + i];
}

- (void)setVisibleFunctionTable:(id<MTLVisibleFunctionTable>)functionTable atBufferIndex:(NSUInteger)bufferIndex {
    if (bufferIndex >= RDNA4VFT_MAX_NESTED_TABLES) {
        RDNA4LOG_WARN("RDNA4VisibleFunctionTable setVisibleFunctionTable:atBufferIndex:%lu out of bounds",
                      (unsigned long)bufferIndex);
        return;
    }
    if (functionTable == (id)self) {
        RDNA4LOG_WARN("RDNA4VisibleFunctionTable: rejected cyclic self-attachment at buffer index %lu",
                      (unsigned long)bufferIndex);
        return;
    }
    if (functionTable &&
        (![functionTable isKindOfClass:[RDNA4VisibleFunctionTable class]] ||
         [(RDNA4VisibleFunctionTable *)functionTable device] != _dev)) {
        RDNA4LOG_WARN("RDNA4VisibleFunctionTable: rejected invalid nested table class or device");
        return;
    }
    [_lock lock];
    _nestedTables[bufferIndex] = functionTable;
    [_lock unlock];
}

- (void)setVisibleFunctionTables:(const id<MTLVisibleFunctionTable> __unsafe_unretained *)functionTables
                 withBufferRange:(NSRange)bufferRange {
    if (bufferRange.length == 0) return;
    if (!functionTables ||
        bufferRange.location > RDNA4VFT_MAX_NESTED_TABLES ||
        bufferRange.length > RDNA4VFT_MAX_NESTED_TABLES - bufferRange.location) {
        RDNA4LOG_WARN("RDNA4VisibleFunctionTable setVisibleFunctionTables:withBufferRange: invalid range or NULL array");
        return;
    }
    for (NSUInteger i = 0; i < bufferRange.length; i++)
        [self setVisibleFunctionTable:functionTables[i] atBufferIndex:bufferRange.location + i];
}

- (RDNA4Buffer *)rdna4BackingBuffer { return _backing; }
- (uint64_t)rdna4BackingGPUAddress { return _backing ? [_backing gpuAddress] : 0u; }
- (NSUInteger)rdna4FunctionCount { return (NSUInteger)_table.slot_count; }
- (uint64_t)rdna4OwnerToken { return _ownerToken; }
- (NSUInteger)rdna4Stage { return _stage; }

- (uint32_t)rdna4HandleIdAtIndex:(NSUInteger)index {
    [_lock lock];
    uint32_t hid = index <= UINT32_MAX ? rdna4vft_get_slot(&_table, (uint32_t)index) : RDNA4VFT_HANDLE_NULL;
    [_lock unlock];
    return hid;
}

- (RDNA4Buffer *)rdna4AttachedBufferAtIndex:(NSUInteger)index offset:(NSUInteger *)offsetOut {
    if (offsetOut) *offsetOut = 0;
    if (index >= RDNA4VFT_MAX_ATTACH_BUFFERS) return nil;
    [_lock lock];
    RDNA4Buffer *b = _attachedBuffers[index];
    if (offsetOut) *offsetOut = _attachedBufferOffsets[index];
    [_lock unlock];
    return b;
}

- (id)rdna4NestedTableAtBufferIndex:(NSUInteger)index {
    if (index >= RDNA4VFT_MAX_NESTED_TABLES) return nil;
    [_lock lock];
    id nested = _nestedTables[index];
    [_lock unlock];
    return nested;
}

- (void)rdna4RetainIntoCommandBuffer:(id)cb depth:(unsigned)depth {
    if (!cb || depth > 8u) return;
    SEL retainSel = sel_registerName("rdna4RetainEncoded:");
    if (![cb respondsToSelector:retainSel]) return;
    void (*retainFn)(id, SEL, id) = (void (*)(id, SEL, id))objc_msgSend;
    retainFn(cb, retainSel, self);
    if (_backing) retainFn(cb, retainSel, _backing);
    [_lock lock];
    for (uint32_t i = 0; i < RDNA4VFT_MAX_ATTACH_BUFFERS; i++) {
        if (_attachedBuffers[i]) retainFn(cb, retainSel, _attachedBuffers[i]);
    }
    NSMutableArray *nested = nil;
    for (uint32_t i = 0; i < RDNA4VFT_MAX_NESTED_TABLES; i++) {
        if (_nestedTables[i]) {
            if (!nested) nested = [NSMutableArray array];
            [nested addObject:_nestedTables[i]];
        }
    }
    [_lock unlock];
    for (RDNA4VisibleFunctionTable *nt in nested)
        [nt rdna4RetainIntoCommandBuffer:cb depth:depth + 1u];
}

- (void)rdna4RetainIntoCommandBuffer:(id)cb {
    [self rdna4RetainIntoCommandBuffer:cb depth:0u];
}

// ---- MTLResource surface ----
- (id)device { return _dev; }
- (NSString *)label { [_lock lock]; NSString *l = _lbl; [_lock unlock]; return l; }
- (void)setLabel:(NSString *)label { [_lock lock]; _lbl = [label copy]; [_lock unlock]; }
- (MTLStorageMode)storageMode { return MTLStorageModeShared; }
- (MTLCPUCacheMode)cpuCacheMode { return MTLCPUCacheModeDefaultCache; }
- (MTLHazardTrackingMode)hazardTrackingMode { return MTLHazardTrackingModeDefault; }
- (MTLResourceOptions)resourceOptions { return MTLResourceStorageModeShared; }
- (NSUInteger)allocatedSize { return _backing ? [_backing allocatedSize] : 0u; }
- (RDNA4Heap *)rdna4ResourceHeap { return nil; }
- (NSUInteger)rdna4ResourceHeapOffset { return 0; }
- (BOOL)rdna4ResourceAliasable { return NO; }
- (void)rdna4ResourceMakeAliasable {}
- (BOOL)rdna4ResourceAliases:(id)other { (void)other; return NO; }
// Bridge 3 (Metal 3 Bindless): return backing GPU BDA for gpuResourceID.
RDNA4_RESOURCE_SPI_CUSTOM_UID(_backing ? [_backing gpuAddress] : rdna4_resource_uid(&_uid))

@end

// =================================================================================================
// Pipeline & Footnote 10 integration helpers (used by Wave 3B compute/render/mesh pipeline wiring)
// =================================================================================================
static _Atomic uint64_t rdna4_vft_owner_seq;

__attribute__((unused)) GARNET_INTERNAL uint64_t rdna4_vft_next_owner_token(void) {
    return atomic_fetch_add(&rdna4_vft_owner_seq, 1u) + 1u;
}

__attribute__((unused)) GARNET_INTERNAL BOOL rdna4_dynlib_vft_translator_ready(void) {
    return rdna4x_active() && rdna4x_stage2_symbols_available();
}

__attribute__((unused)) GARNET_INTERNAL id<MTLFunctionHandle> rdna4_vft_pipeline_handle(
    id pso, id dev, uint64_t ownerToken, const rdna4vft_reflection_t *refl,
    id<MTLFunction> function, NSUInteger stage)
{
    if (!pso || !dev || !ownerToken || !refl || !refl->present || !function) return nil;
    NSString *nm = [function respondsToSelector:@selector(name)] ? [function name] : nil;
    if (!nm.length) return nil;
    uint32_t hid = rdna4vft_refl_find_handle(refl, nm.UTF8String);
    if (hid == RDNA4VFT_HANDLE_NULL) return nil;
    MTLFunctionType ftype = [function respondsToSelector:@selector(functionType)]
        ? [function functionType] : MTLFunctionTypeVisible;
    return (id<MTLFunctionHandle>)[[RDNA4FunctionHandle alloc]
        initWithDevice:dev ownerPipeline:pso ownerToken:ownerToken stage:stage
          functionType:ftype name:nm handleId:hid];
}

__attribute__((unused)) GARNET_INTERNAL id<MTLVisibleFunctionTable> rdna4_vft_pipeline_new_table(
    id pso, id dev, uint64_t ownerToken, const rdna4vft_reflection_t *refl,
    MTLVisibleFunctionTableDescriptor *descriptor, NSUInteger stage)
{
    if (!pso || !dev || !ownerToken || !descriptor) return nil;
    NSUInteger count = descriptor.functionCount;
    uint32_t maxHid = (refl && refl->present) ? refl->max_handle_id : 0u;
    NSError *e = nil;
    RDNA4VisibleFunctionTable *tbl = [[RDNA4VisibleFunctionTable alloc]
        initWithDevice:dev ownerToken:ownerToken stage:stage functionCount:count maxHandleId:maxHid error:&e];
    if (!tbl && e) RDNA4LOG_ERR("newVisibleFunctionTableWithDescriptor failed: %s", e.localizedDescription.UTF8String);
    return (id<MTLVisibleFunctionTable>)tbl;
}

static BOOL rdna4_vft_linked_nonempty(id linked) {
    if (!linked) return NO;
    if (rdna4x_array_nonempty(linked, "functions") ||
        rdna4x_array_nonempty(linked, "privateFunctions") ||
        rdna4x_array_nonempty(linked, "binaryFunctions"))
        return YES;
    id gr = rdna4x_prop(linked, "groups");
    return [gr isKindOfClass:[NSDictionary class]] && [(NSDictionary *)gr count] > 0;
}

__attribute__((unused)) GARNET_INTERNAL BOOL rdna4_vft_validate_render_footnote10(
    id desc, BOOL isMeshDesc, BOOL hasVftMeta, BOOL hasRenderRT, NSError **error)
{
    uint32_t flags = 0u;
    if (isMeshDesc || rdna4x_prop(desc, "meshFunction") != nil) flags |= RDNA4FN10_HAS_MESH_STAGE;
    if (rdna4x_prop(desc, "objectFunction") != nil) flags |= RDNA4FN10_HAS_OBJECT_STAGE;
    if (rdna4_vft_linked_nonempty(rdna4x_prop(desc, "vertexLinkedFunctions"))) flags |= RDNA4FN10_HAS_VERTEX_FN_PTRS;
    if (rdna4_vft_linked_nonempty(rdna4x_prop(desc, "fragmentLinkedFunctions")) || hasVftMeta) flags |= RDNA4FN10_HAS_FRAGMENT_FN_PTRS;
    if (rdna4_vft_linked_nonempty(rdna4x_prop(desc, "objectLinkedFunctions"))) flags |= RDNA4FN10_HAS_OBJECT_FN_PTRS;
    if (rdna4_vft_linked_nonempty(rdna4x_prop(desc, "meshLinkedFunctions"))) flags |= RDNA4FN10_HAS_MESH_FN_PTRS;
    if (rdna4x_bool_prop(desc, "supportAddingVertexBinaryFunctions") ||
        rdna4x_bool_prop(desc, "supportAddingFragmentBinaryFunctions") ||
        rdna4x_bool_prop(desc, "supportAddingObjectBinaryFunctions") ||
        rdna4x_bool_prop(desc, "supportAddingMeshBinaryFunctions"))
        flags |= RDNA4FN10_SUPPORT_ADD_BIN_FNS;
    if (hasRenderRT) flags |= RDNA4FN10_HAS_RENDER_RAYTRACING;

    const char *why = NULL;
    rdna4vft_status_t st = rdna4vft_check_footnote10(flags, &why);
    if (st != RDNA4VFT_OK) {
        if (error) *error = rdna4_err(49, [NSString stringWithUTF8String:why ? why : rdna4vft_status_str(st)]);
        return NO;
    }
    return YES;
}

#endif // RDNA4_DYNLIB_VFT_IMPL_INC
#endif // !defined(GARNET_SHADER_REQUEST_CAPTURE_ONLY)
