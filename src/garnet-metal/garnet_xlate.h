// garnet_xlate.h - bundle 14: in-process shader translation for sandboxed applications (an internal design note, sections 1.1-1.7).
// Pure C: no Metal, no Foundation, no IOKit. Host test test-xlate.c compiles this very header and drives the very functions GarnetDevice.m calls; test-xlate-corpus.sh drives the real
// reader and translator through the real engine over the shader corpus.
//
// What it does. A pipeline whose SPIR-V is in neither the bundle's spvcache nor the cache directories is, in an ADMITTED APPLICATION (or an RDNA4M_ALLOW root tool with RDNA4M_TEST_INPROC=1), translated INSIDE
// the process: the AIR bitcode is turned into LLVM text by the operating system's own LLVM (GPUCompiler.framework's libLLVM, loaded with dlopen on the first miss only, ten functions), the text goes to
// Resources/libgarnetxlate.dylib (the LGPL translator, a C ABI), and the SPIR-V plus its trimmed meta are installed in the user's cache directory. WindowServer never applies: it keeps the precompiled
// spvcache plus the daemon. Nothing here starts a process, and nothing is written outside the cache directory.
//
// Safety shape (section 1.6/1.7): a gate (rdna4x_applies), a crash marker per translation (<sha>.inflight, O_EXCL, holding the pid; a leftover marker whose process is dead becomes <sha>.crash and that sha is
// never translated again under this key), a negative cache (<sha>.fail), size caps, one worker thread on a 64 MiB stack, jobs one at a time, and every failure falling back to today's behaviour.
#ifndef RDNA4_XLATE_H
#define RDNA4_XLATE_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <mach-o/loader.h>
#include <mach-o/dyld.h>
#include <CommonCrypto/CommonDigest.h>
#include "garnet_gate.h"
#include "garnet_query_caps.h"

// ---- constants ----
#define RDNA4X_KEY_TAG       "rdna4x-54-gpu-icb-render-lowering"
#define RDNA4X_ABI_VERSION   3u   // capability-aware exports; ABI 2 signatures remain unchanged
#define RDNA4X_MAX_DEPS      64u   // linked dependencies one pipeline stage may carry (more = no linked translation)
#define RDNA4X_MAX_DEP_AIR   (8u * 1024u * 1024u)   // total bitcode of all dependencies of one stage
#define RDNA4X_MAX_AIR       (4u * 1024u * 1024u)        // bitcode: the daemon's cap
#define RDNA4X_MAX_LL        (64u * 1024u * 1024u)       // LLVM text
#define RDNA4X_MAX_SPV       (16u * 1024u * 1024u)       // SPIR-V
#define RDNA4X_MAX_META      (8u * 1024u * 1024u)        // trimmed meta JSON
#define RDNA4X_CACHE_CAP     (256ull * 1024ull * 1024ull)   // per application; trimmed to 80% when exceeded
#define RDNA4X_WORKER_STACK  ((size_t)64u * 1024u * 1024u)   // a default secondary thread has 512 KiB; the command-line tool's 8 MiB main stack is the only depth anyone has measured
#define RDNA4X_KILL_FILE     "/private/tmp/rdna4m-noinproc"
#define RDNA4X_CACHE_SUB     "org.garnet.xlate"
#define RDNA4X_ENV_PREFIX    "METAL2VULKAN_"
#define RDNA4X_LIB_NAME      "libgarnetxlate.dylib"
#define RDNA4X_GPUC_BASE     "/System/Library/PrivateFrameworks/GPUCompiler.framework/Versions"
#define RDNA4X_PATH_MAX      1024

// ---- result codes of one job ----
#define RDNA4X_OK            0
#define RDNA4X_E_CAP         1    // bitcode empty / too large, LLVM text / SPIR-V / meta over its cap
#define RDNA4X_E_SKIP_CRASH  2    // <sha>.crash exists (or a dead process left <sha>.inflight): never translated again under this key
#define RDNA4X_E_SKIP_FAIL   3    // <sha>.fail exists: refused before, retried only when the key changes
#define RDNA4X_E_SKIP_LIVE   4    // <sha>.inflight belongs to a LIVE process (another instance is translating it right now)
#define RDNA4X_E_READER      5    // the bitcode reader refused the module
#define RDNA4X_E_REFUSED     6    // the translator refused (code 1) or panicked (code 2) or rejected its arguments (code 3)
#define RDNA4X_E_IO          7    // could not create the marker / write the result
#define RDNA4X_E_UNAVAILABLE 8    // the reader or the translator library could not be loaded
#define RDNA4X_E_TIMEOUT     9    // the caller stopped waiting; the worker finishes and installs the result

// ======================================================================================================================================================================
// 1. The gate (section 1.6)
// ======================================================================================================================================================================
// isWs        WindowServer: NEVER (it keeps the precompiled spvcache plus the daemon and never loads either library).
// admitted    the kernel admitted this (class OTHER) process as an allow-listed application.
// rootAllow   an RDNA4M_ALLOW=1 root tool; it applies only with RDNA4M_TEST_INPROC=1 (this retires the add-air.py seeding root probes needed).
// killed      /private/tmp/rdna4m-noinproc exists or could not be read (fail closed), read once per process.
// envDirty    a METAL2VULKAN_* variable is present: several change the translator's output (RELOOPER_MAX_BLOCKS, WHOLE_PART, TIR_ONLY, ...), and the cache is keyed without them.
static inline int rdna4x_applies(int isWs, int admitted, int rootAllow, int testInproc, int killed, int envDirty) {
    if (isWs) return 0;
    if (killed) return 0;
    if (envDirty) return 0;
    if (rootAllow) return testInproc ? 1 : 0;
    return admitted ? 1 : 0;
}
// 1 when any variable in envp (NULL-terminated, like environ) starts with METAL2VULKAN_.
static inline int rdna4x_env_dirty(char *const *envp) {
    if (!envp) return 0;
    for (; *envp; envp++) if (strncmp(*envp, RDNA4X_ENV_PREFIX, sizeof RDNA4X_ENV_PREFIX - 1) == 0) return 1;
    return 0;
}
// The kill file, with garnet_gate.h's states: anything but a clean "absent" is killed (fail closed).
static inline int rdna4x_killed_from_stat(int statRc, int err) { return rdna4g_kill_state(statRc, err) != RDNA4G_KILL_ABSENT; }
// The side directory /private/var/tmp/rdna4m-spv is read by WindowServer's sandbox profile and by root tools; a sandboxed application is denied it (deny-log spam), so it is skipped there.
static inline int rdna4x_skip_side_dir(int isWs, int sandboxed) { return !isWs && sandboxed; }

// ======================================================================================================================================================================
// 2. The --local override table (matches autotranslate/overrides.txt; test-xlate.c compares them)
// ======================================================================================================================================================================
typedef struct { const char *name; uint32_t xyz[3]; } rdna4x_override;
static const rdna4x_override rdna4x_overrides[] = {
    { "sum_rgba_columns", { 32, 1, 1 } },
    { "sum_rgba_rows",    { 32, 1, 1 } },
    { "write_tex",        {  8, 8, 1 } },
};
#define RDNA4X_NOVERRIDES (sizeof rdna4x_overrides / sizeof rdna4x_overrides[0])
// The nominal threadgroup size for the kernel named `name`, or NULL (the translator's default 64,1,1).
static inline const uint32_t *rdna4x_local_for(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < RDNA4X_NOVERRIDES; i++) if (strcmp(rdna4x_overrides[i].name, name) == 0) return rdna4x_overrides[i].xyz;
    return NULL;
}

// ======================================================================================================================================================================
// 3. The cache key and the paths (section 1.4)
// ======================================================================================================================================================================
static inline void rdna4x_hex(char *out, const uint8_t *p, size_t n) {
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[2 * i] = d[p[i] >> 4]; out[2 * i + 1] = d[p[i] & 15]; }
    out[2 * n] = 0;
}
// sha256 over the override table as the lines "name=x,y,z\n" in table order: a change to the table is a new key.
static inline void rdna4x_override_hash(uint8_t out[CC_SHA256_DIGEST_LENGTH]) {
    CC_SHA256_CTX c; CC_SHA256_Init(&c);
    for (size_t i = 0; i < RDNA4X_NOVERRIDES; i++) {
        char l[160]; const int n = snprintf(l, sizeof l, "%s=%u,%u,%u\n", rdna4x_overrides[i].name, rdna4x_overrides[i].xyz[0], rdna4x_overrides[i].xyz[1], rdna4x_overrides[i].xyz[2]);
        CC_SHA256_Update(&c, l, (CC_LONG)n);
    }
    CC_SHA256_Final(out, &c);
}
// K includes the binding/addressing generation, translator UUID, AIR reader identity,
// override table and kern.osversion. BDA64 layouts must not reuse earlier entries.
// readerId is the reader's LC_UUID when it is known from a loaded image, or (a reader in the dyld shared cache has no file and is not loaded at lookup time) the shared cache's UUID, which pins
// every library in it, GPUCompiler's included. out needs 17 bytes.
static inline void rdna4x_capabilities_hash(uint8_t out[CC_SHA256_DIGEST_LENGTH], const rdna4x_backend_capabilities *caps) {
    const rdna4x_backend_capabilities disabled = rdna4x_query_caps(0, 0, 0, 0, 0);
    const rdna4x_backend_capabilities *c = caps ? caps : &disabled;
    const uint32_t fields[] = { RDNA4X_QUERY_POLICY_REVISION, c->struct_size, c->version,
        c->flags, c->subgroup_size, c->subgroup_supported_stages, c->subgroup_supported_operations };
    CC_SHA256(fields, sizeof fields, out);
}
static inline void rdna4x_key_with_capabilities(char out[17], const uint8_t xlUuid[16], const uint8_t readerId[16], const uint8_t ovHash[32], const char *osversion, const rdna4x_backend_capabilities *caps) {
    CC_SHA256_CTX c; CC_SHA256_Init(&c);
    CC_SHA256_Update(&c, RDNA4X_KEY_TAG, (CC_LONG)(sizeof RDNA4X_KEY_TAG));   // includes the NUL: a fixed delimiter
    uint8_t capHash[CC_SHA256_DIGEST_LENGTH]; rdna4x_capabilities_hash(capHash, caps);
    CC_SHA256_Update(&c, capHash, sizeof capHash);
    CC_SHA256_Update(&c, xlUuid, 16);
    CC_SHA256_Update(&c, readerId, 16);
    CC_SHA256_Update(&c, ovHash, 32);
    const char *os = osversion ? osversion : "";
    CC_SHA256_Update(&c, os, (CC_LONG)strlen(os));
    uint8_t d[CC_SHA256_DIGEST_LENGTH]; CC_SHA256_Final(d, &c);
    rdna4x_hex(out, d, 8);
}
static inline void rdna4x_key(char out[17], const uint8_t xlUuid[16], const uint8_t readerId[16], const uint8_t ovHash[32], const char *osversion) {
    rdna4x_key_with_capabilities(out, xlUuid, readerId, ovHash, osversion, NULL);
}
static inline int rdna4x_sha_ok(const char *s) {
    if (!s) return 0;
    for (int i = 0; i < 64; i++) { const char c = s[i]; if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0; }
    return s[64] == 0;
}
static inline int rdna4x_key_ok(const char *k) {
    if (!k) return 0;
    for (int i = 0; i < 16; i++) { const char c = k[i]; if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0; }
    return k[16] == 0;
}
// ---- build 18 (P3): the cache key of a LINKED translation ----
// The same entry function linked to different functions is a different program (the dependency decides the output: garnetxlate/tests/linked.rs), so the key is
//   sha256( "rdna4x-link-1\n" + entrySha(64 hex) + "\n" + for each dependency, sorted by (symbol, sha) with exact duplicates dropped: symbol + "\0" + sha(64 hex) + "\n" ).
// The order the client listed the functions in, and a function listed twice, do not change the key; a different function, a different name, an added or a removed dependency do. 0 = ok, else out is empty.
typedef struct { const char *sym; const char *sha; } rdna4x_lkpair;
static inline int rdna4x_link_cmp(const void *a, const void *b) {
    const rdna4x_lkpair *x = (const rdna4x_lkpair *)a, *y = (const rdna4x_lkpair *)b;
    const int c = strcmp(x->sym, y->sym);
    return c ? c : strcmp(x->sha, y->sha);
}
static inline int rdna4x_link_sha(char out[65], const char *entrySha, size_t n, const char *const *syms, const char *const *shas) {
    out[0] = 0;
    if (!rdna4x_sha_ok(entrySha) || n > 4096) return -1;
    rdna4x_lkpair *v = (rdna4x_lkpair *)calloc(n ? n : 1, sizeof *v);
    if (!v) return -1;
    for (size_t i = 0; i < n; i++) { if (!syms[i] || !syms[i][0] || !rdna4x_sha_ok(shas[i])) { free(v); return -1; } v[i].sym = syms[i]; v[i].sha = shas[i]; }
    qsort(v, n, sizeof *v, rdna4x_link_cmp);
    CC_SHA256_CTX c; CC_SHA256_Init(&c);
    CC_SHA256_Update(&c, "rdna4x-link-1\n", 12);
    CC_SHA256_Update(&c, entrySha, 64); CC_SHA256_Update(&c, "\n", 1);
    for (size_t i = 0; i < n; i++) {
        if (i && !rdna4x_link_cmp(&v[i], &v[i - 1])) continue;
        CC_SHA256_Update(&c, v[i].sym, (CC_LONG)strlen(v[i].sym)); CC_SHA256_Update(&c, "\0", 1); CC_SHA256_Update(&c, v[i].sha, 64); CC_SHA256_Update(&c, "\n", 1);
    }
    uint8_t d[CC_SHA256_DIGEST_LENGTH]; CC_SHA256_Final(d, &c);
    rdna4x_hex(out, d, 32);
    free(v);
    return 0;
}
// <userCacheDir>/org.garnet.xlate  (userCacheDir is confstr(_CS_DARWIN_USER_CACHE_DIR), which ends with '/')
static inline size_t rdna4x_parent_path(char *out, size_t n, const char *userCacheDir) {
    if (!out || n == 0) return 0;
    out[0] = 0;
    if (!userCacheDir || !userCacheDir[0]) return 0;
    const size_t dl = strlen(userCacheDir);
    const int w = snprintf(out, n, "%s%s" RDNA4X_CACHE_SUB, userCacheDir, userCacheDir[dl - 1] == '/' ? "" : "/");
    if (w <= 0 || (size_t)w >= n) { out[0] = 0; return 0; }
    return (size_t)w;
}
// <userCacheDir>/org.garnet.xlate/<K>
static inline size_t rdna4x_dir_path(char *out, size_t n, const char *userCacheDir, const char *key) {
    if (!out || n == 0) return 0;
    out[0] = 0;
    if (!rdna4x_key_ok(key)) return 0;
    char p[RDNA4X_PATH_MAX];
    if (!rdna4x_parent_path(p, sizeof p, userCacheDir)) return 0;
    const int w = snprintf(out, n, "%s/%s", p, key);
    if (w <= 0 || (size_t)w >= n) { out[0] = 0; return 0; }
    return (size_t)w;
}
// <dir>/<sha><ext>  with ext one of .spv .meta.json .fail .inflight .crash
#define RDNA4X_EXT_SPV      ".spv"
#define RDNA4X_EXT_META     ".meta.json"
#define RDNA4X_EXT_FAIL     ".fail"
#define RDNA4X_EXT_INFLIGHT ".inflight"
#define RDNA4X_EXT_CRASH    ".crash"
static inline size_t rdna4x_file_path(char *out, size_t n, const char *dir, const char *sha, const char *ext) {
    if (!out || n == 0) return 0;
    out[0] = 0;
    if (!dir || !rdna4x_sha_ok(sha) || !ext) return 0;
    const int w = snprintf(out, n, "%s/%s%s", dir, sha, ext);
    if (w <= 0 || (size_t)w >= n) { out[0] = 0; return 0; }
    return (size_t)w;
}

// ---- the cache directory: created 0700, lstat-checked (a real directory owned by the caller, never a symlink), old keys removed (section 1.4) ----
// mkdir (EEXIST is fine), then lstat: only a real directory owned by euid passes. 0 = ok.
static inline int rdna4x_mkdir_checked(const char *path) {
    if (mkdir(path, 0700) != 0 && errno != EEXIST) return -1;
    struct stat st;
    if (lstat(path, &st) != 0 || !S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode) || st.st_uid != geteuid()) return -1;
    return 0;
}
// mkdir -p for the cache path: a sandboxed application's container cache directory
// (<user cache>/com.apple.<service>) is NOT created by the system up front, so
// creating <container>/org.garnet.xlate failed with ENOENT and every translation of
// that process was refused. Each missing ancestor is created 0700 and then checked
// like the final directory (a real directory of ours). Existing ancestors are
// followed through symlinks (/var -> /private/var, /tmp -> /private/tmp); the LAST
// component, the directory we then write into, is never a symlink.
static inline int rdna4x_mkdir_p(const char *path) {
    if (!path || path[0] != '/') return -1;
    char buf[RDNA4X_PATH_MAX];
    const size_t n = strlen(path);
    if (n == 0 || n >= sizeof buf) return -1;
    memcpy(buf, path, n + 1);
    for (size_t i = 1; i <= n; i++) {
        if (buf[i] != '/' && buf[i] != 0) continue;
        const char c = buf[i];
        const int last = (buf[i] == 0);
        buf[i] = 0;
        struct stat st;
        if (last ? (lstat(buf, &st) == 0) : (stat(buf, &st) == 0)) {   // an existing ancestor (/private, /var, the user's cache) is used as-is: it is not ours to own
            if (!S_ISDIR(st.st_mode) || (last && S_ISLNK(st.st_mode))) { buf[i] = c; return -1; }
        } else {
            if (mkdir(buf, 0700) != 0 && errno != EEXIST) { buf[i] = c; return -1; }
            if (lstat(buf, &st) != 0 || !S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode) || st.st_uid != geteuid()) { buf[i] = c; return -1; }
        }
        buf[i] = c;
    }
    return 0;
}
// Removes every key directory under parent except `keep`: plain files (lstat) are unlinked, then rmdir. A symlink, or anything that is not a real directory of ours, is left alone and never followed.
// Returns the number of directories removed.
static inline int rdna4x_remove_old_keys(const char *parent, const char *keep) {
    DIR *d = opendir(parent);
    if (!d) return 0;
    int removed = 0; struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *nm = e->d_name;
        if (!strcmp(nm, ".") || !strcmp(nm, "..") || !strcmp(nm, keep)) continue;
        char sub[RDNA4X_PATH_MAX]; struct stat st;
        if (snprintf(sub, sizeof sub, "%s/%s", parent, nm) >= (int)sizeof sub) continue;
        if (lstat(sub, &st) != 0 || !S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode) || st.st_uid != geteuid()) continue;
        DIR *sd = opendir(sub);
        if (!sd) continue;
        struct dirent *f;
        while ((f = readdir(sd)) != NULL) {
            if (!strcmp(f->d_name, ".") || !strcmp(f->d_name, "..")) continue;
            char fp[RDNA4X_PATH_MAX + 300]; struct stat fs;
            if (snprintf(fp, sizeof fp, "%s/%s", sub, f->d_name) >= (int)sizeof fp) continue;
            if (lstat(fp, &fs) == 0 && S_ISREG(fs.st_mode)) (void)unlink(fp);
        }
        closedir(sd);
        if (rmdir(sub) == 0) removed++;
    }
    closedir(d);
    return removed;
}
// parent + parent/<key> exist and pass the checks, and old keys are gone. 0 = ready.
static inline int rdna4x_prepare_dirs(const char *parent, const char *dir, const char *key) {
    if (rdna4x_mkdir_p(parent) != 0) return -1;
    if (rdna4x_mkdir_checked(dir) != 0) return -1;
    (void)rdna4x_remove_old_keys(parent, key);
    return 0;
}

// ======================================================================================================================================================================
// 4. Size caps (section 1.5)
// ======================================================================================================================================================================
static inline int rdna4x_air_size_ok(size_t n) { return n >= 4 && n <= RDNA4X_MAX_AIR; }
static inline int rdna4x_ll_size_ok(size_t n)  { return n >= 1 && n <= RDNA4X_MAX_LL; }
static inline int rdna4x_spv_size_ok(size_t n) { return n >= 20 && (n & 3u) == 0 && n <= RDNA4X_MAX_SPV; }
static inline int rdna4x_meta_size_ok(size_t n) { return n >= 2 && n <= RDNA4X_MAX_META; }

// ======================================================================================================================================================================
// 5. The crash-marker state machine (section 1.7)
// ======================================================================================================================================================================
#define RDNA4X_PL_TRANSLATE          0   // nothing is known against this sha: write the marker and translate
#define RDNA4X_PL_SKIP_CRASH         1   // <sha>.crash: a translation of this sha killed a process before; never again under this key
#define RDNA4X_PL_SKIP_FAIL          2   // <sha>.fail: the translator refused it before
#define RDNA4X_PL_SKIP_LIVE          3   // <sha>.inflight of a live process
#define RDNA4X_PL_PROMOTE_THEN_SKIP  4   // <sha>.inflight of a DEAD process: it becomes <sha>.crash, and the sha is skipped
static inline int rdna4x_plan(int hasCrash, int hasFail, int hasInflight, int inflightAlive) {
    if (hasCrash) return RDNA4X_PL_SKIP_CRASH;
    if (hasInflight) return inflightAlive ? RDNA4X_PL_SKIP_LIVE : RDNA4X_PL_PROMOTE_THEN_SKIP;
    if (hasFail) return RDNA4X_PL_SKIP_FAIL;
    return RDNA4X_PL_TRANSLATE;
}
static inline int rdna4x_pid_alive(long pid) {
    if (pid <= 0) return 0;
    if (kill((pid_t)pid, 0) == 0) return 1;
    return errno == EPERM;   // exists, not ours
}
static inline int rdna4x_exists(const char *path) { struct stat st; return lstat(path, &st) == 0; }
// Reads the pid out of a marker; -1 when the file is unreadable or holds no number (a marker nobody can vouch for counts as DEAD: the translation that wrote it cannot be known to be running).
static inline long rdna4x_marker_pid(const char *path) {
    char b[32]; const int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return -1;
    const ssize_t n = read(fd, b, sizeof b - 1);
    close(fd);
    if (n <= 0) return -1;
    b[n] = 0;
    char *e = NULL; const long v = strtol(b, &e, 10);
    return (e && e != b) ? v : -1;
}
// One sha's state in dir -> plan. Promotes a dead marker to .crash (rename, no link followed: both names are ours in a directory we own).
static inline int rdna4x_probe(const char *dir, const char *sha) {
    char crash[RDNA4X_PATH_MAX], fail[RDNA4X_PATH_MAX], infl[RDNA4X_PATH_MAX];
    if (!rdna4x_file_path(crash, sizeof crash, dir, sha, RDNA4X_EXT_CRASH) || !rdna4x_file_path(fail, sizeof fail, dir, sha, RDNA4X_EXT_FAIL) || !rdna4x_file_path(infl, sizeof infl, dir, sha, RDNA4X_EXT_INFLIGHT)) return RDNA4X_PL_SKIP_CRASH;   // unusable names: never translate
    const int hasInfl = rdna4x_exists(infl);
    const int alive = hasInfl ? rdna4x_pid_alive(rdna4x_marker_pid(infl)) : 0;
    const int plan = rdna4x_plan(rdna4x_exists(crash), rdna4x_exists(fail), hasInfl, alive);
    if (plan == RDNA4X_PL_PROMOTE_THEN_SKIP) { if (rename(infl, crash) != 0) (void)unlink(infl); }
    return plan;
}
// Creates <sha>.inflight with O_EXCL (mode 0600) holding the pid. 0 = created, 1 = already exists, -1 = error.
// 0 = marker written, 1 = another live process holds it, negative = -errno.
static inline int rdna4x_marker_create(const char *dir, const char *sha) {
    char infl[RDNA4X_PATH_MAX];
    if (!rdna4x_file_path(infl, sizeof infl, dir, sha, RDNA4X_EXT_INFLIGHT)) return -EINVAL;
    const int fd = open(infl, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) return errno == EEXIST ? 1 : -errno;
    char b[32]; const int n = snprintf(b, sizeof b, "%d\n", (int)getpid());
    const ssize_t w = write(fd, b, (size_t)n);
    const int c = close(fd);
    if (w != n || c != 0) { const int er = errno ? errno : EIO; (void)unlink(infl); return -er; }
    return 0;
}
static inline void rdna4x_marker_remove(const char *dir, const char *sha) {
    char infl[RDNA4X_PATH_MAX];
    if (rdna4x_file_path(infl, sizeof infl, dir, sha, RDNA4X_EXT_INFLIGHT)) (void)unlink(infl);
}
// The first miss in a process: every leftover marker in dir whose process is dead becomes <sha>.crash. Returns how many were promoted.
static inline int rdna4x_sweep(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    int n = 0; struct dirent *e;
    const size_t el = sizeof RDNA4X_EXT_INFLIGHT - 1;
    while ((e = readdir(d)) != NULL) {
        const size_t l = strlen(e->d_name);
        if (l != 64 + el || strcmp(e->d_name + 64, RDNA4X_EXT_INFLIGHT) != 0) continue;
        char sha[65]; memcpy(sha, e->d_name, 64); sha[64] = 0;
        if (!rdna4x_sha_ok(sha)) continue;
        if (rdna4x_probe(dir, sha) == RDNA4X_PL_PROMOTE_THEN_SKIP) n++;
    }
    closedir(d);
    return n;
}

// ======================================================================================================================================================================
// 6. The size cap and the least-recently-used trim (section 1.4)
// ======================================================================================================================================================================
typedef struct { char sha[65]; int64_t mtime; uint64_t bytes; } rdna4x_ent;   // newest access or modification time and total bytes per sha
typedef struct { int64_t mtime; size_t idx; } rdna4x_ord;
static inline int rdna4x_ord_cmp(const void *a, const void *b) {
    const rdna4x_ord *x = (const rdna4x_ord *)a, *y = (const rdna4x_ord *)b;
    if (x->mtime != y->mtime) return x->mtime < y->mtime ? -1 : 1;
    return x->idx < y->idx ? -1 : (x->idx > y->idx ? 1 : 0);
}
// Fills del[i] = 1 for the entries to remove: nothing while the total is within capBytes, else the oldest entries until the total is at or below 80% of the cap. Returns how many.
static inline size_t rdna4x_trim_select(const rdna4x_ent *e, size_t n, uint64_t capBytes, unsigned char *del) {
    uint64_t total = 0;
    for (size_t i = 0; i < n; i++) { del[i] = 0; total += e[i].bytes; }
    if (total <= capBytes) return 0;
    const uint64_t target = capBytes / 10u * 8u;
    rdna4x_ord *o = (rdna4x_ord *)malloc((n ? n : 1) * sizeof *o);
    if (!o) return 0;
    for (size_t i = 0; i < n; i++) { o[i].mtime = e[i].mtime; o[i].idx = i; }
    qsort(o, n, sizeof *o, rdna4x_ord_cmp);
    size_t k = 0;
    for (size_t j = 0; j < n && total > target; j++) { del[o[j].idx] = 1; total -= e[o[j].idx].bytes; k++; }
    free(o);
    return k;
}
// Executes the trim on one cache directory. Plain files only (lstat); .crash / .inflight markers are never counted and never removed. Returns the number of shas removed.
static inline size_t rdna4x_trim_dir(const char *dir, uint64_t capBytes) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    size_t cap = 256, n = 0; rdna4x_ent *e = (rdna4x_ent *)malloc(cap * sizeof *e);
    if (!e) { closedir(d); return 0; }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *nm = de->d_name; const size_t l = strlen(nm);
        if (l < 65 || nm[64] != '.') continue;
        const char *ext = nm + 64;
        if (strcmp(ext, RDNA4X_EXT_SPV) && strcmp(ext, RDNA4X_EXT_META) && strcmp(ext, RDNA4X_EXT_FAIL)) continue;
        char sha[65]; memcpy(sha, nm, 64); sha[64] = 0;
        if (!rdna4x_sha_ok(sha)) continue;
        char p[RDNA4X_PATH_MAX]; struct stat st;
        if (snprintf(p, sizeof p, "%s/%s", dir, nm) >= (int)sizeof p || lstat(p, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        size_t i = 0; while (i < n && strcmp(e[i].sha, sha)) i++;
        if (i == n) {
            if (n == cap) { rdna4x_ent *g = (rdna4x_ent *)realloc(e, cap * 2 * sizeof *e); if (!g) break; e = g; cap *= 2; }
            memcpy(e[n].sha, sha, 65); e[n].mtime = 0; e[n].bytes = 0; n++;
        }
        const int64_t modified = (int64_t)st.st_mtimespec.tv_sec;
        const int64_t accessed = (int64_t)st.st_atimespec.tv_sec;
        const int64_t recency = !strcmp(ext, RDNA4X_EXT_SPV) && accessed > modified ? accessed : modified;
        if (recency > e[i].mtime) e[i].mtime = recency;
        e[i].bytes += (uint64_t)st.st_size;
    }
    closedir(d);
    unsigned char *del = (unsigned char *)calloc(n ? n : 1, 1);
    size_t removed = 0;
    if (del && rdna4x_trim_select(e, n, capBytes, del)) {
        for (size_t i = 0; i < n; i++) if (del[i]) {
            static const char *const exts[] = { RDNA4X_EXT_META, RDNA4X_EXT_SPV, RDNA4X_EXT_FAIL };
            for (size_t x = 0; x < 3; x++) { char p[RDNA4X_PATH_MAX]; struct stat st; if (rdna4x_file_path(p, sizeof p, dir, e[i].sha, exts[x]) && lstat(p, &st) == 0 && S_ISREG(st.st_mode)) (void)unlink(p); }
            removed++;
        }
    }
    free(del); free(e);
    return removed;
}
// Modification time touched on a cache hit (the caller does it once per sha per process).
static inline void rdna4x_touch(const char *path) { (void)utimes(path, NULL); }

// ======================================================================================================================================================================
// 7. Mach-O UUID of an image in memory (the translator library is read from its file; a loaded reader through dladdr)
// ======================================================================================================================================================================
static inline int rdna4x_macho_uuid(const void *mh, size_t avail, uint8_t out[16]) {
    if (!mh || avail < sizeof(struct mach_header_64)) return 0;
    const struct mach_header_64 *h = (const struct mach_header_64 *)mh;
    if (h->magic != MH_MAGIC_64) return 0;
    const uint8_t *p = (const uint8_t *)mh + sizeof *h;
    size_t left = avail - sizeof *h; if (left > h->sizeofcmds) left = h->sizeofcmds;
    for (uint32_t i = 0; i < h->ncmds; i++) {
        if (left < sizeof(struct load_command)) return 0;
        const struct load_command *lc = (const struct load_command *)p;
        if (lc->cmdsize < sizeof *lc || lc->cmdsize > left) return 0;
        if (lc->cmd == LC_UUID && lc->cmdsize >= sizeof(struct uuid_command)) { memcpy(out, ((const struct uuid_command *)lc)->uuid, 16); return 1; }
        p += lc->cmdsize; left -= lc->cmdsize;
    }
    return 0;
}
// LC_UUID of the thin 64-bit Mach-O file at path (reads the first 64 KiB; the load commands of a dylib are far smaller).
static inline int rdna4x_file_uuid(const char *path, uint8_t out[16]) {
    const int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return 0;
    uint8_t *b = (uint8_t *)malloc(65536);
    if (!b) { close(fd); return 0; }
    const ssize_t n = pread(fd, b, 65536, 0);
    close(fd);
    const int ok = n > 0 && rdna4x_macho_uuid(b, (size_t)n, out);
    free(b);
    return ok;
}

// ======================================================================================================================================================================
// 8. The worker thread (section 1.5): created with a 64 MiB stack
// ======================================================================================================================================================================
static inline int rdna4x_spawn(pthread_t *t, void *(*fn)(void *), void *arg, size_t stackBytes) {
    pthread_attr_t a;
    if (pthread_attr_init(&a) != 0) return -1;
    int rc = pthread_attr_setstacksize(&a, stackBytes);
    if (rc == 0) { (void)pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED); rc = pthread_create(t, &a, fn, arg); }
    pthread_attr_destroy(&a);
    return rc;
}

// ======================================================================================================================================================================
// 9. Small file helpers: atomic install, read
// ======================================================================================================================================================================
// <path>.tmp<pid> -> fchmod 0644 -> rename. 0 = installed.
static inline int rdna4x_write_atomic(const char *path, const void *data, size_t n) {
    char tmp[RDNA4X_PATH_MAX + 32];
    if (snprintf(tmp, sizeof tmp, "%s.tmp%d", path, (int)getpid()) >= (int)sizeof tmp) return -1;
    const int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    const char *p = (const char *)data; size_t left = n; int ok = 1;
    while (left) { const ssize_t w = write(fd, p, left); if (w <= 0) { ok = 0; break; } p += w; left -= (size_t)w; }
    if (ok && fchmod(fd, 0644) != 0) ok = 0;
    if (close(fd) != 0) ok = 0;
    if (!ok || rename(tmp, path) != 0) { (void)unlink(tmp); return -1; }
    return 0;
}
__attribute__((format(printf, 3, 0))) static inline void rdna4x_set_err(char *err, size_t n, const char *fmt, const char *a) { if (err && n) snprintf(err, n, fmt, a ? a : ""); }

// ======================================================================================================================================================================
// 10. Dependencies of one translation (the reader and the translator), so tests can inject fakes and the real ones are loaded lazily
// ======================================================================================================================================================================
typedef struct { const char *symbol; const uint8_t *air; size_t airLen; } rdna4x_ldep;       // a linked dependency as the caller has it: logical name + AIR bitcode
typedef struct { const char *symbol; const char *ll; size_t llLen; } rdna4x_ldep_ll;           // ... after the reader: name + LLVM text
typedef struct {
    void *ctx;
    // Bitcode -> LLVM text (malloc'ed by the reader; release with free_text). 0 = ok.
    int (*air_to_text)(void *ctx, const uint8_t *air, size_t n, char **text, size_t *len, char *err, size_t errn);
    void (*free_text)(void *ctx, char *text);
    // LLVM text -> SPIR-V + trimmed meta (the library's garnetx_translate). 0 ok, 1 refused, 2 panic caught, 3 bad arguments.
    int (*translate)(void *ctx, const char *ll, size_t llLen, const uint32_t *local, uint8_t **spv, size_t *spvLen, uint8_t **meta, size_t *metaLen, char *err, size_t errn);
    void (*release)(void *ctx, uint8_t *p, size_t n);
    // build 18 (P3): LLVM text + the authored dependencies' text -> SPIR-V + meta (the library's garnetx_translate_linked). Same codes as translate. NULL = linked translation unavailable.
    int (*translate_linked)(void *ctx, const char *ll, size_t llLen, const uint32_t *local, const rdna4x_ldep_ll *deps, size_t nd, uint8_t **spv, size_t *spvLen, uint8_t **meta, size_t *metaLen, char *err, size_t errn);
    int (*lower_arguments)(void *ctx, const uint8_t *spv, size_t spvLen, const uint8_t *meta, size_t metaLen,
        uint8_t **code, size_t *codeLen, uint8_t **json, size_t *jsonLen, char *err, size_t errn);
    int lowerTranslatedArguments;
} rdna4x_deps;

// ---- the crash-test hook: compiled in only with -DRDNA4X_TESTHOOKS (test-xlate.c); the shipped bundle has none ----
#ifdef RDNA4X_TESTHOOKS
#define RDNA4X_HOOK_AFTER_MARKER() do { const char *h_ = getenv("RDNA4X_TEST_EXIT_AFTER_MARKER"); if (h_ && h_[0] == '1') _exit(77); } while (0)
#else
#define RDNA4X_HOOK_AFTER_MARKER() do { } while (0)
#endif

// One translation, run on the worker (or directly by a test). Order matters and is pinned by test-xlate.c:
//   caps -> plan against the markers -> write <sha>.inflight (O_EXCL) -> bitcode to text -> text cap -> translate -> caps -> .meta.json -> .spv -> remove the marker -> trim.
// A refusal (reader, translator, a cap) writes <sha>.fail; an I/O problem writes nothing. The marker is removed on every path that returns (a crash leaves it: that is the point).
static inline int rdna4x_process_job(const rdna4x_deps *dp, const char *dir, const char *sha, const char *fname, const uint8_t *air, size_t airLen, const rdna4x_ldep *ld, size_t nld, const uint8_t *inputMeta, size_t inputMetaLen, char *err, size_t errn) {
    if (err && errn) err[0] = 0;
    if (!dp || !dp->release || (inputMeta && !dp->lower_arguments) ||
        (!inputMeta && (!dp->air_to_text || !dp->translate || !dp->free_text))) {
        rdna4x_set_err(err, errn, "required translator operation is unavailable%s", ""); return RDNA4X_E_UNAVAILABLE;
    }
    if (!air || (inputMeta ? !rdna4x_spv_size_ok(airLen) || !rdna4x_meta_size_ok(inputMetaLen) : !rdna4x_air_size_ok(airLen))) {
        rdna4x_set_err(err, errn, "translator input size outside the caps%s", ""); return RDNA4X_E_CAP;
    }
    if (!rdna4x_sha_ok(sha) || !dir || !dir[0]) { rdna4x_set_err(err, errn, "bad sha or directory%s", ""); return RDNA4X_E_IO; }
    {   // P4 (build 16): a repeat of a translation that has already been installed (a caller that missed the running job by a hair) is answered, not run again: the .spv is written LAST, so its presence means the pair is complete
        char done[RDNA4X_PATH_MAX]; struct stat dst;
        if (rdna4x_file_path(done, sizeof done, dir, sha, RDNA4X_EXT_SPV) && lstat(done, &dst) == 0 && S_ISREG(dst.st_mode)) return RDNA4X_OK;
    }
    switch (rdna4x_probe(dir, sha)) {
    case RDNA4X_PL_SKIP_CRASH: case RDNA4X_PL_PROMOTE_THEN_SKIP: rdna4x_set_err(err, errn, "a translation of this shader crashed a process before (.crash)%s", ""); return RDNA4X_E_SKIP_CRASH;
    case RDNA4X_PL_SKIP_FAIL:  rdna4x_set_err(err, errn, "the translator refused this shader before (.fail)%s", ""); return RDNA4X_E_SKIP_FAIL;
    case RDNA4X_PL_SKIP_LIVE:  rdna4x_set_err(err, errn, "another process is translating this shader%s", ""); return RDNA4X_E_SKIP_LIVE;
    default: break;
    }
    int mk = rdna4x_marker_create(dir, sha);
    if (mk < 0) {
        // The cache directory can be gone by the time the job runs: a sandboxed
        // application's container cache directory is materialised only when it is
        // first used, another key's cleanup removes siblings, and /private/var
        // folders are purged. Recreate <parent>/<key> and try once more; a missing
        // directory is an ENOENT here, never a reason to leave a draw without a shader.
        char parent[RDNA4X_PATH_MAX], key[17];
        const char *slash = strrchr(dir, '/');
        const size_t pl = slash ? (size_t)(slash - dir) : 0, kl = slash ? strlen(slash + 1) : 0;
        if (slash && pl > 0 && pl < sizeof parent && kl == 16 && kl < sizeof key) {
            memcpy(parent, dir, pl); parent[pl] = 0;
            memcpy(key, slash + 1, kl + 1);
            if (rdna4x_key_ok(key) && rdna4x_prepare_dirs(parent, dir, key) == 0)
                mk = rdna4x_marker_create(dir, sha);
        }
    }
    if (mk == 1) { rdna4x_set_err(err, errn, "another process is translating this shader%s", ""); return RDNA4X_E_SKIP_LIVE; }
    if (mk != 0) {
        char why0[320];
        snprintf(why0, sizeof why0, "cannot create the crash marker in %s: %s", dir, strerror(mk < 0 ? -mk : EIO));
        rdna4x_set_err(err, errn, "%s", why0);
        return RDNA4X_E_IO;
    }
    RDNA4X_HOOK_AFTER_MARKER();
    char path[RDNA4X_PATH_MAX], failPath[RDNA4X_PATH_MAX], why[512]; why[0] = 0;
    (void)rdna4x_file_path(failPath, sizeof failPath, dir, sha, RDNA4X_EXT_FAIL);
    char *text = NULL; size_t tl = 0; uint8_t *spv = NULL, *meta = NULL; size_t sl = 0, ml = 0; int rc = RDNA4X_OK;
    rdna4x_ldep_ll *dl = NULL; char **dtext = NULL; size_t ndt = 0;
    if (inputMeta) {
        if (dp->lower_arguments(dp->ctx, air, airLen, inputMeta, inputMetaLen, &spv, &sl, &meta, &ml, why, sizeof why)) {
            rc = RDNA4X_E_REFUSED; goto refused;
        }
        goto output_ready;
    }
    if (dp->air_to_text(dp->ctx, air, airLen, &text, &tl, why, sizeof why) != 0 || !text) { rc = RDNA4X_E_READER; goto refused; }
    if (!rdna4x_ll_size_ok(tl)) { snprintf(why, sizeof why, "LLVM text of %zu bytes is outside the cap", tl); rc = RDNA4X_E_CAP; goto refused; }
    if (nld) {   // build 18 (P3): every dependency goes through the SAME reader as the entry; the library then sanitises each one like the entry
        if (!dp->translate_linked || nld > RDNA4X_MAX_DEPS || !ld) { snprintf(why, sizeof why, "linked translation unavailable or too many dependencies (%zu)", nld); rc = RDNA4X_E_UNAVAILABLE; goto refused; }
        dl = (rdna4x_ldep_ll *)calloc(nld, sizeof *dl); dtext = (char **)calloc(nld, sizeof *dtext);
        if (!dl || !dtext) { snprintf(why, sizeof why, "out of memory"); rc = RDNA4X_E_IO; goto refused; }
        size_t tot = tl;
        for (size_t i = 0; i < nld; i++) {
            char *t = NULL; size_t l = 0;
            if (!ld[i].symbol || !ld[i].symbol[0] || !rdna4x_air_size_ok(ld[i].airLen) || !ld[i].air) { snprintf(why, sizeof why, "dependency %zu: bad name or bitcode size", i); rc = RDNA4X_E_CAP; goto refused; }
            if (dp->air_to_text(dp->ctx, ld[i].air, ld[i].airLen, &t, &l, why, sizeof why) != 0 || !t) { rc = RDNA4X_E_READER; goto refused; }
            dtext[ndt++] = t; tot += l;
            if (!rdna4x_ll_size_ok(l) || tot > RDNA4X_MAX_LL) { snprintf(why, sizeof why, "LLVM text of the dependencies (%zu bytes) is outside the cap", tot); rc = RDNA4X_E_CAP; goto refused; }
            dl[i].symbol = ld[i].symbol; dl[i].ll = t; dl[i].llLen = l;
        }
    }
    {
        const uint32_t *loc = rdna4x_local_for(fname);
        const int tr = nld ? dp->translate_linked(dp->ctx, text, tl, loc, dl, nld, &spv, &sl, &meta, &ml, why, sizeof why)
                           : dp->translate(dp->ctx, text, tl, loc, &spv, &sl, &meta, &ml, why, sizeof why);
        dp->free_text(dp->ctx, text); text = NULL;
        for (size_t i = 0; i < ndt; i++) dp->free_text(dp->ctx, dtext[i]);
        ndt = 0; free(dl); free(dtext); dl = NULL; dtext = NULL;
        if (tr != 0) { rc = RDNA4X_E_REFUSED; goto refused; }
    }
output_ready:
    if (!spv || !meta || !rdna4x_spv_size_ok(sl) || !rdna4x_meta_size_ok(ml)) { snprintf(why, sizeof why, "SPIR-V %zu bytes / meta %zu bytes outside the caps", sl, ml); rc = RDNA4X_E_CAP; goto refused; }
    if (!inputMeta && dp->lowerTranslatedArguments && dp->lower_arguments) {
        uint8_t *code = NULL, *json = NULL; size_t cl = 0, jl = 0;
        int result = dp->lower_arguments(dp->ctx, spv, sl, meta, ml, &code, &cl, &json, &jl, why, sizeof why);
        dp->release(dp->ctx, spv, sl); dp->release(dp->ctx, meta, ml);
        spv = code; sl = cl; meta = json; ml = jl;
        if (result) { rc = RDNA4X_E_REFUSED; goto refused; }
        if (!spv || !meta || !rdna4x_spv_size_ok(sl) || !rdna4x_meta_size_ok(ml)) {
            snprintf(why, sizeof why, "argument postpass output outside the caps"); rc = RDNA4X_E_CAP; goto refused;
        }
    }
    // .meta.json FIRST, .spv LAST: the lookup keys on the .spv, so a reader that sees it always finds the meta.
    if (!rdna4x_file_path(path, sizeof path, dir, sha, RDNA4X_EXT_META) || rdna4x_write_atomic(path, meta, ml) != 0) { rc = RDNA4X_E_IO; snprintf(why, sizeof why, "cannot write the meta file"); goto done; }
    if (!rdna4x_file_path(path, sizeof path, dir, sha, RDNA4X_EXT_SPV) || rdna4x_write_atomic(path, spv, sl) != 0) { rc = RDNA4X_E_IO; snprintf(why, sizeof why, "cannot write the spv file"); goto done; }
    goto done;
refused:
    if (text) { dp->free_text(dp->ctx, text); text = NULL; }
    for (size_t i = 0; i < ndt; i++) dp->free_text(dp->ctx, dtext[i]);
    free(dl); free(dtext); dl = NULL; dtext = NULL; ndt = 0;
    (void)rdna4x_write_atomic(failPath, why, strlen(why));
done:
    if (spv) dp->release(dp->ctx, spv, sl);
    if (meta) dp->release(dp->ctx, meta, ml);
    rdna4x_marker_remove(dir, sha);
    if (rc == RDNA4X_OK) (void)rdna4x_trim_dir(dir, RDNA4X_CACHE_CAP);
    if (err && errn && why[0]) snprintf(err, errn, "%s", why);
    return rc;
}
static inline int rdna4x_translate_job_x(const rdna4x_deps *dp, const char *dir, const char *sha, const char *fname, const uint8_t *air, size_t airLen, const rdna4x_ldep *ld, size_t nld, char *err, size_t errn) {
    return rdna4x_process_job(dp, dir, sha, fname, air, airLen, ld, nld, NULL, 0, err, errn);
}
// The unlinked job (every caller before build 18).
static inline int rdna4x_translate_job(const rdna4x_deps *dp, const char *dir, const char *sha, const char *fname, const uint8_t *air, size_t airLen, char *err, size_t errn) {
    return rdna4x_translate_job_x(dp, dir, sha, fname, air, airLen, NULL, 0, err, errn);
}

// ======================================================================================================================================================================
// 11. The reader: Apple's LLVM from the OS (section 1.1, option A)
// ======================================================================================================================================================================
typedef struct {
    void *h; char path[RDNA4X_PATH_MAX];
    void *(*ContextCreate)(void);
    void (*ContextDispose)(void *);
    void (*ContextSetDiagnosticHandler)(void *, void (*)(void *, void *), void *);
    void *(*CreateMemoryBufferWithMemoryRangeCopy)(const char *, size_t, const char *);
    void (*DisposeMemoryBuffer)(void *);
    int (*ParseBitcodeInContext2)(void *, void *, void **);
    char *(*PrintModuleToString)(void *);
    void (*DisposeModule)(void *);
    void (*DisposeMessage)(char *);
    char *(*GetDiagInfoDescription)(void *);
    void (*SetOpaquePointers)(void *, int);   // OPTIONAL 11th symbol, llvm::LLVMContext::setOpaquePointers(bool) const (LLVM 15/16's public C++ method, exported by GPUCompiler's libLLVM): see rdna4x_reader_bind
} rdna4x_reader;

static inline int rdna4x_reader_bind(rdna4x_reader *r, void *h) {
#define RDNA4X_SYM(F) do { *(void **)&r->F = dlsym(h, "LLVM" #F); if (!r->F) return 0; } while (0)
    RDNA4X_SYM(ContextCreate); RDNA4X_SYM(ContextDispose); RDNA4X_SYM(ContextSetDiagnosticHandler); RDNA4X_SYM(CreateMemoryBufferWithMemoryRangeCopy); RDNA4X_SYM(DisposeMemoryBuffer);
    RDNA4X_SYM(ParseBitcodeInContext2); RDNA4X_SYM(PrintModuleToString); RDNA4X_SYM(DisposeModule); RDNA4X_SYM(DisposeMessage); RDNA4X_SYM(GetDiagInfoDescription);
#undef RDNA4X_SYM
    // Apple's GPUCompiler LLVM is an older LLVM that prints TYPED pointers (`%struct._sampler_t addrspace(2)*`, `bitcast ... to ...` constants) unless the context is switched to opaque pointers before
    // the module is read; the translator reads LLVM 23's text (`ptr addrspace(2)`). The switch is the public C++ method, found by its mangled name; a library without it (an LLVM 17+, which is
    // opaque-only, or a future OS) simply prints what it prints.
    *(void **)&r->SetOpaquePointers = dlsym(h, "_ZNK4llvm11LLVMContext17setOpaquePointersEb");
    r->h = h;
    return 1;
}
static inline int rdna4x_num_desc(const void *a, const void *b) {
    const unsigned long x = strtoul(*(const char *const *)a, NULL, 10), y = strtoul(*(const char *const *)b, NULL, 10);
    return x < y ? 1 : (x > y ? -1 : 0);
}
// Candidate paths, in order: Versions/Current, then every numeric Versions/<n>/ directory (highest first). Returns the count (<= max).
static inline size_t rdna4x_reader_candidates(const char *base, char out[][RDNA4X_PATH_MAX], size_t max) {
    size_t n = 0;
    if (n < max) snprintf(out[n++], RDNA4X_PATH_MAX, "%s/Current/Libraries/libLLVM.dylib", base);
    DIR *d = opendir(base);
    if (!d) return n;
    char *names[64]; size_t nn = 0; struct dirent *e;
    while ((e = readdir(d)) != NULL && nn < 64) {
        const char *s = e->d_name; size_t i = 0;
        if (!*s) continue;
        while (s[i] >= '0' && s[i] <= '9') i++;
        if (s[i] != 0 || i == 0 || i > 12) continue;
        names[nn++] = strdup(s);
    }
    closedir(d);
    qsort(names, nn, sizeof names[0], rdna4x_num_desc);
    for (size_t i = 0; i < nn; i++) { if (n < max) snprintf(out[n++], RDNA4X_PATH_MAX, "%s/%s/Libraries/libLLVM.dylib", base, names[i]); free(names[i]); }
    return n;
}
// override != NULL: only that path (tests; the bundle honours it for RDNA4M_ALLOW root tools only). Otherwise the candidates under base. 0 = loaded.
static inline int rdna4x_reader_load(rdna4x_reader *r, const char *base, const char *override, char *err, size_t errn) {
    memset(r, 0, sizeof *r);
    char cand[16][RDNA4X_PATH_MAX]; size_t n = 0;
    if (override && override[0]) { snprintf(cand[0], RDNA4X_PATH_MAX, "%s", override); n = 1; }
    else n = rdna4x_reader_candidates(base, cand, 16);
    for (size_t i = 0; i < n; i++) {
        void *h = dlopen(cand[i], RTLD_NOW | RTLD_LOCAL);
        if (!h) continue;
        if (rdna4x_reader_bind(r, h)) { snprintf(r->path, sizeof r->path, "%s", cand[i]); return 0; }
        dlclose(h);
    }
    rdna4x_set_err(err, errn, "no usable LLVM bitcode reader (%s)", override && override[0] ? override : base);
    return -1;
}
typedef struct { const rdna4x_reader *r; char msg[256]; } rdna4x_diag;
static inline void rdna4x_diag_cb(void *info, void *opaque) {
    rdna4x_diag *d = (rdna4x_diag *)opaque;
    if (d->msg[0] || !info) return;
    char *s = d->r->GetDiagInfoDescription(info);
    if (s) { snprintf(d->msg, sizeof d->msg, "%s", s); d->r->DisposeMessage(s); }
}
static inline int rdna4x_reader_print(const rdna4x_reader *r, const uint8_t *air, size_t n, char **text, size_t *len, char *err, size_t errn) {
    *text = NULL; *len = 0;
    rdna4x_diag dg; dg.r = r; dg.msg[0] = 0;
    void *ctx = r->ContextCreate();
    if (!ctx) { rdna4x_set_err(err, errn, "LLVMContextCreate failed%s", ""); return -1; }
    if (r->SetOpaquePointers) r->SetOpaquePointers(ctx, 1);   // before anything is read into the context
    r->ContextSetDiagnosticHandler(ctx, rdna4x_diag_cb, &dg);
    void *buf = r->CreateMemoryBufferWithMemoryRangeCopy((const char *)air, n, "air");
    void *mod = NULL; int rc = -1;
    if (buf && r->ParseBitcodeInContext2(ctx, buf, &mod) == 0 && mod) {
        char *s = r->PrintModuleToString(mod);
        if (s) {
            const size_t l = strlen(s);
            char *copy = (char *)malloc(l + 1);   // the library's string is released with LLVMDisposeMessage; hand out our own so the caller frees it with free()
            if (copy) { memcpy(copy, s, l + 1); *text = copy; *len = l; rc = 0; }
            r->DisposeMessage(s);
        }
    } else rdna4x_set_err(err, errn, "bitcode not readable: %s", dg.msg[0] ? dg.msg : "parse failed");
    if (mod) r->DisposeModule(mod);
    if (buf) r->DisposeMemoryBuffer(buf);
    r->ContextDispose(ctx);
    return rc;
}

// ======================================================================================================================================================================
// 12. The translator library (Resources/libgarnetxlate.dylib)
// ======================================================================================================================================================================
typedef struct {
    uint32_t location;
    uint32_t buffer_index;
    uint32_t offset;
    uint32_t stride;
    uint8_t vfmt;
    uint8_t step_func;
    uint8_t pad[2];
    uint32_t step_rate;
} garnetx_icb_vattr_t;
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(garnetx_icb_vattr_t) == 24, "garnetx_icb_vattr_t must be 24 bytes");
#endif

typedef struct {
    void *h;
    uint32_t (*abi_version)(void);
    int (*translate)(const uint8_t *, size_t, const uint32_t *, uint8_t **, size_t *, uint8_t **, size_t *, char *, size_t);
    void (*freep)(uint8_t *, size_t);
    int (*translate_linked)(const uint8_t *, size_t, const uint32_t *, const void *, size_t, uint8_t **, size_t *, uint8_t **, size_t *, char *, size_t);   // build 18 (P3), ABI 2
    int (*lower_arguments)(const uint8_t *, size_t, const uint8_t *, size_t,
        uint8_t **, size_t *, uint8_t **, size_t *, char *, size_t);
    int (*translate_with_capabilities)(const uint8_t *, size_t, const uint32_t *, uint8_t **, size_t *, uint8_t **, size_t *, char *, size_t, const rdna4x_backend_capabilities *);
    int (*translate_linked_with_capabilities)(const uint8_t *, size_t, const uint32_t *, const void *, size_t, uint8_t **, size_t *, uint8_t **, size_t *, char *, size_t, const rdna4x_backend_capabilities *);
    int (*lower_arguments_with_capabilities)(const uint8_t *, size_t, const uint8_t *, size_t, uint8_t **, size_t *, uint8_t **, size_t *, char *, size_t, const rdna4x_backend_capabilities *);
    int (*lower_icb_stage)(const uint8_t *, size_t, uint32_t, uint32_t, const garnetx_icb_vattr_t *, size_t, uint8_t **, size_t *, uint32_t *, uint32_t *, char *, size_t);
} rdna4x_xlib;
static inline int rdna4x_xlib_load(rdna4x_xlib *x, const char *path, char *err, size_t errn) {
    memset(x, 0, sizeof *x);
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) { rdna4x_set_err(err, errn, "cannot load the translator library %s", path); return -1; }
    *(void **)&x->abi_version = dlsym(h, "garnetx_abi_version");
    *(void **)&x->translate = dlsym(h, "garnetx_translate");
    *(void **)&x->freep = dlsym(h, "garnetx_free");
    *(void **)&x->translate_linked = dlsym(h, "garnetx_translate_linked");
    *(void **)&x->lower_arguments = dlsym(h, "garnetx_lower_argument_resources");
    *(void **)&x->translate_with_capabilities = dlsym(h, "garnetx_translate_with_capabilities");
    *(void **)&x->translate_linked_with_capabilities = dlsym(h, "garnetx_translate_linked_with_capabilities");
    *(void **)&x->lower_arguments_with_capabilities = dlsym(h, "garnetx_lower_argument_resources_with_capabilities");
    *(void **)&x->lower_icb_stage = dlsym(h, "garnetx_lower_icb_stage");
    if (!x->abi_version || !x->translate || !x->freep || !x->translate_linked ||
        !x->translate_with_capabilities || !x->translate_linked_with_capabilities || !x->lower_arguments_with_capabilities) { dlclose(h); memset(x, 0, sizeof *x); rdna4x_set_err(err, errn, "the translator library lacks the garnetx_* entry points: %s", path); return -1; }
    if (x->abi_version() != RDNA4X_ABI_VERSION) { dlclose(h); memset(x, 0, sizeof *x); rdna4x_set_err(err, errn, "the translator library has an unknown ABI version: %s", path); return -1; }
    x->h = h;
    return 0;
}

typedef struct { rdna4x_reader rd; rdna4x_xlib xl; rdna4x_backend_capabilities capabilities; const char *gpucBase, *readerOverride; } rdna4x_real;
static inline int rdna4x_real_air_to_text(void *c, const uint8_t *air, size_t n, char **t, size_t *l, char *e, size_t en) {
    rdna4x_real *r = c;
    if (!r->rd.h && rdna4x_reader_load(&r->rd, r->gpucBase, r->readerOverride, e, en)) return -1;
    return rdna4x_reader_print(&r->rd, air, n, t, l, e, en);
}
static inline void rdna4x_real_free_text(void *c, char *t) { (void)c; free(t); }
static inline int rdna4x_real_translate(void *c, const char *ll, size_t ll_len, const uint32_t *local, uint8_t **spv, size_t *sl, uint8_t **meta, size_t *ml, char *e, size_t en) {
    rdna4x_real *r = c;
    return r->xl.translate_with_capabilities((const uint8_t *)ll, ll_len, local, spv, sl, meta, ml, e, en, &r->capabilities);
}
static inline void rdna4x_real_release(void *c, uint8_t *p, size_t n) { ((rdna4x_real *)c)->xl.freep(p, n); }
static inline int rdna4x_real_lower_arguments(void *c, const uint8_t *spv, size_t sl, const uint8_t *meta, size_t ml,
    uint8_t **code, size_t *cl, uint8_t **json, size_t *jl, char *error, size_t errorBytes) {
    rdna4x_real *r = c;
    if (!r->xl.lower_arguments_with_capabilities) { rdna4x_set_err(error, errorBytes, "translator lacks GPU argument resource lowering%s", ""); return 3; }
    return r->xl.lower_arguments_with_capabilities(spv, sl, meta, ml, code, cl, json, jl, error, errorBytes, &r->capabilities);
}
// The library's RDNA4xDep: { symbol, symbol_len, ll, ll_len }.
typedef struct { const uint8_t *symbol; size_t symbol_len; const uint8_t *ll; size_t ll_len; } rdna4x_cdep;
static inline int rdna4x_real_translate_linked(void *c, const char *ll, size_t ll_len, const uint32_t *local, const rdna4x_ldep_ll *deps, size_t nd, uint8_t **spv, size_t *sl, uint8_t **meta, size_t *ml, char *e, size_t en) {
    rdna4x_cdep *cd = (rdna4x_cdep *)calloc(nd ? nd : 1, sizeof *cd);
    if (!cd) { rdna4x_set_err(e, en, "out of memory%s", ""); return 3; }
    for (size_t i = 0; i < nd; i++) { cd[i].symbol = (const uint8_t *)deps[i].symbol; cd[i].symbol_len = strlen(deps[i].symbol); cd[i].ll = (const uint8_t *)deps[i].ll; cd[i].ll_len = deps[i].llLen; }
    rdna4x_real *r = c;
    const int rc = r->xl.translate_linked_with_capabilities((const uint8_t *)ll, ll_len, local, cd, nd, spv, sl, meta, ml, e, en, &r->capabilities);
    free(cd);
    return rc;
}
// Load the translator here; cached-SPIR-V jobs do not need the AIR reader.
static inline int rdna4x_real_open(rdna4x_real *r, rdna4x_deps *dp, const char *gpucBase, const char *readerOverride, const char *xlPath, char *err, size_t errn) {
    memset(r, 0, sizeof *r);
    memset(dp, 0, sizeof *dp);
    r->capabilities = rdna4x_query_caps(0, 0, 0, 0, 0);
    r->gpucBase = gpucBase; r->readerOverride = readerOverride;
    if (rdna4x_xlib_load(&r->xl, xlPath, err, errn) != 0) return -1;
    dp->ctx = r; dp->air_to_text = rdna4x_real_air_to_text; dp->free_text = rdna4x_real_free_text; dp->translate = rdna4x_real_translate; dp->release = rdna4x_real_release; dp->translate_linked = rdna4x_real_translate_linked;
    dp->lower_arguments = r->xl.lower_arguments_with_capabilities ? rdna4x_real_lower_arguments : NULL;
    return 0;
}

// ======================================================================================================================================================================
// 13. The engine: one worker thread, jobs one at a time, the caller waits up to a deadline (section 1.5)
// ======================================================================================================================================================================
typedef struct rdna4x_job {
    struct rdna4x_job *next;
    char dir[RDNA4X_PATH_MAX], sha[65], name[128];
    uint8_t *air; size_t airLen;
    uint8_t *inputMeta; size_t inputMetaLen;
    rdna4x_ldep *deps; size_t nd;   // build 18 (P3): owned copies (name + bitcode) of the linked dependencies
    int refs, done, rc; char err[256];
} rdna4x_job;
typedef struct rdna4x_engine {
    pthread_mutex_t mu; pthread_cond_t cvWork, cvDone;
    rdna4x_job *head, *tail, *running;
    int started, tried, ready; size_t stack;
    int lowerTranslatedArguments;
    rdna4x_backend_capabilities capabilities;
    rdna4x_deps deps; rdna4x_real real;
    int useReal; char gpucBase[RDNA4X_PATH_MAX], readerOverride[RDNA4X_PATH_MAX], xlPath[RDNA4X_PATH_MAX];
    char sweptDir[RDNA4X_PATH_MAX];
    char loadErr[256];
} rdna4x_engine;

// deps != NULL: use those (tests). deps == NULL: load the real reader and translator on the worker thread, on the first job. stack: the worker's stack size (RDNA4X_WORKER_STACK in the bundle).
static inline void rdna4x_engine_init(rdna4x_engine *e, const rdna4x_deps *deps, const char *gpucBase, const char *readerOverride, const char *xlPath, size_t stack) {
    memset(e, 0, sizeof *e);
    pthread_mutex_init(&e->mu, NULL); pthread_cond_init(&e->cvWork, NULL); pthread_cond_init(&e->cvDone, NULL);
    e->stack = stack;
    e->capabilities = rdna4x_query_caps(0, 0, 0, 0, 0);
    if (deps) { e->deps = *deps; e->ready = 1; }
    else {
        e->useReal = 1;
        snprintf(e->gpucBase, sizeof e->gpucBase, "%s", gpucBase ? gpucBase : RDNA4X_GPUC_BASE);
        snprintf(e->readerOverride, sizeof e->readerOverride, "%s", readerOverride ? readerOverride : "");
        snprintf(e->xlPath, sizeof e->xlPath, "%s", xlPath ? xlPath : "");
    }
}
static inline void rdna4x_job_unref(rdna4x_engine *e, rdna4x_job *j) {   // caller holds e->mu
    (void)e;
    if (--j->refs == 0) { for (size_t i = 0; i < j->nd; i++) { free((void *)j->deps[i].symbol); free((void *)j->deps[i].air); } free(j->deps); free(j->air); free(j->inputMeta); free(j); }
}
static inline void *rdna4x_worker_main(void *arg) {
    rdna4x_engine *e = (rdna4x_engine *)arg;
    pthread_mutex_lock(&e->mu);
    for (;;) {
        while (!e->head) pthread_cond_wait(&e->cvWork, &e->mu);
        rdna4x_job *j = e->head; e->head = j->next; if (!e->head) e->tail = NULL;
        e->running = j;
        if (!e->ready && !e->tried) {   // first job: load the reader and the translator, here, off the caller's thread
            e->tried = 1;
            pthread_mutex_unlock(&e->mu);
            const int lr = rdna4x_real_open(&e->real, &e->deps, e->gpucBase, e->readerOverride[0] ? e->readerOverride : NULL, e->xlPath, e->loadErr, sizeof e->loadErr);
            pthread_mutex_lock(&e->mu);
            e->real.capabilities = e->capabilities;
            e->ready = (lr == 0);
            e->deps.lowerTranslatedArguments = e->lowerTranslatedArguments;
        }
        pthread_mutex_unlock(&e->mu);
        int rc;
        if (!e->ready) { rc = RDNA4X_E_UNAVAILABLE; snprintf(j->err, sizeof j->err, "%s", e->loadErr[0] ? e->loadErr : "translator unavailable"); }
        else {
            if (strcmp(e->sweptDir, j->dir) != 0) { (void)rdna4x_sweep(j->dir); snprintf(e->sweptDir, sizeof e->sweptDir, "%s", j->dir); }   // the first miss in a directory: dead markers become .crash
            rc = rdna4x_process_job(&e->deps, j->dir, j->sha, j->name, j->air, j->airLen, j->deps, j->nd, j->inputMeta, j->inputMetaLen, j->err, sizeof j->err);
        }
        pthread_mutex_lock(&e->mu);
        j->rc = rc; j->done = 1; e->running = NULL;
        pthread_cond_broadcast(&e->cvDone);
        rdna4x_job_unref(e, j);
    }
    return NULL;
}
static inline rdna4x_job *rdna4x_find_job(rdna4x_engine *e, const char *dir, const char *sha) {   // caller holds e->mu
    for (rdna4x_job *j = e->head; j; j = j->next) if (!strcmp(j->sha, sha) && !strcmp(j->dir, dir)) return j;
    if (e->running && !strcmp(e->running->sha, sha) && !strcmp(e->running->dir, dir)) return e->running;
    return NULL;
}
// Submits one translation and waits up to waitMs (0 = do not wait; the worker still finishes it and installs the result). A second request for a sha already queued or running shares that job.
// Returns the job's code, or RDNA4X_E_TIMEOUT when the wait ran out. err (optional) gets a message.
static inline int rdna4x_engine_submit(rdna4x_engine *e, const char *dir, const char *sha, const char *fname, const uint8_t *air, size_t airLen, const rdna4x_ldep *ld, size_t nld, const uint8_t *inputMeta, size_t inputMetaLen, int waitMs, char *err, size_t errn) {
    if (err && errn) err[0] = 0;
    if (nld > RDNA4X_MAX_DEPS || (nld && !ld)) { rdna4x_set_err(err, errn, "too many linked dependencies%s", ""); return RDNA4X_E_CAP; }
    { size_t tot = 0; for (size_t i = 0; i < nld; i++) { tot += ld[i].airLen; if (!ld[i].symbol || !ld[i].symbol[0] || !rdna4x_air_size_ok(ld[i].airLen) || !ld[i].air || tot > RDNA4X_MAX_DEP_AIR) { if (err && errn) snprintf(err, errn, "linked dependency %zu: bad name or bitcode outside the caps", i); return RDNA4X_E_CAP; } } }
    if (!air || (inputMeta ? !rdna4x_spv_size_ok(airLen) || !rdna4x_meta_size_ok(inputMetaLen) : !rdna4x_air_size_ok(airLen))) {
        rdna4x_set_err(err, errn, "translator input size outside the caps%s", ""); return RDNA4X_E_CAP;
    }
    if (!rdna4x_sha_ok(sha) || !dir || strlen(dir) >= RDNA4X_PATH_MAX) return RDNA4X_E_IO;
    pthread_mutex_lock(&e->mu);
    rdna4x_job *j = rdna4x_find_job(e, dir, sha);
    if (j) j->refs++;
    else {
        j = (rdna4x_job *)calloc(1, sizeof *j);
        uint8_t *copy = (uint8_t *)malloc(airLen);
        if (!j || !copy) { free(j); free(copy); pthread_mutex_unlock(&e->mu); return RDNA4X_E_IO; }
        memcpy(copy, air, airLen);
        j->air = copy; j->airLen = airLen; j->refs = 2;   // the waiter and the worker
        if (inputMeta) {
            j->inputMeta = malloc(inputMetaLen);
            if (!j->inputMeta) { j->refs = 1; rdna4x_job_unref(e, j); pthread_mutex_unlock(&e->mu); return RDNA4X_E_IO; }
            memcpy(j->inputMeta, inputMeta, inputMetaLen); j->inputMetaLen = inputMetaLen;
        }
        if (nld) {   // build 18 (P3): the worker outlives the caller's arrays
            j->deps = (rdna4x_ldep *)calloc(nld, sizeof *j->deps);
            int bad = j->deps == NULL;
            for (size_t i = 0; i < nld && !bad; i++) {
                char *sy = strdup(ld[i].symbol); uint8_t *ab = (uint8_t *)malloc(ld[i].airLen);
                if (!sy || !ab) { free(sy); free(ab); bad = 1; break; }
                memcpy(ab, ld[i].air, ld[i].airLen); j->deps[i].symbol = sy; j->deps[i].air = ab; j->deps[i].airLen = ld[i].airLen; j->nd = i + 1;
            }
            if (bad) { j->refs = 1; rdna4x_job_unref(e, j); pthread_mutex_unlock(&e->mu); return RDNA4X_E_IO; }
        }
        snprintf(j->dir, sizeof j->dir, "%s", dir); memcpy(j->sha, sha, 65); snprintf(j->name, sizeof j->name, "%s", fname ? fname : "");
        if (!e->started) {
            pthread_t t;
            if (rdna4x_spawn(&t, rdna4x_worker_main, e, e->stack) != 0) { j->refs = 1; rdna4x_job_unref(e, j); pthread_mutex_unlock(&e->mu); return RDNA4X_E_IO; }
            e->started = 1;
        }
        if (e->tail) e->tail->next = j; else e->head = j;
        e->tail = j;
        pthread_cond_signal(&e->cvWork);
    }
    int rc = RDNA4X_E_TIMEOUT;
    if (waitMs > 0) {
        struct timeval tv; gettimeofday(&tv, NULL);
        struct timespec ts; ts.tv_sec = tv.tv_sec + waitMs / 1000; ts.tv_nsec = (long)tv.tv_usec * 1000 + (long)(waitMs % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        while (!j->done) { if (pthread_cond_timedwait(&e->cvDone, &e->mu, &ts) == ETIMEDOUT) break; }
    }
    if (j->done) { rc = j->rc; if (err && errn) snprintf(err, errn, "%s", j->err); }
    rdna4x_job_unref(e, j);
    pthread_mutex_unlock(&e->mu);
    return rc;
}
static inline int rdna4x_engine_run_linked(rdna4x_engine *e, const char *dir, const char *sha, const char *fname, const uint8_t *air, size_t airLen, const rdna4x_ldep *ld, size_t nld, int waitMs, char *err, size_t errn) {
    return rdna4x_engine_submit(e, dir, sha, fname, air, airLen, ld, nld, NULL, 0, waitMs, err, errn);
}
static inline int rdna4x_engine_lower_arguments(rdna4x_engine *e, const char *dir, const char *sha,
    const uint8_t *spv, size_t spvBytes, const uint8_t *meta, size_t metaBytes, int waitMs, char *err, size_t errn) {
    if (!meta) { rdna4x_set_err(err, errn, "missing argument metadata%s", ""); return RDNA4X_E_CAP; }
    return rdna4x_engine_submit(e, dir, sha, "argument resources", spv, spvBytes, NULL, 0, meta, metaBytes, waitMs, err, errn);
}
// The unlinked request (every caller before build 18).
static inline int rdna4x_engine_run(rdna4x_engine *e, const char *dir, const char *sha, const char *fname, const uint8_t *air, size_t airLen, int waitMs, char *err, size_t errn) {
    return rdna4x_engine_run_linked(e, dir, sha, fname, air, airLen, NULL, 0, waitMs, err, errn);
}

#endif /* RDNA4_XLATE_H */
