// garnet_xlate_fallback.h - Pure C POSIX helpers for two-tier in-process shader cache fallback.
// Zero macOS-only headers so Linux and macOS host tests can compile and execute this exact production code under ASan+UBSan.
#ifndef RDNA4_XLATE_FALLBACK_H
#define RDNA4_XLATE_FALLBACK_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "garnet_gate.h"

#ifndef RDNA4X_PATH_MAX
#define RDNA4X_PATH_MAX 1024
#endif
#ifndef RDNA4X_CACHE_SUB
#define RDNA4X_CACHE_SUB "org.garnet.xlate"
#endif

// Tier-2 bounded process-local memory cache (64 MiB default) used when both
// <DARWIN_USER_CACHE_DIR>/org.garnet.xlate/<K> and <DARWIN_USER_TEMP_DIR>/TemporaryItems/org.garnet.xlate/<K>
// are denied by a strict Seatbelt sandbox (e.g. com.apple.WebKit.GPU).
#define RDNA4X_MEMCACHE_CAP (64u * 1024u * 1024u)
#define RDNA4X_MEMCACHE_DIR ":memory:"

static inline int rdna4x_is_mem_dir(const char *dir) {
    return dir != NULL && strcmp(dir, RDNA4X_MEMCACHE_DIR) == 0;
}

// ABI 1.14 sandbox-aware kill-file check for /private/tmp/garnet-noinproc:
// A PRESENT file always disables inproc. ENOENT/ENOTDIR means enabled.
// For a kernel-admitted sandboxed app (isAdmittedApp != 0), EPERM/EACCES from Seatbelt on /private/tmp
// does not disable inproc; root tools still fail closed on unreadable kill files.
static inline int rdna4x_killed_from_stat_for_app(int statRc, int err, int isAdmittedApp) {
    const int st = rdna4g_kill_state(statRc, err);
    if (st == RDNA4G_KILL_PRESENT) return 1;
    if (st == RDNA4G_KILL_ABSENT) return 0;
    if (isAdmittedApp && (err == EPERM || err == EACCES)) return 0;
    return 1;
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

// <userCacheDir>/org.garnet.xlate
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

// Tier-1 Chromium/Electron sandbox fallback: <userTempDir>/TemporaryItems/org.garnet.xlate[/<K>]
static inline size_t rdna4x_temp_parent_path(char *out, size_t n, const char *userTempDir) {
    if (!out || n == 0) return 0;
    out[0] = 0;
    if (!userTempDir || !userTempDir[0]) return 0;
    const size_t dl = strlen(userTempDir);
    const int w = snprintf(out, n, "%s%sTemporaryItems/" RDNA4X_CACHE_SUB, userTempDir, userTempDir[dl - 1] == '/' ? "" : "/");
    if (w <= 0 || (size_t)w >= n) { out[0] = 0; return 0; }
    return (size_t)w;
}
static inline size_t rdna4x_temp_dir_path(char *out, size_t n, const char *userTempDir, const char *key) {
    if (!out || n == 0) return 0;
    out[0] = 0;
    if (!rdna4x_key_ok(key)) return 0;
    char p[RDNA4X_PATH_MAX];
    if (!rdna4x_temp_parent_path(p, sizeof p, userTempDir)) return 0;
    const int w = snprintf(out, n, "%s/%s", p, key);
    if (w <= 0 || (size_t)w >= n) { out[0] = 0; return 0; }
    return (size_t)w;
}

static inline int rdna4x_mkdir_checked(const char *path) {
    if (mkdir(path, 0700) != 0 && errno != EEXIST) return -1;
    struct stat st;
    if (lstat(path, &st) != 0 || !S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode) || st.st_uid != geteuid()) return -1;
    return 0;
}
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
        if (last ? (lstat(buf, &st) == 0) : (stat(buf, &st) == 0)) {
            if (!S_ISDIR(st.st_mode) || (last && S_ISLNK(st.st_mode))) { buf[i] = c; return -1; }
        } else {
            if (mkdir(buf, 0700) != 0 && errno != EEXIST) { buf[i] = c; return -1; }
            if (lstat(buf, &st) != 0 || !S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode) || st.st_uid != geteuid()) { buf[i] = c; return -1; }
        }
        buf[i] = c;
    }
    return 0;
}
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
// Probe full atomic install (open + write + rename + unlink) inside dir so Seatbelt sandboxes
// that allow stat/EEXIST or create but deny write/rename fall through cleanly to Tier-1 or Tier-2 memory.
static inline int rdna4x_dir_writable(const char *dir) {
    static uint32_t probeSeq = 0;
    const uint32_t seq = __sync_fetch_and_add(&probeSeq, 1u);
    char p1[RDNA4X_PATH_MAX + 64], p2[RDNA4X_PATH_MAX + 64];
    const int n1 = snprintf(p1, sizeof p1, "%s/.garnet-probe-%ld-%u.tmp", dir, (long)getpid(), seq);
    const int n2 = snprintf(p2, sizeof p2, "%s/.garnet-probe-%ld-%u.ok",  dir, (long)getpid(), seq);
    if (n1 <= 0 || (size_t)n1 >= sizeof p1 || n2 <= 0 || (size_t)n2 >= sizeof p2) return 0;
    const int fd = open(p1, O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return 0;
    const char b = '1';
    int ok = (write(fd, &b, 1) == 1);
    if (close(fd) != 0) ok = 0;
    if (!ok || rename(p1, p2) != 0) {
        (void)unlink(p1);
        return 0;
    }
    (void)unlink(p2);
    return 1;
}
static inline int rdna4x_prepare_dirs(const char *parent, const char *dir, const char *key) {
    if (rdna4x_mkdir_p(parent) != 0) return -1;
    if (rdna4x_mkdir_checked(dir) != 0) return -1;
    if (!rdna4x_dir_writable(dir)) return -1;
    (void)rdna4x_remove_old_keys(parent, key);
    return 0;
}

// Two-tier disk directory selection:
//   returns 1 -> Tier-0 primary user cache dir ready in outDir
//   returns 2 -> Tier-1 <userTempDir>/TemporaryItems/org.garnet.xlate/<key> ready in outDir (owner + non-symlink validated)
//   returns 0 -> neither disk dir is writable (caller activates Tier-2 bounded 64 MiB memory cache)
static inline int rdna4x_select_disk_cache(const char *userCacheDir, const char *userTempDir, const char *key,
                                           char *outDir, size_t outDirCap) {
    if (!outDir || outDirCap == 0) return 0;
    outDir[0] = 0;
    if (!rdna4x_key_ok(key)) return 0;
    char parent[RDNA4X_PATH_MAX], dir[RDNA4X_PATH_MAX];
    if (userCacheDir && userCacheDir[0] &&
        rdna4x_parent_path(parent, sizeof parent, userCacheDir) &&
        rdna4x_dir_path(dir, sizeof dir, userCacheDir, key) &&
        rdna4x_prepare_dirs(parent, dir, key) == 0) {
        if (strlen(dir) < outDirCap) { memcpy(outDir, dir, strlen(dir) + 1); return 1; }
    }
    if (userTempDir && userTempDir[0] &&
        rdna4x_temp_parent_path(parent, sizeof parent, userTempDir) &&
        rdna4x_temp_dir_path(dir, sizeof dir, userTempDir, key)) {
        // Also verify TemporaryItems itself is not a symlink before descending into it.
        char tmpItems[RDNA4X_PATH_MAX];
        const size_t dl = strlen(userTempDir);
        const int w = snprintf(tmpItems, sizeof tmpItems, "%s%sTemporaryItems", userTempDir, userTempDir[dl - 1] == '/' ? "" : "/");
        if (w > 0 && (size_t)w < sizeof tmpItems) {
            struct stat tst;
            const int lrc = lstat(tmpItems, &tst);
            const int symlinkBad = (lrc == 0 && S_ISLNK(tst.st_mode));
            if (!symlinkBad && rdna4x_prepare_dirs(parent, dir, key) == 0 && strlen(dir) < outDirCap) {
                memcpy(outDir, dir, strlen(dir) + 1);
                return 2;
            }
        }
    }
    return 0;
}

// ---- Tier-2 bounded LRU process-local in-memory SPIR-V + metadata cache ----
typedef struct rdna4x_mement {
    char sha[65];
    uint8_t *spv;
    size_t spvLen;
    uint8_t *meta;
    size_t metaLen;
    int refused;
    char err[256];
    size_t bytes;
    struct rdna4x_mement *prev, *next;
} rdna4x_mement_t;

typedef struct rdna4x_memcache {
    pthread_mutex_t mu;
    rdna4x_mement_t *head, *tail;   // head = MRU, tail = LRU
    size_t totalBytes;
    size_t capBytes;
    size_t count;
    size_t evictions;
} rdna4x_memcache_t;

static inline void rdna4x_memcache_init(rdna4x_memcache_t *mc, size_t capBytes) {
    memset(mc, 0, sizeof *mc);
    pthread_mutex_init(&mc->mu, NULL);
    mc->capBytes = capBytes ? capBytes : (size_t)RDNA4X_MEMCACHE_CAP;
}

static inline void rdna4x_memcache_unlink_locked(rdna4x_memcache_t *mc, rdna4x_mement_t *e) {
    if (e->prev) e->prev->next = e->next; else mc->head = e->next;
    if (e->next) e->next->prev = e->prev; else mc->tail = e->prev;
    e->prev = e->next = NULL;
}

static inline void rdna4x_memcache_push_head_locked(rdna4x_memcache_t *mc, rdna4x_mement_t *e) {
    e->prev = NULL;
    e->next = mc->head;
    if (mc->head) mc->head->prev = e; else mc->tail = e;
    mc->head = e;
}

static inline void rdna4x_memcache_free_ent(rdna4x_mement_t *e) {
    if (!e) return;
    free(e->spv);
    free(e->meta);
    free(e);
}

static inline void rdna4x_memcache_destroy(rdna4x_memcache_t *mc) {
    if (!mc) return;
    pthread_mutex_lock(&mc->mu);
    rdna4x_mement_t *e = mc->head;
    while (e) {
        rdna4x_mement_t *nx = e->next;
        rdna4x_memcache_free_ent(e);
        e = nx;
    }
    mc->head = mc->tail = NULL;
    mc->totalBytes = 0;
    mc->count = 0;
    pthread_mutex_unlock(&mc->mu);
    pthread_mutex_destroy(&mc->mu);
}

// Returns 1 on positive hit (copies allocated via malloc into non-NULL outSpv/outMeta),
// -1 on negative refusal hit (reason copied to errBuf), 0 on miss.
static inline int rdna4x_memcache_get(rdna4x_memcache_t *mc, const char *sha,
                                      uint8_t **outSpv, size_t *outSpvLen,
                                      uint8_t **outMeta, size_t *outMetaLen,
                                      char *errBuf, size_t errCap) {
    if (outSpv) *outSpv = NULL;
    if (outSpvLen) *outSpvLen = 0;
    if (outMeta) *outMeta = NULL;
    if (outMetaLen) *outMetaLen = 0;
    if (errBuf && errCap) errBuf[0] = 0;
    if (!mc || !rdna4x_sha_ok(sha)) return 0;

    pthread_mutex_lock(&mc->mu);
    rdna4x_mement_t *e = mc->head;
    while (e && strcmp(e->sha, sha) != 0) e = e->next;
    if (!e) { pthread_mutex_unlock(&mc->mu); return 0; }
    if (e != mc->head) {
        rdna4x_memcache_unlink_locked(mc, e);
        rdna4x_memcache_push_head_locked(mc, e);
    }
    if (e->refused) {
        if (errBuf && errCap) snprintf(errBuf, errCap, "%s", e->err);
        pthread_mutex_unlock(&mc->mu);
        return -1;
    }
    uint8_t *sc = NULL, *mcpy = NULL;
    if (outSpv && e->spv && e->spvLen) {
        sc = (uint8_t *)malloc(e->spvLen);
        if (!sc) { pthread_mutex_unlock(&mc->mu); return 0; }
        memcpy(sc, e->spv, e->spvLen);
    }
    if (outMeta && e->meta && e->metaLen) {
        mcpy = (uint8_t *)malloc(e->metaLen);
        if (!mcpy) { free(sc); pthread_mutex_unlock(&mc->mu); return 0; }
        memcpy(mcpy, e->meta, e->metaLen);
    }
    if (outSpv) *outSpv = sc;
    if (outSpvLen) *outSpvLen = e->spvLen;
    if (outMeta) *outMeta = mcpy;
    if (outMetaLen) *outMetaLen = e->metaLen;
    pthread_mutex_unlock(&mc->mu);
    return 1;
}

// Stores a translated SPIR-V + metadata pair, evicting LRU entries to stay within capBytes.
// Rejects items larger than capBytes without evicting existing entries (-1). Returns 0 on success.
static inline int rdna4x_memcache_put(rdna4x_memcache_t *mc, const char *sha,
                                      const uint8_t *spv, size_t spvLen,
                                      const uint8_t *meta, size_t metaLen) {
    if (!mc || !rdna4x_sha_ok(sha) || !spv || !spvLen || !meta || !metaLen) return -1;
    if (spvLen > mc->capBytes || metaLen > mc->capBytes - spvLen) return -1;
    const size_t need = spvLen + metaLen;

    rdna4x_mement_t *ne = (rdna4x_mement_t *)calloc(1, sizeof *ne);
    uint8_t *sc = (uint8_t *)malloc(spvLen);
    uint8_t *mcpy = (uint8_t *)malloc(metaLen);
    if (!ne || !sc || !mcpy) { free(ne); free(sc); free(mcpy); return -1; }
    memcpy(ne->sha, sha, 65);
    memcpy(sc, spv, spvLen);
    memcpy(mcpy, meta, metaLen);
    ne->spv = sc; ne->spvLen = spvLen;
    ne->meta = mcpy; ne->metaLen = metaLen;
    ne->bytes = need;

    pthread_mutex_lock(&mc->mu);
    for (rdna4x_mement_t *old = mc->head; old; old = old->next) {
        if (strcmp(old->sha, sha) == 0) {
            rdna4x_memcache_unlink_locked(mc, old);
            mc->totalBytes -= old->bytes;
            mc->count--;
            rdna4x_memcache_free_ent(old);
            break;
        }
    }
    while (mc->tail && mc->totalBytes + need > mc->capBytes) {
        rdna4x_mement_t *vic = mc->tail;
        rdna4x_memcache_unlink_locked(mc, vic);
        mc->totalBytes -= vic->bytes;
        mc->count--;
        mc->evictions++;
        rdna4x_memcache_free_ent(vic);
    }
    rdna4x_memcache_push_head_locked(mc, ne);
    mc->totalBytes += need;
    mc->count++;
    pthread_mutex_unlock(&mc->mu);
    return 0;
}

static inline int rdna4x_memcache_put_refusal(rdna4x_memcache_t *mc, const char *sha, const char *why) {
    if (!mc || !rdna4x_sha_ok(sha)) return -1;
    const size_t need = 256u;
    if (need > mc->capBytes) return -1;
    rdna4x_mement_t *ne = (rdna4x_mement_t *)calloc(1, sizeof *ne);
    if (!ne) return -1;
    memcpy(ne->sha, sha, 65);
    ne->refused = 1;
    snprintf(ne->err, sizeof ne->err, "%s", why ? why : "refused");
    ne->bytes = need;

    pthread_mutex_lock(&mc->mu);
    for (rdna4x_mement_t *old = mc->head; old; old = old->next) {
        if (strcmp(old->sha, sha) == 0) {
            rdna4x_memcache_unlink_locked(mc, old);
            mc->totalBytes -= old->bytes;
            mc->count--;
            rdna4x_memcache_free_ent(old);
            break;
        }
    }
    while (mc->tail && mc->totalBytes + need > mc->capBytes) {
        rdna4x_mement_t *vic = mc->tail;
        rdna4x_memcache_unlink_locked(mc, vic);
        mc->totalBytes -= vic->bytes;
        mc->count--;
        mc->evictions++;
        rdna4x_memcache_free_ent(vic);
    }
    rdna4x_memcache_push_head_locked(mc, ne);
    mc->totalBytes += need;
    mc->count++;
    pthread_mutex_unlock(&mc->mu);
    return 0;
}

#endif /* RDNA4_XLATE_FALLBACK_H */
