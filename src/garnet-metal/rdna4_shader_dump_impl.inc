// ---------------------------------------------------------------------------------------------------------------
// AIR dump for the pipeline-creation overrides (NATIVE-S4-M10 "Shaders": cache miss -> /tmp/rdna4m/<sha256>.air).
// ---------------------------------------------------------------------------------------------------------------
static NSString *rdna4_sha256hex(NSData *d) {
    unsigned char h[CC_SHA256_DIGEST_LENGTH]; CC_SHA256(d.bytes, (CC_LONG)d.length, h);
    NSMutableString *s = [NSMutableString string];
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) [s appendFormat:@"%02x", h[i]];
    return s;
}

// RDNA4_ONCE: one log line per call site per process (no-op stubs are called thousands of times a second by the compositor; moved up in build 6 for the dump helpers)
#define RDNA4_ONCE(fmt, ...) do { static _Atomic int once_; if (!atomic_exchange(&once_, 1)) RDNA4LOG(fmt, ##__VA_ARGS__); } while (0)
// Bundle 9 (app crash study item 1): runs when Metal loads this bundle.  Core Image reflects a CIKL string kernel through [device newLibraryWithSource:] and asks the stitchable function
// for its arguments; that goes to -[_MTLDevice compiler] (nil here), the argument list stays empty and CIKernelReflection::consolidate faults (Preview, Adjust Color).  With the variable
// at 0 Core Image uses its own kernel-language parser instead.  An existing value (even "1") is never overwritten.  A process that calls kernelWithString before any Metal device exists
// has already read the flag: the arm script's `launchctl setenv` is the backstop for that.
__attribute__((constructor)) static void rdna4_cienv_ctor(void) {
    int r = rdna4ce_apply((rdna4ce_getenv_fn)getenv, setenv);
    if (r == RDNA4CE_SET) RDNA4_ONCE("cienv: set " RDNA4CE_VAR "=" RDNA4CE_VALUE " (Core Image takes its kernel-language parser, not the nil [device compiler])");
    else if (r == RDNA4CE_ALREADY_SET) RDNA4_ONCE("cienv: " RDNA4CE_VAR " is already set in this process (=%s); left alone", getenv(RDNA4CE_VAR));
    else RDNA4_ONCE("cienv: setenv " RDNA4CE_VAR " failed (errno %d)", errno);
}
// 0.0.641 (G4 review LOW) kept, bundle build 6 (C1 finding A): the dump area is /tmp/rdna4m (sticky 01777 parent, rdna4g_dumproot_ok) and every uid dumps into ITS OWN subdirectory /tmp/rdna4m/<euid> (0700, a
// real directory owned by the caller, rdna4g_dumpdir_ok). No chmod ever follows a symlink; any failed check skips the dump (and says so once per reason in the log). The translate daemon's
// account is granted list+search on the subdirectory by an ACL entry on the open directory descriptor (rdna4_dumpacl.h); a failed grant is logged and the dump still happens.
static int rdna4_dump_dir_ready(char *sub, size_t subn) {
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
static int rdna4_dump_write(const char *path, NSData *data) {
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
static void rdna4_dump_function(id fn, const char *role, NSMutableArray *paths, NSMutableArray *notes) {
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

static NSError *rdna4_dump_pipeline(MTLRenderPipelineDescriptor *d, BOOL vertexMissing, BOOL fragmentMissing) {
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


