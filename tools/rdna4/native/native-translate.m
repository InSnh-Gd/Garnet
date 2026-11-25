// Build from the repository root on macOS (development tools are build-time only):
// xcrun clang -arch x86_64 -fobjc-arc -Wall -Wextra -Werror -framework Foundation -lproc -I src/garnet-metal tools/rdna4/native/native-translate.m -o native-translate  (tools/rdna4/build-native-tools.sh)
// Runtime: Foundation, libSystem/libproc, packaged libgarnetxlate, OS GPUCompiler LLVM.
// No external validator is invoked; structural checks below are not spirv-val.
#import <Foundation/Foundation.h>
#include "garnet_xlate.h"
#include <libproc.h>
#include <spawn.h>
#include <sys/event.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <limits.h>

extern char **environ;
static volatile sig_atomic_t stopping;
static const char *libraryDefault = "/Library/GPUBundles/GarnetMetal.bundle/Contents/Resources/libgarnetxlate.dylib";
#define STATE ".native-translate"
#define WATCH_LIMIT 128
#define SCAN_BUDGET 512
#define SIDECAR_CAP (64u * 1024u)
#define REQUEST_INVALID 65
#define REQUEST_CANCELLED 66

static void stopService(int sig) { (void)sig; stopping = 1; }
static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
static void pauseUntil(double deadline) {
    while (!stopping) {
        double left = deadline - now();
        if (left <= 0) break;
        struct timespec t = { (time_t)left, (long)((left - (time_t)left) * 1e9) };
        if (nanosleep(&t, NULL) == 0 || errno != EINTR) break;
    }
}
static BOOL uidNumber(const char *s, uid_t *uid) {
    if (!s || !s[0] || (s[0] == '0' && s[1])) return NO;
    uint64_t n = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || n > (UINT32_MAX - (unsigned)(*s - '0')) / 10u) return NO;
        n = n * 10 + (unsigned)(*s - '0');
    }
    *uid = (uid_t)n;
    return YES;
}

// Walk every component without following links, including intermediate directories.
static int openDirectory(const char *path, mode_t createMode) {
    if (!path || path[0] != '/' || strlen(path) >= PATH_MAX) { errno = EINVAL; return -1; }
    char copy[PATH_MAX]; snprintf(copy, sizeof copy, "%s", path);
    int fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    char *save = NULL;
    for (char *part = strtok_r(copy, "/", &save); part && fd >= 0; part = strtok_r(NULL, "/", &save)) {
        if (!strcmp(part, ".") || !strcmp(part, "..")) { close(fd); errno = EINVAL; return -1; }
        BOOL made = NO;
        if (createMode && (!save || !save[0])) {
            if (mkdirat(fd, part, createMode) == 0) made = YES;
            else if (errno != EEXIST) { close(fd); return -1; }
        }
        int next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next >= 0 && made) {
            struct stat st;
            if (fstat(next, &st) != 0 || st.st_uid != geteuid() || fchmod(next, createMode) != 0) {
                close(next); close(fd); errno = EACCES; return -1;
            }
        }
        close(fd); fd = next;
    }
    return fd;
}
static BOOL ownedDirectory(int fd, uid_t owner) {
    struct stat st;
    return fd >= 0 && fstat(fd, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == owner && !(st.st_mode & 0022);
}
static BOOL privateDirectory(const char *name) {
    if (mkdir(name, 0700) != 0 && errno != EEXIST) return NO;
    int fd = open(name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    BOOL ok = ownedDirectory(fd, geteuid()) && fstat(fd, &st) == 0 && !(st.st_mode & 0077);
    if (fd >= 0) close(fd);
    return ok;
}
static BOOL sameFile(const struct stat *a, const struct stat *b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_uid == b->st_uid &&
        a->st_size == b->st_size && a->st_mtimespec.tv_sec == b->st_mtimespec.tv_sec &&
        a->st_mtimespec.tv_nsec == b->st_mtimespec.tv_nsec &&
        a->st_ctimespec.tv_sec == b->st_ctimespec.tv_sec && a->st_ctimespec.tv_nsec == b->st_ctimespec.tv_nsec;
}
static NSData *readFD(int fd, uid_t owner, size_t cap) {
    struct stat before, after;
    if (fstat(fd, &before) != 0 || !S_ISREG(before.st_mode) || before.st_uid != owner ||
        before.st_nlink != 1 || before.st_size < 1 || (uint64_t)before.st_size > cap) return nil;
    NSMutableData *data = [NSMutableData dataWithLength:(NSUInteger)before.st_size];
    size_t used = 0;
    while (used < data.length) {
        ssize_t n = pread(fd, (char *)data.mutableBytes + used, data.length - used, (off_t)used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return nil;
        used += (size_t)n;
    }
    if (fstat(fd, &after) != 0 || !sameFile(&before, &after)) return nil;
    return data;
}
static NSData *readAt(int dir, const char *name, uid_t owner, size_t cap) {
    int fd = openat(dir, name, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return nil;
    NSData *data = readFD(fd, owner, cap);
    close(fd);
    return data;
}
#include "native-ready.h"
static BOOL airIdentity(NSData *air, const char *expected) {
    if (!air || !rdna4x_air_size_ok(air.length)) return NO;
    const uint8_t *bytes = air.bytes;
    if (memcmp(bytes, "BC\xc0\xde", 4) && memcmp(bytes, "\xde\xc0\x17\x0b", 4)) return NO;
    uint8_t hash[32]; char sha[65];
    CC_SHA256(bytes, (CC_LONG)air.length, hash); rdna4x_hex(sha, hash, 32);
    return strcmp(sha, expected) == 0;
}
static NSString *shaderName(int dir, const char *sha, uid_t uid, size_t airBytes) {
    NSString *found = nil;
    const char *roles[] = { "fragment", "kernel", "vertex" };
    for (size_t i = 0; i < 3; i++) {
        char path[96]; snprintf(path, sizeof path, "%s.%s.json", sha, roles[i]);
        struct stat st;
        if (fstatat(dir, path, &st, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno == ENOENT) continue;
            return nil;
        }
        NSData *data = readAt(dir, path, uid, SIDECAR_CAP);
        id j = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:NULL] : nil;
        if (![j isKindOfClass:NSDictionary.class]) return nil;
        id name = j[@"function"], hash = j[@"sha256"], bytes = j[@"bytes"];
        if (![name isKindOfClass:NSString.class] || ![name length] || [name lengthOfBytesUsingEncoding:NSUTF8StringEncoding] > 1024 ||
            strlen([name UTF8String]) != [name lengthOfBytesUsingEncoding:NSUTF8StringEncoding] ||
            ![hash isEqual:[NSString stringWithUTF8String:sha]] || ![bytes isKindOfClass:NSNumber.class] ||
            [bytes unsignedLongLongValue] != airBytes || (found && ![found isEqual:name])) return nil;
        found = name;
    }
    if (found) return found;
    // Legacy dumps used the first whitespace-delimited token of <sha>.txt.
    char path[80]; snprintf(path, sizeof path, "%s.txt", sha);
    struct stat legacy;
    if (fstatat(dir, path, &legacy, AT_SYMLINK_NOFOLLOW) != 0)
        return errno == ENOENT ? @"" : nil; // No sidecar: engine's default local size.
    NSData *text = readAt(dir, path, uid, SIDECAR_CAP);
    NSString *s = text ? [[NSString alloc] initWithData:text encoding:NSUTF8StringEncoding] : nil;
    for (NSString *word in [s componentsSeparatedByCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet]) {
        if (word.length && [word lengthOfBytesUsingEncoding:NSUTF8StringEncoding] <= 1024 &&
            strlen(word.UTF8String) == [word lengthOfBytesUsingEncoding:NSUTF8StringEncoding]) return word;
    }
    return nil; // An invalid/mid-write sidecar is retried, never a translation verdict.
}

static BOOL libraryHash(const char *path, uint8_t hash[32]) {
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    struct stat before, after;
    if (fd < 0) return NO;
    BOOL ok = fstat(fd, &before) == 0 && S_ISREG(before.st_mode) && before.st_size > 0 &&
        before.st_size <= 512 * 1024 * 1024 && !(before.st_mode & 0022) &&
        (before.st_uid == 0 || before.st_uid == geteuid());
    CC_SHA256_CTX ctx; CC_SHA256_Init(&ctx);
    uint8_t buf[65536]; off_t used = 0;
    while (ok && used < before.st_size) {
        ssize_t n = pread(fd, buf, sizeof buf, used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = NO; break; }
        CC_SHA256_Update(&ctx, buf, (CC_LONG)n); used += n;
    }
    ok = ok && fstat(fd, &after) == 0 && sameFile(&before, &after);
    close(fd);
    if (ok) CC_SHA256_Final(hash, &ctx);
    return ok;
}
static BOOL translationKey(const char *library, char key[17]) {
    uint8_t libHash[32], readerID[16], overrideHash[32];
    if (!libraryHash(library, libHash)) return NO;
    rdna4x_reader reader; char why[256];
    if (rdna4x_reader_load(&reader, RDNA4X_GPUC_BASE, NULL, why, sizeof why) != 0) {
        fprintf(stderr, "%s\n", why); return NO;
    }
    Dl_info image = {0};
    BOOL ok = dladdr((const void *)reader.ParseBitcodeInContext2, &image) && image.dli_fbase &&
        rdna4x_macho_uuid(image.dli_fbase, 65536, readerID);
    dlclose(reader.h);
    char os[128] = {0}; size_t len = sizeof os;
    if (!ok || sysctlbyname("kern.osversion", os, &len, NULL, 0) != 0 || !os[0]) return NO;
    rdna4x_override_hash(overrideHash);
    // Include every dylib byte (also handles fat Mach-O), reader UUID, overrides and OS.
    char engineKey[17]; rdna4x_key(engineKey, libHash, readerID, overrideHash, os);
    CC_SHA256_CTX ctx; CC_SHA256_Init(&ctx);
    CC_SHA256_Update(&ctx, "rdna4-worker-1", 14);
    CC_SHA256_Update(&ctx, engineKey, 16); CC_SHA256_Update(&ctx, libHash, sizeof libHash);
    uint8_t digest[32]; CC_SHA256_Final(digest, &ctx); rdna4x_hex(key, digest, 8);
    return YES;
}
static void cachePath(char path[RDNA4X_PATH_MAX], const char *key) {
    snprintf(path, RDNA4X_PATH_MAX, STATE "/%s", key);
}
static void dropPublished(const char *sha) {
    char path[96];
    snprintf(path, sizeof path, "%s.spv", sha); (void)unlink(path);
    snprintf(path, sizeof path, "%s.meta.json", sha); (void)unlink(path);
    snprintf(path, sizeof path, STATE "/%s.key", sha); (void)unlink(path);
}
static BOOL usablePair(const char *dir, const char *sha, NSData **spvOut, NSData **metaOut) {
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return NO;
    char path[80]; snprintf(path, sizeof path, "%s.spv", sha);
    NSData *spv = readAt(fd, path, geteuid(), RDNA4X_MAX_SPV);
    snprintf(path, sizeof path, "%s.meta.json", sha);
    NSData *meta = readAt(fd, path, geteuid(), RDNA4X_MAX_META);
    close(fd);
    if (!spv || !meta || !rdna4x_spv_size_ok(spv.length) || !rdna4x_meta_size_ok(meta.length)) return NO;
    const uint32_t *w = spv.bytes;
    if (w[0] != 0x07230203 || !w[3] || w[4] != 0) return NO;
    size_t words = spv.length / 4;
    for (size_t p = 5; p < words;) {
        size_t count = w[p] >> 16;
        if (!count || count > words - p) return NO;
        p += count;
    }
    id j = [NSJSONSerialization JSONObjectWithData:meta options:0 error:NULL];
    if (![j isKindOfClass:NSDictionary.class] || ![j[@"entry_point"] isKindOfClass:NSString.class] ||
        ![j[@"entry_point"] length] || ![j[@"stage"] isKindOfClass:NSString.class] ||
        ![j[@"bindings"] isKindOfClass:NSArray.class]) return NO;
    if (spvOut) *spvOut = spv;
    if (metaOut) *metaOut = meta;
    return YES;
}
static BOOL publish(const char *dir, const char *sha, const char *key) {
    NSData *spv = nil, *meta = nil;
    if (!usablePair(dir, sha, &spv, &meta)) return NO;
    char receipt[128]; snprintf(receipt, sizeof receipt, STATE "/%s.key", sha);
    int root = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    NSData *old = root >= 0 ? readAt(root, receipt, geteuid(), 16) : nil;
    if (root >= 0) close(root);
    if (old.length == 16 && !memcmp(old.bytes, key, 16) && usablePair(".", sha, NULL, NULL)) return YES;
    // Withdraw the previous commit before changing its metadata; publish SPIR-V last.
    dropPublished(sha);
    char path[RDNA4X_PATH_MAX];
    rdna4x_file_path(path, sizeof path, ".", sha, RDNA4X_EXT_META);
    if (rdna4x_write_atomic(path, meta.bytes, meta.length) != 0) return NO;
    rdna4x_file_path(path, sizeof path, ".", sha, RDNA4X_EXT_SPV);
    if (rdna4x_write_atomic(path, spv.bytes, spv.length) != 0) return NO;
    return rdna4x_write_atomic(receipt, key, 16) == 0;
}

// Private re-exec mode: fd 3 is an already-open, owner-checked AIR inode.
static int request(int argc, char **argv) {
    uid_t uid;
    if (argc != 8 || !rdna4x_sha_ok(argv[2]) || !rdna4x_key_ok(argv[3]) || !uidNumber(argv[6], &uid)) return 64;
    int seconds = atoi(argv[7]);
    if (seconds < 1 || seconds > 120) return 64;
    signal(SIGALRM, SIG_DFL); signal(SIGXCPU, SIG_DFL);
    alarm((unsigned)seconds + 1);
    struct rlimit cpu = { (rlim_t)seconds + 1, (rlim_t)seconds + 1 }, core = {0, 0};
    if (setrlimit(RLIMIT_CPU, &cpu) != 0 || setrlimit(RLIMIT_CORE, &core) != 0) return RDNA4X_E_IO;
    NSData *air = readFD(3, uid, RDNA4X_MAX_AIR); close(3);
    if (!airIdentity(air, argv[2])) return REQUEST_INVALID;
    const char *sha = argv[2];
    char actualKey[17];
    if (!translationKey(argv[5], actualKey) || strcmp(actualKey, argv[3])) return RDNA4X_E_UNAVAILABLE;
    char dir[RDNA4X_PATH_MAX]; cachePath(dir, argv[3]);
    if (!privateDirectory(STATE) || !privateDirectory(dir)) return RDNA4X_E_IO;
    rdna4x_engine engine;
    rdna4x_engine_init(&engine, NULL, RDNA4X_GPUC_BASE, NULL, argv[5], RDNA4X_WORKER_STACK);
    char why[512] = {0};
    int rc = rdna4x_engine_run(&engine, dir, sha, argv[4], air.bytes, air.length, seconds * 1000, why, sizeof why);
    if (rc == RDNA4X_OK && !usablePair(dir, sha, NULL, NULL)) rc = RDNA4X_E_REFUSED;
    if (rc != RDNA4X_OK) fprintf(stderr, "%s rc=%d %s\n", sha, rc, why);
    // The engine's detached thread must not outlive the request's stack or AIR data.
    fflush(NULL); _exit(rc);
}

static int runRequest(const char *executable, const char *library, int airFD, const char *sha,
                      const char *key, NSString *name, uid_t uid, int seconds) {
    char uidText[16], timeout[16];
    snprintf(uidText, sizeof uidText, "%u", uid); snprintf(timeout, sizeof timeout, "%d", seconds);
    char *args[] = { (char *)executable, "--request", (char *)sha, (char *)key,
        (char *)name.UTF8String, (char *)library, uidText, timeout, NULL };
    posix_spawn_file_actions_t actions;
    int rc = posix_spawn_file_actions_init(&actions);
    if (rc) return RDNA4X_E_IO;
    rc = posix_spawn_file_actions_adddup2(&actions, airFD, 3);
    pid_t pid = -1;
    if (!rc) rc = posix_spawn(&pid, executable, &actions, NULL, args, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (rc) return RDNA4X_E_IO;
    double deadline = now() + seconds;
    int status = 0; BOOL killed = NO;
    for (;;) {
        pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid) break;
        if (result < 0 && errno == EINTR) continue;
        if (result < 0) return RDNA4X_E_IO;
        struct proc_taskinfo task;
        BOOL memory = proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &task, sizeof task) == (int)sizeof task &&
            task.pti_resident_size > 1024ull * 1024ull * 1024ull;
        if (stopping || now() >= deadline || memory) {
            (void)kill(pid, SIGKILL); killed = YES;
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            break;
        }
        pauseUntil(now() + 0.05);
    }
    if (stopping) return REQUEST_CANCELLED;
    if (killed || !WIFEXITED(status)) return RDNA4X_E_TIMEOUT;
    return WEXITSTATUS(status);
}

typedef struct {
    DIR *root, *sub;
    uid_t uid;
    int kq, watches[WATCH_LIMIT]; size_t nw;
} Scan;
static void watchDirectory(Scan *s, int fd) {
    if (s->nw == WATCH_LIMIT || s->kq < 0) return;
    int copy = fcntl(fd, F_DUPFD_CLOEXEC, 10);
    if (copy < 0) return;
    struct kevent event;
    EV_SET(&event, copy, EVFILT_VNODE, EV_ADD | EV_CLEAR, NOTE_WRITE | NOTE_RENAME | NOTE_DELETE | NOTE_ATTRIB, 0, NULL);
    if (kevent(s->kq, &event, 1, NULL, 0, NULL) == 0) s->watches[s->nw++] = copy;
    else close(copy);
}
static BOOL beginScan(Scan *s, const char *source) {
    for (size_t i = 0; i < s->nw; i++) close(s->watches[i]);
    s->nw = 0;
    int fd = openDirectory(source, 01777);
    struct stat st;
    if (fd < 0) return NO;
    if (fstat(fd, &st) != 0 || !rdna4g_dumproot_ok(0, S_ISDIR(st.st_mode), 0, st.st_uid, st.st_mode, geteuid())) {
        close(fd); fprintf(stderr, "source must be an owned/root/nobody sticky directory with mode 1777\n"); return NO;
    }
    watchDirectory(s, fd);
    s->root = fdopendir(fd);
    if (!s->root) close(fd);
    return s->root != NULL;
}
static BOOL scanSlice(Scan *s, const char *executable, const char *library, const char *key, int seconds, int *errors) {
    unsigned budget = SCAN_BUDGET;
    size_t bytesRead = 0;
    while (budget-- && bytesRead < 16u * 1024u * 1024u && !stopping) {
      @autoreleasepool {
        if (!s->sub) {
            struct dirent *e = readdir(s->root);
            if (!e) { closedir(s->root); s->root = NULL; return NO; }
            if (!uidNumber(e->d_name, &s->uid)) continue;
            int fd = openat(dirfd(s->root), e->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            struct stat st;
            if (fd < 0) continue;
            if (fstat(fd, &st) != 0 || !rdna4g_dumpdir_ok(0, S_ISDIR(st.st_mode), 0, st.st_uid, s->uid) || (st.st_mode & 0022)) {
                close(fd); continue;
            }
            watchDirectory(s, fd);
            s->sub = fdopendir(fd);
            if (!s->sub) close(fd);
            continue;
        }
        struct dirent *e = readdir(s->sub);
        if (!e) { closedir(s->sub); s->sub = NULL; continue; }
        if (strlen(e->d_name) != 68 || strcmp(e->d_name + 64, ".air")) continue;
        char sha[65]; memcpy(sha, e->d_name, 64); sha[64] = 0;
        if (!rdna4x_sha_ok(sha)) continue;
        int fd = openat(dirfd(s->sub), e->d_name, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
        struct stat st;
        if (fd < 0) continue;
        if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != s->uid || st.st_nlink != 1) { close(fd); continue; }
        NSData *air = readFD(fd, s->uid, RDNA4X_MAX_AIR);
        if (air) bytesRead += air.length;
        if (!airIdentity(air, sha)) {
            // Invalid bytes cannot poison the shared SHA's translator decisions.
            close(fd); (*errors)++; continue;
        }
        char dir[RDNA4X_PATH_MAX]; cachePath(dir, key);
        int plan = rdna4x_probe(dir, sha);
        if (plan != RDNA4X_PL_TRANSLATE) {
            if (plan != RDNA4X_PL_SKIP_LIVE) dropPublished(sha);
            close(fd); continue;
        }
        if (usablePair(dir, sha, NULL, NULL)) {
            if (!publish(dir, sha, key)) (*errors)++;
            close(fd); continue;
        }
        NSString *name = shaderName(dirfd(s->sub), sha, s->uid, air.length);
        if (!name) { close(fd); (*errors)++; continue; }
        int rc = runRequest(executable, library, fd, sha, key, name, s->uid, seconds);
        close(fd);
        if (rc == RDNA4X_OK && !publish(dir, sha, key)) rc = RDNA4X_E_IO;
        if (rc == RDNA4X_OK) {
            printf("OK %s uid=%u key=%s\n", sha, s->uid, key);
        } else {
            if (rc != REQUEST_INVALID && rc != REQUEST_CANCELLED) dropPublished(sha);
            char marker[RDNA4X_PATH_MAX];
            const char *ext = rc == RDNA4X_E_TIMEOUT ? RDNA4X_EXT_CRASH : RDNA4X_EXT_FAIL;
            rdna4x_file_path(marker, sizeof marker, dir, sha, ext);
            char why[96]; int n = snprintf(why, sizeof why, "native worker rc=%d\n", rc);
            if (rc != RDNA4X_E_IO && rc != RDNA4X_E_SKIP_LIVE && rc != REQUEST_INVALID && rc != REQUEST_CANCELLED)
                (void)rdna4x_write_atomic(marker, why, (size_t)n);
            rdna4x_marker_remove(dir, sha);
            fprintf(stderr, "FAIL %s uid=%u key=%s rc=%d\n", sha, s->uid, key, rc);
            (*errors)++;
        }
        fflush(NULL);
        return YES; // At most one subprocess and 512 directory entries per slice.
      }
    }
    return YES;
}
static void usage(void) {
    puts("Usage: native-translate [--once] [--source ABS] [--output ABS] [--library ABS] [--timeout 1..120]\n"
         "       native-translate --wait-ready 1..120 [--output ABS]\n"
         "Defaults: /private/tmp/rdna4m -> /private/var/tmp/rdna4m-spv; timeout 20 seconds.\n"
         "Library: /Library/GPUBundles/GarnetMetal.bundle/Contents/Resources/libgarnetxlate.dylib\n"
         "Input: <source>/<uid>/<64 lowercase SHA256>.air plus .vertex/.fragment/.kernel.json\n"
         "with function, sha256, bytes; legacy .txt first token is supported.\n"
         "Absent sidecars use the translator's default local size (no named override).\n"
         "Source mode 1777; numeric subdir and regular files must belong to that uid.\n"
         "Output must belong to this service uid, mode 0755; private decisions in .native-translate/<key>.\n"
         "Publishes <sha>.meta.json then <sha>.spv, mode 0644, using atomic rename.\n"
         "--once scans to EOF (0: no new failures, 1: request/setup failure, 64: usage).\n"
         "Service uses vnode notifications, 250ms pacing, and a 5s fallback scan.\n"
         "--wait-ready waits for a live initialized nobody service using a local socket.\n"
         "No GPU access or external SPIR-V validator; translation is not a GPU validation claim.");
}
int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc > 1 && !strcmp(argv[1], "--request")) return request(argc, argv);
        const char *source = "/private/tmp/rdna4m", *output = "/private/var/tmp/rdna4m-spv", *library = libraryDefault;
        BOOL once = NO; int seconds = 20, waitSeconds = 0;
        for (int i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "--help")) { usage(); return 0; }
            if (!strcmp(argv[i], "--once")) { once = YES; continue; }
            if (i + 1 == argc) { usage(); return 64; }
            const char *option = argv[i], *value = argv[++i];
            if (!strcmp(option, "--source")) source = value;
            else if (!strcmp(option, "--output")) output = value;
            else if (!strcmp(option, "--library")) library = value;
            else if (!strcmp(option, "--timeout") || !strcmp(option, "--wait-ready")) {
                uid_t n;
                if (!uidNumber(value, &n) || n < 1 || n > 120) return 64;
                if (!strcmp(option, "--timeout")) seconds = (int)n;
                else waitSeconds = (int)n;
            } else { usage(); return 64; }
        }
        if (source[0] != '/' || output[0] != '/' || library[0] != '/' || rdna4x_env_dirty(environ)) {
            fprintf(stderr, "absolute paths and an environment without METAL2VULKAN_* are required\n"); return 64;
        }
        if (waitSeconds) return once ? 64 : waitReady(output, (unsigned)waitSeconds);
        char executable[PATH_MAX]; uint32_t size = sizeof executable;
        if (_NSGetExecutablePath(executable, &size) != 0) return 1;
        char absolute[PATH_MAX];
        if (!realpath(executable, absolute)) return 1;
        int out = openDirectory(output, 0755);
        if (!ownedDirectory(out, geteuid()) || fchmod(out, 0755) != 0 || fchdir(out) != 0) {
            if (out >= 0) close(out);
            fprintf(stderr, "output must be a real directory owned by the service uid\n"); return 1;
        }
        close(out);
        if (!privateDirectory(STATE)) {
            fprintf(stderr, "private state directory %s/%s must be mode 0700 owned by service uid\n", output, STATE);
            return 1;
        }
        int lock = open(STATE "/service.lock", O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
        struct stat st;
        if (lock < 0 || fstat(lock, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
            flock(lock, LOCK_EX | LOCK_NB) != 0) { if (lock >= 0) close(lock); return 1; }
        signal(SIGTERM, stopService); signal(SIGINT, stopService);
        Scan scan = { .kq = kqueue() }; int errors = 0;
        if (scan.kq >= 0) (void)fcntl(scan.kq, F_SETFD, FD_CLOEXEC);
        int listener = once ? -1 : readyListener(scan.kq);
        if (!once && listener < 0) { perror("readiness socket"); close(lock); return 1; }
        char key[17] = {0}; double nextKey = 0; BOOL ready = NO, rescan = NO;
        do {
            @autoreleasepool {
                if (now() >= nextKey || !key[0]) {
                    if (!translationKey(library, key)) { fprintf(stderr, "cannot identify translator/OS reader\n"); return 1; }
                    char dir[RDNA4X_PATH_MAX]; cachePath(dir, key);
                    if (!privateDirectory(dir)) return 1;
                    nextKey = now() + 5;
                }
                if (!scan.root) {
                    if (beginScan(&scan, source)) rescan = NO;
                    else if (once) { fprintf(stderr, "cannot scan source: %s\n", source); errors++; break; }
                }
                BOOL scanning = scan.root != NULL;
                if (scanning && listener >= 0) readyReply(listener);
                BOOL more = scanning && scanSlice(&scan, absolute, library, key, seconds, &errors);
                if (!ready && scanning && !more) {
                    printf("READY uid=%u key=%s source=%s output=%s\n", geteuid(), key, source, output);
                    fflush(stdout);
                    ready = YES;
                }
                if (once) { if (!more) break; continue; }
                // A notification may refer to a UID already passed by this scan.
                BOOL pace = more || rescan;
                const double paced = now() + 0.25;
                if (scan.kq >= 0) {
                    struct timespec timeout = { pace ? 0 : 5, pace ? 250000000 : 0 };
                    struct kevent events[16];
                    int count = kevent(scan.kq, NULL, 0, events, 16, &timeout);
                    for (int i = 0; i < count; i++)
                        if (events[i].filter == EVFILT_VNODE) rescan = YES;
                }
                pauseUntil(pace || rescan ? paced : now() + 0.25);
            }
        } while (!stopping);
        if (scan.sub) closedir(scan.sub);
        if (scan.root) closedir(scan.root);
        for (size_t i = 0; i < scan.nw; i++) close(scan.watches[i]);
        if (scan.kq >= 0) close(scan.kq);
        if (listener >= 0) { close(listener); (void)unlink(N44_READY_SOCKET); }
        close(lock);
        return once && errors ? 1 : 0;
    }
}

