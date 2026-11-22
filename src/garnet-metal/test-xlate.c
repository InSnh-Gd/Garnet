// test-xlate.c: rdna4_xlate.h (bundle 14, in-process shader translation; an internal design note section 1) on the host, plus source pins that tie the REAL GarnetDevice.m to it.
// Build/run on the host Mac (from this directory; argv[1] = the directory holding GarnetDevice.m and rdna4_xlate.h, default "."; argv[2] = autotranslate/overrides.txt, default ../../tools/native/autotranslate/overrides.txt):
//   cc -O1 -Wall -Wextra -Werror -DRDNA4X_TESTHOOKS -o /tmp/test-xlate test-xlate.c && /tmp/test-xlate .
// test-b14-plant.sh plants breaks in the REAL rdna4_xlate.h, GarnetDevice.m and the fork's Rust and demands that this suite (or the fork's cargo tests) FAIL on each.
// Covers:
//   T1  every combination of rdna4x_applies; the kill-file and environment readers; the side-directory rule; the cache key (every part of it matters) and the path builders; the size caps; the crash-marker
//       plan table and the real marker files (create, probe, promote, sweep); the least-recently-used trim (the pure decision and the real directory); the override table against overrides.txt;
//       the Mach-O UUID reader; the reader's candidate order; the worker thread's stack; one job's order of operations (marker, meta before spv, .fail, caps); the engine (timeout, sharing, late install)
//   T4  crash guard: a child process that dies mid-translation (the test-only _exit hook) leaves the marker; the next run skips the sha and never calls the translator; a new key retries
//   G   source pins on GarnetDevice.m: the gate call, the lookup order, the side-directory skip, the worker stack, the order of the miss branches, the LINKAGE IGNORED lines
// (T2/T3/T5 are the fork's cargo tests and test-xlate-corpus.sh.)
#define _DARWIN_C_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <mach-o/dyld.h>
#ifndef RDNA4X_TESTHOOKS
#error "test-xlate requires -DRDNA4X_TESTHOOKS (see the build line at the top)"
#endif
#include "rdna4_xlate.h"

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

static char TMP[512];   // scratch root, removed at exit (prefix-checked)
static void rmtree(const char *path) {   // plain files and directories only; never follows a link
    struct stat st; if (lstat(path, &st) != 0) return;
    if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
        DIR *d = opendir(path); struct dirent *e;
        if (d) { while ((e = readdir(d)) != NULL) { if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue; char p[1400]; snprintf(p, sizeof p, "%s/%s", path, e->d_name); rmtree(p); } closedir(d); }
        rmdir(path);
    } else unlink(path);
}
static void cleanup(void) { if (strstr(TMP, "/rdna4xlate-test.") && strlen(TMP) > 20) rmtree(TMP); }
static void mkd(const char *p) { mkdir(p, 0700); }
static void touch_file(const char *p, const char *content) { FILE *f = fopen(p, "wb"); if (f) { fputs(content, f); fclose(f); } }
static void set_mtime(const char *p, long secs) { struct timeval tv[2]; tv[0].tv_sec = tv[1].tv_sec = secs; tv[0].tv_usec = tv[1].tv_usec = 0; utimes(p, tv); }
static const char SHA_A[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static const char SHA_B[] = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
static const char SHA_C[] = "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
static const char SHA_D[] = "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";

// ---- fake dependencies ----
static struct {
    int airCalls, xlCalls, relCalls, freeCalls;
    int readerFails, translateRc, bigText, bigSpv, delayMs, markerSeenInReader, markerSeenInXlate;
    const char *dir, *sha; size_t stackSeen;
} FK;
static void fk_reset(void) { memset(&FK, 0, sizeof FK); }
static int fk_air(void *c, const uint8_t *air, size_t n, char **t, size_t *l, char *e, size_t en) {
    (void)c; (void)air; (void)n; FK.airCalls++;
    FK.stackSeen = pthread_get_stacksize_np(pthread_self());
    if (FK.dir && FK.sha) { char p[1100]; rdna4x_file_path(p, sizeof p, FK.dir, FK.sha, RDNA4X_EXT_INFLIGHT); if (rdna4x_exists(p)) FK.markerSeenInReader = 1; }
    if (FK.delayMs) usleep((useconds_t)FK.delayMs * 1000);
    if (FK.readerFails) { snprintf(e, en, "fake reader refused"); return -1; }
    *t = strdup("; fake ll\n"); *l = FK.bigText ? (size_t)RDNA4X_MAX_LL + 1 : strlen(*t); return 0;
}
static void fk_free_text(void *c, char *t) { (void)c; FK.freeCalls++; free(t); }
static int fk_xl(void *c, const char *ll, size_t llLen, const uint32_t *local, uint8_t **spv, size_t *sl, uint8_t **meta, size_t *ml, char *e, size_t en) {
    (void)c; (void)ll; (void)llLen; (void)local; FK.xlCalls++;
    if (FK.dir && FK.sha) { char p[1100]; rdna4x_file_path(p, sizeof p, FK.dir, FK.sha, RDNA4X_EXT_INFLIGHT); if (rdna4x_exists(p)) FK.markerSeenInXlate = 1; }
    if (FK.translateRc) { snprintf(e, en, "fake translator rc %d", FK.translateRc); return FK.translateRc; }
    *sl = FK.bigSpv ? (size_t)RDNA4X_MAX_SPV + 4 : 64; *spv = calloc(1, *sl ? *sl : 1); if (FK.bigSpv) *sl = (size_t)RDNA4X_MAX_SPV + 4;
    (*spv)[0] = 3; (*spv)[1] = 2; (*spv)[2] = 0x23; (*spv)[3] = 7;
    const char *m = "{\"stage\":\"Kernel\"}"; *ml = strlen(m); *meta = malloc(*ml); memcpy(*meta, m, *ml);
    return 0;
}
static void fk_release(void *c, uint8_t *p, size_t n) { (void)c; (void)n; FK.relCalls++; free(p); }
static const rdna4x_deps FDEPS = { .air_to_text = fk_air, .free_text = fk_free_text,
    .translate = fk_xl, .release = fk_release };
static int fk_job(const char *dir, const char *sha, const char *name, char *err, size_t en) {
    static const uint8_t air[16] = "BC\xc0\xde-----------";
    return rdna4x_translate_job(&FDEPS, dir, sha, name, air, sizeof air, err, en);
}
static int file_exists(const char *dir, const char *sha, const char *ext) { char p[1100]; return rdna4x_file_path(p, sizeof p, dir, sha, ext) && rdna4x_exists(p); }

// ---- the worker-stack probe (T1: the real engine must hand its worker 64 MiB) ----
static int stack_air(void *c, const uint8_t *air, size_t n, char **t, size_t *l, char *e, size_t en) { FK.stackSeen = pthread_get_stacksize_np(pthread_self()); return fk_air(c, air, n, t, l, e, en); }

struct share { rdna4x_engine *e; const char *d; int rc; };
static void *share_thread(void *a) {   // a second caller, shortly after the first, asking for the same sha
    struct share *sp = (struct share *)a; static const uint8_t air2[16] = "BC\xc0\xde-----------"; char er[100];
    usleep(60000); sp->rc = rdna4x_engine_run(sp->e, sp->d, SHA_D, "k", air2, sizeof air2, 5000, er, sizeof er); return NULL;
}
static size_t seen_stack;
static void *probe_stack(void *a) { (void)a; seen_stack = pthread_get_stacksize_np(pthread_self()); return NULL; }

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    char ovrPath[1024]; snprintf(ovrPath, sizeof ovrPath, "%s", argc > 2 ? argv[2] : "../../tools/native/autotranslate/overrides.txt");
    {   // scratch root: under $TMPDIR (or /private/tmp)
        const char *base = getenv("RDNA4_TEST_TMP"); if (!base) base = getenv("TMPDIR"); if (!base || !*base) base = "/private/tmp";
        snprintf(TMP, sizeof TMP, "%s/rdna4xlate-test.XXXXXX", base);
        if (!mkdtemp(TMP)) { printf("cannot create a scratch dir under %s\n", base); return 2; }
        atexit(cleanup);
    }

    // ===== T1.1 the gate: every combination =====
    {
        int bad = 0, n = 0;
        for (int m = 0; m < 64; m++) {
            const int isWs = m & 1, admitted = (m >> 1) & 1, rootAllow = (m >> 2) & 1, testInproc = (m >> 3) & 1, killed = (m >> 4) & 1, envDirty = (m >> 5) & 1;
            int want;   // written out as a table, not as the implementation's expression
            if (isWs) want = 0; else if (killed) want = 0; else if (envDirty) want = 0; else if (rootAllow) want = testInproc; else want = admitted;
            if (rdna4x_applies(isWs, admitted, rootAllow, testInproc, killed, envDirty) != want) bad++;
            n++;
        }
        CHECK("rdna4x_applies: all 64 combinations match the table", bad == 0 && n == 64);
        CHECK("rdna4x_applies: WindowServer is never in-process, whatever else is set", !rdna4x_applies(1, 1, 1, 1, 0, 0) && !rdna4x_applies(1, 1, 0, 0, 0, 0) && !rdna4x_applies(1, 0, 1, 1, 0, 0));
        CHECK("rdna4x_applies: an unadmitted ordinary process is not in-process", !rdna4x_applies(0, 0, 0, 0, 0, 0) && !rdna4x_applies(0, 0, 0, 1, 0, 0));
        CHECK("rdna4x_applies: an admitted application is", rdna4x_applies(0, 1, 0, 0, 0, 0));
        CHECK("rdna4x_applies: a root tool needs RDNA4M_TEST_INPROC", !rdna4x_applies(0, 0, 1, 0, 0, 0) && rdna4x_applies(0, 0, 1, 1, 0, 0));
        CHECK("rdna4x_applies: the kill file turns an admitted application off", !rdna4x_applies(0, 1, 0, 0, 1, 0) && !rdna4x_applies(0, 0, 1, 1, 1, 0));
        CHECK("rdna4x_applies: a METAL2VULKAN_ variable turns an admitted application off", !rdna4x_applies(0, 1, 0, 0, 0, 1) && !rdna4x_applies(0, 0, 1, 1, 0, 1));
    }
    {
        char *e0[] = { NULL };
        char *e1[] = { (char *)"PATH=/usr/bin", (char *)"HOME=/x", NULL };
        char *e2[] = { (char *)"PATH=/usr/bin", (char *)"METAL2VULKAN_WHOLE_PART=1", NULL };
        char *e3[] = { (char *)"METAL2VULKAN", (char *)"XMETAL2VULKAN_A=1", (char *)"metal2vulkan_a=1", NULL };
        char *e4[] = { (char *)"METAL2VULKAN_", NULL };
        CHECK("env: nothing / unrelated variables are clean", !rdna4x_env_dirty(e0) && !rdna4x_env_dirty(e1) && !rdna4x_env_dirty(NULL));
        CHECK("env: a METAL2VULKAN_ variable is dirty, wherever it sits", rdna4x_env_dirty(e2) && rdna4x_env_dirty(e4));
        CHECK("env: near misses (no underscore, a prefix, lower case) are clean", !rdna4x_env_dirty(e3));
        CHECK("kill file: absent (ENOENT / ENOTDIR) is not killed", !rdna4x_killed_from_stat(-1, ENOENT) && !rdna4x_killed_from_stat(-1, ENOTDIR));
        CHECK("kill file: present is killed", rdna4x_killed_from_stat(0, 0) == 1);
        CHECK("kill file: unreadable (EPERM / EACCES / other) is killed, fail closed", rdna4x_killed_from_stat(-1, EPERM) && rdna4x_killed_from_stat(-1, EACCES) && rdna4x_killed_from_stat(-1, EIO));
        CHECK("side directory: skipped for a sandboxed application, kept for WindowServer and for unsandboxed processes",
              rdna4x_skip_side_dir(0, 1) == 1 && rdna4x_skip_side_dir(1, 1) == 0 && rdna4x_skip_side_dir(0, 0) == 0 && rdna4x_skip_side_dir(1, 0) == 0);
    }

    // ===== T1.2 the cache key =====
    {
        uint8_t xl[16], rd[16], ov[32]; memset(xl, 1, 16); memset(rd, 2, 16); rdna4x_override_hash(ov);
        char k0[17], k1[17], k2[17], k3[17], k4[17], k5[17];
        rdna4x_key(k0, xl, rd, ov, "26A434"); rdna4x_key(k1, xl, rd, ov, "26A434");
        CHECK("key: deterministic, 16 lowercase hex digits", !strcmp(k0, k1) && rdna4x_key_ok(k0) && strlen(k0) == 16);
        uint8_t xl2[16]; memcpy(xl2, xl, 16); xl2[15] ^= 1; rdna4x_key(k2, xl2, rd, ov, "26A434");
        uint8_t rd2[16]; memcpy(rd2, rd, 16); rd2[7] ^= 0x80; rdna4x_key(k3, xl, rd2, ov, "26A434");
        uint8_t ov2[32]; memcpy(ov2, ov, 32); ov2[0] ^= 1; rdna4x_key(k4, xl, rd, ov2, "26A434");
        rdna4x_key(k5, xl, rd, ov, "26A435");
        CHECK("key: a new translator build (LC_UUID of libgarnetxlate) is a new key", strcmp(k0, k2) != 0);
        CHECK("key: a new reader library (its UUID) is a new key", strcmp(k0, k3) != 0);
        CHECK("key: a changed override table is a new key", strcmp(k0, k4) != 0);
        CHECK("key: a new OS build (kern.osversion) is a new key", strcmp(k0, k5) != 0);
        char k6[17], k7[17]; uint8_t z[16]; memset(z, 0, 16); rdna4x_key(k6, xl, z, ov, "26A434"); rdna4x_key(k7, z, rd, ov, "26A434");
        CHECK("key: the translator and the reader identities are not interchangeable", strcmp(k6, k7) != 0 && strcmp(k6, k0) != 0);
        uint8_t ov3[32]; rdna4x_override_hash(ov3);
        CHECK("key: the override hash is stable and non-zero", !memcmp(ov, ov3, 32) && memcmp(ov, z, 16) != 0);
        CHECK("key: a NULL osversion is the empty string, not a crash", ({ char kk[17]; rdna4x_key(kk, xl, rd, ov, NULL); rdna4x_key_ok(kk); }));
        CC_SHA256_CTX old; CC_SHA256_Init(&old);
        CC_SHA256_Update(&old, "rdna4x-1", sizeof "rdna4x-1");
        CC_SHA256_Update(&old, xl, 16); CC_SHA256_Update(&old, rd, 16);
        CC_SHA256_Update(&old, ov, 32); CC_SHA256_Update(&old, "26A434", 6);
        uint8_t oldDigest[32]; char oldKey[17]; CC_SHA256_Final(oldDigest, &old); rdna4x_hex(oldKey, oldDigest, 8);
        CHECK("key: old address-only generation cannot reuse a BDA64 translation", strcmp(k0, oldKey) != 0);
        CHECK("rdna4x_key_ok: rejects upper case, short, long, non-hex", !rdna4x_key_ok("ABCDEF0123456789") && !rdna4x_key_ok("abc") && !rdna4x_key_ok("0123456789abcdef0") && !rdna4x_key_ok("0123456789abcdeg") && !rdna4x_key_ok(NULL));
    }

    // ===== T1.3 paths =====
    {
        char p[1100], q[1100];
        CHECK("parent path: <ucd>/org.garnet.xlate (ucd ends with '/')", rdna4x_parent_path(p, sizeof p, "/var/folders/xx/C/") && !strcmp(p, "/var/folders/xx/C/org.garnet.xlate"));
        CHECK("parent path: a user cache dir without the slash gets one", rdna4x_parent_path(p, sizeof p, "/var/folders/xx/C") && !strcmp(p, "/var/folders/xx/C/org.garnet.xlate"));
        CHECK("parent path: no user cache dir -> none", rdna4x_parent_path(p, sizeof p, NULL) == 0 && rdna4x_parent_path(p, sizeof p, "") == 0 && p[0] == 0);
        CHECK("dir path: <ucd>/org.garnet.xlate/<K>", rdna4x_dir_path(p, sizeof p, "/c/", "0123456789abcdef") && !strcmp(p, "/c/org.garnet.xlate/0123456789abcdef"));
        CHECK("dir path: a bad key makes no path", rdna4x_dir_path(p, sizeof p, "/c/", "nothex") == 0 && p[0] == 0);
        CHECK("dir path: too small a buffer -> none", rdna4x_dir_path(q, 12, "/c/", "0123456789abcdef") == 0 && q[0] == 0);
        CHECK("file path: <dir>/<sha><ext> for each extension", rdna4x_file_path(p, sizeof p, "/d", SHA_A, RDNA4X_EXT_SPV) && !strcmp(p + strlen(p) - 4, ".spv") && strstr(p, "/d/aaaa") == p &&
              rdna4x_file_path(p, sizeof p, "/d", SHA_A, RDNA4X_EXT_META) && !strcmp(p + strlen(p) - 10, ".meta.json") &&
              rdna4x_file_path(p, sizeof p, "/d", SHA_A, RDNA4X_EXT_FAIL) && rdna4x_file_path(p, sizeof p, "/d", SHA_A, RDNA4X_EXT_INFLIGHT) && rdna4x_file_path(p, sizeof p, "/d", SHA_A, RDNA4X_EXT_CRASH));
        CHECK("file path: a path-traversal or short sha makes no path", rdna4x_file_path(p, sizeof p, "/d", "../../etc/passwd", ".spv") == 0 && rdna4x_file_path(p, sizeof p, "/d", "abc", ".spv") == 0 && rdna4x_file_path(p, sizeof p, "/d", NULL, ".spv") == 0);
        char upper[65]; memset(upper, 'A', 64); upper[64] = 0;
        CHECK("sha_ok: lower-case hex of exactly 64", rdna4x_sha_ok(SHA_A) && !rdna4x_sha_ok(upper) && !rdna4x_sha_ok("") && !rdna4x_sha_ok(NULL));
    }

    // ===== T1.4 caps =====
    CHECK("caps: bitcode 4 bytes .. 4 MiB", !rdna4x_air_size_ok(0) && !rdna4x_air_size_ok(3) && rdna4x_air_size_ok(4) && rdna4x_air_size_ok(4u << 20) && !rdna4x_air_size_ok((4u << 20) + 1));
    CHECK("caps: LLVM text 1 .. 64 MiB", !rdna4x_ll_size_ok(0) && rdna4x_ll_size_ok(1) && rdna4x_ll_size_ok(64u << 20) && !rdna4x_ll_size_ok((64u << 20) + 1));
    CHECK("caps: SPIR-V >= 20 bytes, word aligned, <= 16 MiB", !rdna4x_spv_size_ok(16) && !rdna4x_spv_size_ok(22) && rdna4x_spv_size_ok(20) && rdna4x_spv_size_ok(16u << 20) && !rdna4x_spv_size_ok((16u << 20) + 4));
    CHECK("caps: the documented sizes", RDNA4X_MAX_AIR == 4u << 20 && RDNA4X_MAX_LL == 64u << 20 && RDNA4X_MAX_SPV == 16u << 20 && RDNA4X_CACHE_CAP == 256ull << 20 && RDNA4X_WORKER_STACK == (size_t)64u << 20);

    // ===== T1.5 the plan table =====
    {
        int bad = 0;
        for (int m = 0; m < 16; m++) {
            const int crash = m & 1, fail = (m >> 1) & 1, infl = (m >> 2) & 1, alive = (m >> 3) & 1;
            int want;   // the order of precedence, written out
            if (crash) want = RDNA4X_PL_SKIP_CRASH;
            else if (infl && alive) want = RDNA4X_PL_SKIP_LIVE;
            else if (infl && !alive) want = RDNA4X_PL_PROMOTE_THEN_SKIP;
            else if (fail) want = RDNA4X_PL_SKIP_FAIL;
            else want = RDNA4X_PL_TRANSLATE;
            if (rdna4x_plan(crash, fail, infl, alive) != want) bad++;
        }
        CHECK("plan: all 16 combinations match the table (crash beats everything; a dead marker is promoted; a live one is skipped)", bad == 0);
        CHECK("plan: a .crash is never translated again", rdna4x_plan(1, 0, 0, 0) == RDNA4X_PL_SKIP_CRASH && rdna4x_plan(1, 1, 1, 1) == RDNA4X_PL_SKIP_CRASH);
    }

    // ===== T1.6 real marker files =====
    char D[1024]; snprintf(D, sizeof D, "%s/k1", TMP); mkd(D);
    {
        CHECK("marker: create -> 0, again (O_EXCL) -> 1", rdna4x_marker_create(D, SHA_A) == 0 && rdna4x_marker_create(D, SHA_A) == 1);
        char p[1100]; rdna4x_file_path(p, sizeof p, D, SHA_A, RDNA4X_EXT_INFLIGHT);
        CHECK("marker: holds this process id", rdna4x_marker_pid(p) == (long)getpid());
        struct stat st; stat(p, &st);
        CHECK("marker: mode 0600", (st.st_mode & 0777) == 0600);
        CHECK("probe: a marker of a live process (this one) -> skip, left in place", rdna4x_probe(D, SHA_A) == RDNA4X_PL_SKIP_LIVE && rdna4x_exists(p));
        rdna4x_marker_remove(D, SHA_A);
        CHECK("marker: remove", !rdna4x_exists(p) && rdna4x_probe(D, SHA_A) == RDNA4X_PL_TRANSLATE);
        // a dead process's marker
        pid_t c = fork(); if (c == 0) _exit(0);
        int stt = 0; waitpid(c, &stt, 0);
        char pid[32]; snprintf(pid, sizeof pid, "%d\n", (int)c); touch_file(p, pid);
        CHECK("pid_alive: a reaped child is dead, this process is alive, junk is dead", !rdna4x_pid_alive((long)c) && rdna4x_pid_alive((long)getpid()) && !rdna4x_pid_alive(-1) && !rdna4x_pid_alive(0));
        CHECK("probe: a marker of a DEAD process is promoted to .crash and the sha is skipped", rdna4x_probe(D, SHA_A) == RDNA4X_PL_PROMOTE_THEN_SKIP && file_exists(D, SHA_A, RDNA4X_EXT_CRASH) && !file_exists(D, SHA_A, RDNA4X_EXT_INFLIGHT));
        CHECK("probe: afterwards the sha stays skipped (the .crash)", rdna4x_probe(D, SHA_A) == RDNA4X_PL_SKIP_CRASH);
        touch_file(p, "not a number");   // reuse: an unreadable marker counts as dead
        rdna4x_file_path(p, sizeof p, D, SHA_B, RDNA4X_EXT_INFLIGHT); touch_file(p, "junk\n");
        CHECK("probe: a marker with no pid counts as dead -> promoted", rdna4x_probe(D, SHA_B) == RDNA4X_PL_PROMOTE_THEN_SKIP && file_exists(D, SHA_B, RDNA4X_EXT_CRASH));
        rdna4x_file_path(p, sizeof p, D, SHA_C, RDNA4X_EXT_FAIL); touch_file(p, "refused");
        CHECK("probe: a .fail is skipped", rdna4x_probe(D, SHA_C) == RDNA4X_PL_SKIP_FAIL);
        CHECK("probe: an unusable sha is never translated", rdna4x_probe(D, "zz") == RDNA4X_PL_SKIP_CRASH);
    }
    {   // the sweep at the first miss
        char sd[1024]; snprintf(sd, sizeof sd, "%s/k2", TMP); mkd(sd);
        pid_t c = fork(); if (c == 0) _exit(0);
        int stt = 0; waitpid(c, &stt, 0);
        char p[1100], pid[32]; snprintf(pid, sizeof pid, "%d\n", (int)c);
        rdna4x_file_path(p, sizeof p, sd, SHA_A, RDNA4X_EXT_INFLIGHT); touch_file(p, pid);          // dead
        rdna4x_marker_create(sd, SHA_B);                                                          // live (this process)
        CHECK("sweep: dead markers become .crash, live ones stay, the count is right", rdna4x_sweep(sd) == 1 && file_exists(sd, SHA_A, RDNA4X_EXT_CRASH) && !file_exists(sd, SHA_A, RDNA4X_EXT_INFLIGHT) && file_exists(sd, SHA_B, RDNA4X_EXT_INFLIGHT));
        rdna4x_marker_remove(sd, SHA_B);
    }

    // ===== T1.7 trim: the pure decision and the real directory =====
    {
        rdna4x_ent e[5]; unsigned char del[5];
        memset(e, 0, sizeof e);
        for (int i = 0; i < 5; i++) { snprintf(e[i].sha, 65, "%064d", i); e[i].bytes = 100; e[i].mtime = 1000 + (i == 2 ? -500 : i); }   // entry 2 is the oldest
        CHECK("trim: within the cap nothing goes", rdna4x_trim_select(e, 5, 500, del) == 0 && !del[0] && !del[1] && !del[2] && !del[3] && !del[4]);
        CHECK("trim: over the cap the OLDEST go first, down to 80% of the cap", rdna4x_trim_select(e, 5, 450, del) == 2 && del[2] == 1 && del[0] == 1 && !del[1] && !del[3] && !del[4]);   // 500 -> 360 (80% of 450): remove 2 oldest = 200 -> 300
        e[1].bytes = 1000;
        CHECK("trim: one large entry is enough", rdna4x_trim_select(e, 5, 1000, del) >= 1);
        for (int i = 0; i < 5; i++) { e[i].bytes = 100; e[i].mtime = 7; }
        CHECK("trim: equal times break ties by position (deterministic)", rdna4x_trim_select(e, 5, 400, del) == 2 && del[0] == 1 && del[1] == 1 && !del[2]);
        CHECK("trim: an empty list is fine", rdna4x_trim_select(e, 0, 0, del) == 0);
        CHECK("trim: the target is 80% of the cap exactly", ({ rdna4x_ent t[10]; unsigned char dl[10]; memset(t, 0, sizeof t); for (int i = 0; i < 10; i++) { snprintf(t[i].sha, 65, "%064d", i); t[i].bytes = 10; t[i].mtime = i; } rdna4x_trim_select(t, 10, 90, dl) == 3; }));   // 100 > 90 -> target 72 -> 3 removed -> 70
    }
    {
        char td[1024]; snprintf(td, sizeof td, "%s/k3", TMP); mkd(td);
        const char *shas[4] = { SHA_A, SHA_B, SHA_C, SHA_D };
        for (int i = 0; i < 4; i++) {
            char p[1100]; char big[1001]; memset(big, 'x', 1000); big[1000] = 0;
            rdna4x_file_path(p, sizeof p, td, shas[i], RDNA4X_EXT_SPV); touch_file(p, big); set_mtime(p, 1000 + i * 10);
            rdna4x_file_path(p, sizeof p, td, shas[i], RDNA4X_EXT_META); touch_file(p, big); set_mtime(p, 1000 + i * 10);
        }
        char p[1100]; rdna4x_file_path(p, sizeof p, td, SHA_A, RDNA4X_EXT_CRASH); touch_file(p, "");   // a marker: never counted, never removed
        rdna4x_file_path(p, sizeof p, td, SHA_A, RDNA4X_EXT_SPV);
        struct timeval read_recently[2] = { { 1040, 0 }, { 1000, 0 } };
        CHECK("trim_dir: test fixture records a recent access on only the SPIR-V", utimes(p, read_recently) == 0);
        const size_t removed = rdna4x_trim_dir(td, 6500);
        CHECK("trim_dir: recent read keeps both files of the oldest install", removed == 2 &&
              file_exists(td, SHA_A, RDNA4X_EXT_SPV) && file_exists(td, SHA_A, RDNA4X_EXT_META) &&
              !file_exists(td, SHA_B, RDNA4X_EXT_SPV) && !file_exists(td, SHA_C, RDNA4X_EXT_SPV) &&
              file_exists(td, SHA_D, RDNA4X_EXT_META));
        CHECK("trim_dir: a .crash marker is never removed", file_exists(td, SHA_A, RDNA4X_EXT_CRASH));
        CHECK("trim_dir: within the cap it removes nothing", rdna4x_trim_dir(td, 1u << 20) == 0 && file_exists(td, SHA_D, RDNA4X_EXT_SPV));
    }

    // ===== T1.8 directory preparation and old keys =====
    {
        char par[1024], k1[1100], k2[1100], other[1100]; snprintf(par, sizeof par, "%s/cache/" RDNA4X_CACHE_SUB, TMP);
        char ucd[1024]; snprintf(ucd, sizeof ucd, "%s/cache", TMP); mkd(ucd);
        snprintf(k1, sizeof k1, "%s/1111111111111111", par); snprintf(k2, sizeof k2, "%s/2222222222222222", par);
        CHECK("prepare: creates parent and key dir (0700)", rdna4x_prepare_dirs(par, k1, "1111111111111111") == 0 && ({ struct stat st; stat(k1, &st); (st.st_mode & 0777) == 0700; }));
        char f[1200]; snprintf(f, sizeof f, "%s/%s.spv", k1, SHA_A); touch_file(f, "old");
        CHECK("prepare: a new key removes the old key directory with its files", rdna4x_prepare_dirs(par, k2, "2222222222222222") == 0 && !rdna4x_exists(k1) && rdna4x_exists(k2));
        snprintf(f, sizeof f, "%s/%s.spv", k2, SHA_A); touch_file(f, "current");
        CHECK("prepare: the current key's files survive a second prepare", rdna4x_prepare_dirs(par, k2, "2222222222222222") == 0 && rdna4x_exists(f));
        // a symlink planted as an old key, pointing at a directory with a file: never followed
        snprintf(other, sizeof other, "%s/elsewhere", TMP); mkd(other);
        char vf[1200]; snprintf(vf, sizeof vf, "%s/precious", other); touch_file(vf, "keep");
        char lnk[1200]; snprintf(lnk, sizeof lnk, "%s/3333333333333333", par);
        CHECK("symlink planted as an old key", symlink(other, lnk) == 0);
        (void)rdna4x_remove_old_keys(par, "2222222222222222");
        CHECK("remove_old_keys: a symlink is not followed and its target is untouched", rdna4x_exists(vf));
        unlink(lnk);
        // a symlink planted as the parent is refused
        char par2[1100]; snprintf(par2, sizeof par2, "%s/cache2", TMP); symlink(other, par2);
        CHECK("mkdir_checked: a symlink in place of the directory is refused", rdna4x_mkdir_checked(par2) != 0);
        unlink(par2);
        CHECK("mkdir_checked: a regular file in place of the directory is refused", ({ char pf[1100]; snprintf(pf, sizeof pf, "%s/afile", TMP); touch_file(pf, "x"); rdna4x_mkdir_checked(pf) != 0; }));
    }

    // ===== T1.9 the override table =====
    {
        char *txt = slurp(".", ovrPath);
        if (!txt) { char alt[1100]; snprintf(alt, sizeof alt, "%s/%s", dir, ovrPath); txt = slurp(".", alt); }
        CHECK("overrides.txt is readable", txt != NULL);
        size_t found = 0, lines = 0; int allMatch = 1;
        if (txt) {
            for (char *l = strtok(txt, "\n"); l; l = strtok(NULL, "\n")) {
                while (*l == ' ' || *l == '\t') l++;
                if (!*l || *l == '#') continue;
                char *eq = strchr(l, '='); if (!eq) continue;
                *eq = 0; unsigned a, b, c;
                lines++;
                if (sscanf(eq + 1, "%u,%u,%u", &a, &b, &c) != 3) { allMatch = 0; continue; }
                const uint32_t *o = rdna4x_local_for(l);
                if (!o || o[0] != a || o[1] != b || o[2] != c) allMatch = 0; else found++;
            }
            free(txt);
        }
        CHECK("override table: every line of overrides.txt is in the table with the same size, and the table has no extra", allMatch && found == lines && lines == RDNA4X_NOVERRIDES);
        CHECK("override table: an unlisted kernel gets none", rdna4x_local_for("some_kernel") == NULL && rdna4x_local_for(NULL) == NULL && rdna4x_local_for("") == NULL);
    }

    // ===== T1.10 the Mach-O UUID reader =====
    {
        uint8_t a[16], b[16], mem[16]; char exe[1024]; uint32_t sz = sizeof exe; _NSGetExecutablePath(exe, &sz);
        const int f = rdna4x_file_uuid(exe, a);
        const int m = rdna4x_macho_uuid(_dyld_get_image_header(0), 65536, mem);
        CHECK("macho uuid: a thin 64-bit executable has one, and the file's equals the loaded image's", f == 1 && m == 1 && !memcmp(a, mem, 16));
        CHECK("macho uuid: not Mach-O / short -> 0", rdna4x_macho_uuid("abcdefghijklmnopqrstuvwx", 24, b) == 0 && rdna4x_macho_uuid(mem, 4, b) == 0 && rdna4x_file_uuid("/nonexistent/xx", b) == 0);
        uint8_t hdr[256]; memset(hdr, 0, sizeof hdr); struct mach_header_64 *h = (struct mach_header_64 *)hdr; h->magic = MH_MAGIC_64; h->ncmds = 1; h->sizeofcmds = 24;
        struct uuid_command *u = (struct uuid_command *)(hdr + sizeof *h); u->cmd = LC_UUID; u->cmdsize = 24; for (int i = 0; i < 16; i++) u->uuid[i] = (uint8_t)(i + 1);
        CHECK("macho uuid: a synthetic header", rdna4x_macho_uuid(hdr, sizeof hdr, b) == 1 && b[0] == 1 && b[15] == 16);
        u->cmdsize = 4000;
        CHECK("macho uuid: a load command that runs past the header is refused", rdna4x_macho_uuid(hdr, sizeof hdr, b) == 0);
    }

    // ===== T1.11 the reader's candidate order =====
    {
        char base[1024]; snprintf(base, sizeof base, "%s/Versions", TMP); mkd(base);
        const char *names[] = { "A", "32023", "Current", "31000", "abc", "5", "99999999999999999999" };
        for (size_t i = 0; i < sizeof names / sizeof *names; i++) { char p[1100]; snprintf(p, sizeof p, "%s/%s", base, names[i]); mkd(p); }
        static char cand[16][RDNA4X_PATH_MAX];
        const size_t n = rdna4x_reader_candidates(base, cand, 16);
        char want0[1100], want1[1100], want2[1100], want3[1100]; snprintf(want0, sizeof want0, "%s/Current/Libraries/libLLVM.dylib", base); snprintf(want1, sizeof want1, "%s/32023/Libraries/libLLVM.dylib", base);
        snprintf(want2, sizeof want2, "%s/31000/Libraries/libLLVM.dylib", base); snprintf(want3, sizeof want3, "%s/5/Libraries/libLLVM.dylib", base);
        CHECK("reader candidates: Current first, then the numeric version directories, highest first; non-numeric names are ignored", n == 4 && !strcmp(cand[0], want0) && !strcmp(cand[1], want1) && !strcmp(cand[2], want2) && !strcmp(cand[3], want3));
        rdna4x_reader r; char err[200];
        CHECK("reader: no usable library -> a clean failure, not a crash", rdna4x_reader_load(&r, base, NULL, err, sizeof err) != 0 && err[0]);
        CHECK("reader: an override path that is not a library -> a clean failure", rdna4x_reader_load(&r, base, "/nonexistent/libLLVM.dylib", err, sizeof err) != 0);
    }

    // ===== T4 the crash guard, BEFORE any thread exists =====
    {
        char cd[1024]; snprintf(cd, sizeof cd, "%s/k4", TMP); mkd(cd);
        fk_reset(); FK.dir = cd; FK.sha = SHA_A;
        pid_t c = fork();
        if (c == 0) {   // the child: translate with the test-only exit hook set: it dies right after the marker is written
            setenv("RDNA4X_TEST_EXIT_AFTER_MARKER", "1", 1);
            char err[100]; (void)fk_job(cd, SHA_A, "k", err, sizeof err);
            _exit(0);   // never reached when the hook works
        }
        int stt = 0; waitpid(c, &stt, 0);
        CHECK("T4: the child died mid-translation (the test hook's exit code)", WIFEXITED(stt) && WEXITSTATUS(stt) == 77);
        CHECK("T4: it left its marker behind", file_exists(cd, SHA_A, RDNA4X_EXT_INFLIGHT) && !file_exists(cd, SHA_A, RDNA4X_EXT_SPV) && !file_exists(cd, SHA_A, RDNA4X_EXT_META));
        char err[256]; fk_reset();
        int rc = fk_job(cd, SHA_A, "k", err, sizeof err);
        CHECK("T4: the next run skips that sha (E_SKIP_CRASH) and never calls the reader or the translator", rc == RDNA4X_E_SKIP_CRASH && FK.airCalls == 0 && FK.xlCalls == 0);
        CHECK("T4: the dead marker became .crash", file_exists(cd, SHA_A, RDNA4X_EXT_CRASH) && !file_exists(cd, SHA_A, RDNA4X_EXT_INFLIGHT));
        fk_reset(); rc = fk_job(cd, SHA_A, "k", err, sizeof err);
        CHECK("T4: and again on the run after that", rc == RDNA4X_E_SKIP_CRASH && FK.xlCalls == 0);
        fk_reset(); rc = fk_job(cd, SHA_B, "k", err, sizeof err);
        CHECK("T4: other shas still translate", rc == RDNA4X_OK && FK.xlCalls == 1);
        char cd2[1024]; snprintf(cd2, sizeof cd2, "%s/k5", TMP); mkd(cd2);   // a new key is a new directory: the sha is tried again
        fk_reset(); rc = fk_job(cd2, SHA_A, "k", err, sizeof err);
        CHECK("T4: under a new key the sha is retried", rc == RDNA4X_OK && FK.xlCalls == 1);
        // a crash in a child that was killed by a signal rather than a clean exit is the same thing
        char cd3[1024]; snprintf(cd3, sizeof cd3, "%s/k6", TMP); mkd(cd3);
        c = fork();
        if (c == 0) { rdna4x_marker_create(cd3, SHA_C); raise(SIGKILL); _exit(0); }
        waitpid(c, &stt, 0);
        fk_reset(); rc = fk_job(cd3, SHA_C, "k", err, sizeof err);
        CHECK("T4: a process killed by a signal mid-translation is handled the same way", WIFSIGNALED(stt) && rc == RDNA4X_E_SKIP_CRASH && FK.xlCalls == 0);
    }

    // ===== T1.12 one job's order of operations =====
    {
        char jd[1024]; snprintf(jd, sizeof jd, "%s/j1", TMP); mkd(jd);
        char err[256]; int rc;
        fk_reset(); FK.dir = jd; FK.sha = SHA_A;
        rc = fk_job(jd, SHA_A, "k", err, sizeof err);
        CHECK("job: a good shader is installed (.meta.json and .spv), the marker is gone", rc == RDNA4X_OK && file_exists(jd, SHA_A, RDNA4X_EXT_SPV) && file_exists(jd, SHA_A, RDNA4X_EXT_META) && !file_exists(jd, SHA_A, RDNA4X_EXT_INFLIGHT) && !file_exists(jd, SHA_A, RDNA4X_EXT_FAIL));
        CHECK("job: the crash marker existed while the reader and the translator ran", FK.markerSeenInReader == 1 && FK.markerSeenInXlate == 1);
        CHECK("job: buffers are released (text, spv, meta)", FK.freeCalls == 1 && FK.relCalls == 2);
        char p[1100]; rdna4x_file_path(p, sizeof p, jd, SHA_A, RDNA4X_EXT_SPV); struct stat st; stat(p, &st);
        CHECK("job: the .spv is world-readable 0644 and not a temporary name", (st.st_mode & 0777) == 0644 && st.st_size == 64);
        // order: meta BEFORE spv
        char jo[1024]; snprintf(jo, sizeof jo, "%s/j2", TMP); mkd(jo);
        rdna4x_file_path(p, sizeof p, jo, SHA_B, RDNA4X_EXT_SPV); mkd(p);   // block the .spv name with a directory: its rename fails
        fk_reset(); rc = fk_job(jo, SHA_B, "k", err, sizeof err);
        CHECK("job order: with the .spv blocked, the .meta.json was already written (meta first)", rc == RDNA4X_E_IO && file_exists(jo, SHA_B, RDNA4X_EXT_META) && !file_exists(jo, SHA_B, RDNA4X_EXT_INFLIGHT));
        char jm[1024]; snprintf(jm, sizeof jm, "%s/j3", TMP); mkd(jm);
        rdna4x_file_path(p, sizeof p, jm, SHA_C, RDNA4X_EXT_META); mkd(p);
        fk_reset(); rc = fk_job(jm, SHA_C, "k", err, sizeof err);
        CHECK("job order: with the .meta.json blocked, NO .spv appears (the spv comes last, only after the meta)", rc == RDNA4X_E_IO && !file_exists(jm, SHA_C, RDNA4X_EXT_SPV) && !file_exists(jm, SHA_C, RDNA4X_EXT_INFLIGHT));
        // refusals -> .fail
        char jf[1024]; snprintf(jf, sizeof jf, "%s/j4", TMP); mkd(jf);
        fk_reset(); FK.translateRc = 1;
        rc = fk_job(jf, SHA_A, "k", err, sizeof err);
        CHECK("job: a translator refusal writes .fail, removes the marker, installs nothing", rc == RDNA4X_E_REFUSED && file_exists(jf, SHA_A, RDNA4X_EXT_FAIL) && !file_exists(jf, SHA_A, RDNA4X_EXT_INFLIGHT) && !file_exists(jf, SHA_A, RDNA4X_EXT_SPV) && strstr(err, "fake translator"));
        fk_reset(); rc = fk_job(jf, SHA_A, "k", err, sizeof err);
        CHECK("job: the negative cache: the next run does not call anything", rc == RDNA4X_E_SKIP_FAIL && FK.airCalls == 0 && FK.xlCalls == 0);
        fk_reset(); FK.translateRc = 2; rc = fk_job(jf, SHA_B, "k", err, sizeof err);
        CHECK("job: a caught panic (code 2) is a refusal and is cached too", rc == RDNA4X_E_REFUSED && file_exists(jf, SHA_B, RDNA4X_EXT_FAIL));
        fk_reset(); FK.readerFails = 1; rc = fk_job(jf, SHA_C, "k", err, sizeof err);
        CHECK("job: a reader refusal writes .fail, translator not called", rc == RDNA4X_E_READER && file_exists(jf, SHA_C, RDNA4X_EXT_FAIL) && FK.xlCalls == 0 && !file_exists(jf, SHA_C, RDNA4X_EXT_INFLIGHT));
        fk_reset(); FK.bigText = 1; rc = fk_job(jf, SHA_D, "k", err, sizeof err);
        CHECK("job: LLVM text over the 64 MiB cap is refused before the translator runs", rc == RDNA4X_E_CAP && FK.xlCalls == 0 && file_exists(jf, SHA_D, RDNA4X_EXT_FAIL));
        char jg[1024]; snprintf(jg, sizeof jg, "%s/j5", TMP); mkd(jg);
        fk_reset(); FK.bigSpv = 1; rc = fk_job(jg, SHA_A, "k", err, sizeof err);
        CHECK("job: SPIR-V over the 16 MiB cap is not installed", rc == RDNA4X_E_CAP && !file_exists(jg, SHA_A, RDNA4X_EXT_SPV) && file_exists(jg, SHA_A, RDNA4X_EXT_FAIL));
        fk_reset(); { uint8_t big[8]; rc = rdna4x_translate_job(&FDEPS, jg, SHA_B, "k", big, RDNA4X_MAX_AIR + 1, err, sizeof err); }
        CHECK("job: bitcode over the 4 MiB cap is refused before anything is written", rc == RDNA4X_E_CAP && FK.airCalls == 0 && !file_exists(jg, SHA_B, RDNA4X_EXT_INFLIGHT) && !file_exists(jg, SHA_B, RDNA4X_EXT_FAIL));
        fk_reset(); rc = rdna4x_translate_job(&FDEPS, jg, SHA_B, "k", (const uint8_t *)"abc", 3, err, sizeof err);
        CHECK("job: empty / tiny bitcode is refused", rc == RDNA4X_E_CAP && FK.airCalls == 0);
        fk_reset(); rc = fk_job(jg, "short", "k", err, sizeof err);
        CHECK("job: a bad sha is refused", rc == RDNA4X_E_IO && FK.airCalls == 0);
        // a live marker from another process: skipped
        char jl[1024]; snprintf(jl, sizeof jl, "%s/j6", TMP); mkd(jl);
        rdna4x_marker_create(jl, SHA_A);   // this process: alive
        fk_reset(); rc = fk_job(jl, SHA_A, "k", err, sizeof err);
        CHECK("job: a live process's marker means skip (not our translation to run)", rc == RDNA4X_E_SKIP_LIVE && FK.airCalls == 0 && file_exists(jl, SHA_A, RDNA4X_EXT_INFLIGHT));
        rdna4x_marker_remove(jl, SHA_A);
        fk_reset(); FK.dir = jl; FK.sha = SHA_B; rc = fk_job(jl, SHA_B, "write_tex", err, sizeof err);
        CHECK("job: a kernel in the override table is still translated (the table is applied inside the library call)", rc == RDNA4X_OK);
    }

    // ===== T1.13 the engine: the worker's stack, timeouts, sharing =====
    {
        char ed[1024]; snprintf(ed, sizeof ed, "%s/e1", TMP); mkd(ed);
        static rdna4x_engine eng; rdna4x_deps sd = FDEPS; sd.air_to_text = stack_air;
        rdna4x_engine_init(&eng, &sd, NULL, NULL, NULL, RDNA4X_WORKER_STACK);
        fk_reset(); char err[256];
        static const uint8_t air[16] = "BC\xc0\xde-----------";
        int rc = rdna4x_engine_run(&eng, ed, SHA_A, "k", air, sizeof air, 5000, err, sizeof err);
        CHECK("engine: a job runs on the worker and installs its result", rc == RDNA4X_OK && file_exists(ed, SHA_A, RDNA4X_EXT_SPV));
        CHECK("engine: the worker thread has the 64 MiB stack (a default secondary thread has 512 KiB)", FK.stackSeen >= RDNA4X_WORKER_STACK && FK.stackSeen >= (size_t)64u << 20);
        // timeout, then the worker finishes and installs the result
        fk_reset(); FK.delayMs = 400;
        rc = rdna4x_engine_run(&eng, ed, SHA_B, "k", air, sizeof air, 50, err, sizeof err);
        CHECK("engine: the caller stops waiting after its deadline (E_TIMEOUT)", rc == RDNA4X_E_TIMEOUT && !file_exists(ed, SHA_B, RDNA4X_EXT_SPV));
        for (int i = 0; i < 100 && !file_exists(ed, SHA_B, RDNA4X_EXT_SPV); i++) usleep(50000);
        CHECK("engine: the worker finishes the job after the caller left and installs the result", file_exists(ed, SHA_B, RDNA4X_EXT_SPV) && file_exists(ed, SHA_B, RDNA4X_EXT_META) && !file_exists(ed, SHA_B, RDNA4X_EXT_INFLIGHT));
        // not waiting at all
        fk_reset(); FK.delayMs = 100;
        rc = rdna4x_engine_run(&eng, ed, SHA_C, "k", air, sizeof air, 0, err, sizeof err);
        CHECK("engine: wait 0 returns at once and the job still completes", rc == RDNA4X_E_TIMEOUT);
        for (int i = 0; i < 100 && !file_exists(ed, SHA_C, RDNA4X_EXT_SPV); i++) usleep(50000);
        CHECK("engine: ... completes in the background", file_exists(ed, SHA_C, RDNA4X_EXT_SPV));
        // sharing: two callers for one sha are one translation
        fk_reset(); FK.delayMs = 300;
        struct share sh = { &eng, ed, -1 };
        pthread_t th; pthread_create(&th, NULL, share_thread, &sh);
        rc = rdna4x_engine_run(&eng, ed, SHA_D, "k", air, sizeof air, 5000, err, sizeof err);
        pthread_join(th, NULL);
        CHECK("engine: two callers for one sha share one translation", rc == RDNA4X_OK && sh.rc == RDNA4X_OK && FK.xlCalls == 1);
        {   // build 16 (P4): a caller that arrives after the job finished (it missed the running job by a hair) is answered from the installed pair, not by a second translation
            const int before = FK.xlCalls;
            rc = rdna4x_engine_run(&eng, ed, SHA_D, "k", air, sizeof air, 5000, err, sizeof err);
            CHECK("engine (P4): a repeat request for an already-installed sha returns OK without translating again", rc == RDNA4X_OK && FK.xlCalls == before);
        }
        // unavailable translator
        static rdna4x_engine bad; rdna4x_engine_init(&bad, NULL, "/nonexistent-base", "/nonexistent/libLLVM.dylib", "/nonexistent/libgarnetxlate.dylib", RDNA4X_WORKER_STACK);
        fk_reset(); rc = rdna4x_engine_run(&bad, ed, SHA_A, "k", air, sizeof air, 5000, err, sizeof err);
        CHECK("engine: libraries that cannot be loaded give E_UNAVAILABLE with a message, not a crash", rc == RDNA4X_E_UNAVAILABLE && err[0]);
        rc = rdna4x_engine_run(&bad, ed, SHA_B, "k", air, 2, 100, err, sizeof err);
        CHECK("engine: oversize / tiny bitcode is refused before any thread work", rc == RDNA4X_E_CAP);
        // a directory that sweeps on the first job: dead markers become .crash
        char sw[1024]; snprintf(sw, sizeof sw, "%s/e2", TMP); mkd(sw);
        pid_t c = fork(); if (c == 0) _exit(0);
        int stt = 0; waitpid(c, &stt, 0);
        char p[1100], pid[32]; snprintf(pid, sizeof pid, "%d\n", (int)c);
        rdna4x_file_path(p, sizeof p, sw, SHA_C, RDNA4X_EXT_INFLIGHT); touch_file(p, pid);
        fk_reset(); rc = rdna4x_engine_run(&eng, sw, SHA_A, "k", air, sizeof air, 5000, err, sizeof err);
        CHECK("engine: the first job in a directory sweeps it (a dead marker of ANOTHER sha becomes .crash)", rc == RDNA4X_OK && file_exists(sw, SHA_C, RDNA4X_EXT_CRASH) && !file_exists(sw, SHA_C, RDNA4X_EXT_INFLIGHT));
    }
    {   // spawn: a thread made by rdna4x_spawn gets exactly the stack asked for (>= 64 MiB); one made the default way does not
        seen_stack = 0;
        pthread_t t; rdna4x_spawn(&t, probe_stack, NULL, RDNA4X_WORKER_STACK);
        for (int i = 0; i < 100 && !seen_stack; i++) usleep(10000);
        CHECK("spawn: rdna4x_spawn(..., RDNA4X_WORKER_STACK) gives a 64 MiB stack", seen_stack >= RDNA4X_WORKER_STACK);
        pthread_attr_t a; size_t def = 0; pthread_attr_init(&a); pthread_attr_getstacksize(&a, &def); pthread_attr_destroy(&a);
        CHECK("spawn: the default secondary-thread stack is far smaller (this is what the 64 MiB avoids)", def < (size_t)8u << 20);
    }

    // ===== G source pins on GarnetDevice.m =====
    {
        char *src = slurp(dir, "GarnetDevice.m");
        CHECK("GarnetDevice.m is readable", src != NULL);
        if (src) {
            const char *act = fn_start(src, "static BOOL rdna4x_active(void) {");
            const char *actEnd = act ? strstr(act, "\n}\n") : NULL;
            CHECK("G: rdna4x_active calls rdna4x_applies with WindowServer first, then admitted, the root-allow tool, RDNA4M_TEST_INPROC, the kill file and the environment",
                  has(act, actEnd, "rdna4x_applies(rdna4_is_ws(), atomic_load(&rdna4_app_admitted), rdna4_allow(), rdna4x_test_inproc(), RDNA4XL.killed, RDNA4XL.envDirty)"));
            const char *gr = fn_start(src, "static void rdna4x_gate_read(void) {");
            const char *grEnd = gr ? strstr(gr, "\n}\n") : NULL;
            CHECK("G: the kill file is read with rdna4x_killed_from_stat (fail closed) and the environment with rdna4x_env_dirty, never in WindowServer",
                  has(gr, grEnd, "if (!rdna4_is_ws())") && has(gr, grEnd, "stat(RDNA4X_KILL_FILE, &st)") && has(gr, grEnd, "RDNA4XL.killed = rdna4x_killed_from_stat(rc, er);") && has(gr, grEnd, "RDNA4XL.envDirty = rdna4x_env_dirty(*_NSGetEnviron());"));
            const char *dirs = fn_start(src, "static NSArray<NSString *> *rdna4_spv_dirs(void) {");
            const char *dirsEnd = dirs ? strstr(dirs, "\n}\n") : NULL;
            const char *pb = dirs ? strstr(dirs, "NSMutableArray *m = [bundleOnly mutableCopy];") : NULL;
            const char *pu = dirs ? strstr(dirs, "if (u) { [m addObject:u]; userFor = u; }") : NULL;
            const char *ps = dirs ? strstr(dirs, "if (!skipSide) [m addObject:rdna4_side_dir()];") : NULL;
            CHECK("G: lookup order: the bundle spvcache first, then the user cache directory, then the side directory", before(pb, pu) && before(pu, ps) && pb < dirsEnd && ps < dirsEnd);
            CHECK("G: the bundle spvcache is the first entry of the array the user directory is appended to", has(dirs, dirsEnd, "bundleOnly = @[ [[[NSBundle bundleForClass:[GarnetDevice class]] resourcePath] stringByAppendingPathComponent:@\"spvcache\"] ];"));
            CHECK("G: the side directory is skipped for a sandboxed non-WindowServer process that translates in process",
                  has(dirs, dirsEnd, "const BOOL skipSide = u && rdna4x_skip_side_dir(rdna4_is_ws(), rdna4x_sandboxed());") && has(src, NULL, "RDNA4XL.sandboxed = sandbox_check(getpid(), NULL, 0) != 0;"));
            const char *su = fn_start(src, "static void rdna4x_setup_locked(void) {");
            const char *suEnd = su ? strstr(su, "\n}\n") : NULL;
            CHECK("G: the key is made from the translator's UUID, the reader's identity, the override hash and kern.osversion",
                  has(su, suEnd, "rdna4x_file_uuid(rdna4x_xlib_path().fileSystemRepresentation, xl)") && has(su, suEnd, "rdna4x_override_hash(ov)") && has(su, suEnd, "rdna4x_key_with_capabilities(key, xl, rd, ov, osv, &queryCaps)") && has(su, suEnd, "_dyld_get_shared_cache_uuid(cu)") &&
                  has(su, suEnd, "sysctlbyname(\"kern.osversion\""));
            CHECK("G: the user cache directory is set up by rdna4x_user_dir after admission, not inside rdna4_spv_dirs' dispatch_once", has(src, NULL, "static NSString *rdna4x_user_dir(void) {\n    if (!rdna4x_active()) return nil;") && !has(dirs, strstr(dirs, "dispatch_once(&o, ^{"), "rdna4x_user_dir"));
            const char *tf = fn_start(src, "static BOOL rdna4x_translate_fn_with_error(id fn, const char *role, uint64_t deadlineNs, NSError **error) {");
            const char *tfEnd = tf ? strstr(tf, "\n}\n") : NULL;
            const char *prepare = fn_start(src, "static void rdna4x_engine_prepare(void) {");
            const char *prepareEnd = prepare ? strstr(prepare, "\n}\n") : NULL;
            CHECK("G: translation and argument lowering share the 64-MiB worker", has(tf, tfEnd, "rdna4x_engine_prepare();") && has(prepare, prepareEnd, "rdna4x_engine_init(&RDNA4XL.eng, NULL, NULL, rl, rdna4x_xlib_path().fileSystemRepresentation, RDNA4X_WORKER_STACK);") && has(prepare, prepareEnd, "RDNA4XL.eng.lowerTranslatedArguments = RDNA4R.gpuArgumentTables;"));
            CHECK("G: in-process waits obey the app deadline", has(tf, tfEnd, "deadlineNs > now") && has(tf, tfEnd, "leftMs > RDNA4G_SYNC_WAIT_APP_MS ? RDNA4G_SYNC_WAIT_APP_MS : (int)leftMs") && !has(tf, tfEnd, "RDNA4G_SYNC_WAIT_MS"));
            CHECK("G: in-process waits share the process strikes budget and still enqueue after exhaustion",
                  has(tf, tfEnd, "waitMs > 0 && rdna4_sync_begin(!rdna4_is_ws())") && has(tf, tfEnd, "if (!waiting) waitMs = 0;") &&
                  has(tf, tfEnd, "if (waiting) rdna4_sync_end(rc == RDNA4X_OK, rc == RDNA4X_E_TIMEOUT);") &&
                  before(strstr(tf, "if (!waiting) waitMs = 0;"), strstr(tf, "rdna4x_engine_run_linked(")) &&
                  before(strstr(tf, "if (!waiting) waitMs = 0;"), strstr(tf, "rdna4x_engine_run(")));
            const char *sw = fn_start(src, "static BOOL rdna4_sync_wait(const char *what, NSArray<NSString *> *shas, BOOL needMeta) {");
            const char *swEnd = sw ? strstr(sw, "\n}\n") : NULL;
            CHECK("G: daemon waits use the WS/app class budget and shared strikes state",
                  has(sw, swEnd, "if (!rdna4_sync_begin(NO)) return NO;") && has(sw, swEnd, "rdna4g_sync_wait_ms(rdna4_is_ws())") &&
                  has(sw, swEnd, "rdna4_sync_end(found, !found && rdna4_now() >= deadline);"));
            // the three miss branches: translate in process first, the dump and the daemon wait only when not
            const char *ri = fn_start(src, "- (instancetype)initWithDevice:(id)dev descriptor:(MTLRenderPipelineDescriptor *)d noFallback:(BOOL)nofb error:(NSError **)err {");
            const char *riEnd = ri ? strstr(ri, "\n}\n") : NULL;
            CHECK("G: render translation and both postpasses share a deadline after backend initialization", has(ri, riEnd, "const uint64_t deadline = rdna4_now() + (uint64_t)RDNA4G_SYNC_WAIT_APP_MS * 1000000ull;") && has(ri, riEnd, "const uint64_t dl = deadline;") && has(ri, riEnd, "rdna4_gpu_argument_spv(vsd, &mv_, deadline, err)") && has(ri, riEnd, "rdna4_gpu_argument_spv(fsd, &mf_, deadline, err)") && before(strstr(ri, "if (!rdna4_radv_open(err)) return nil;"), strstr(ri, "const uint64_t deadline")));
            const char *r1 = ri ? strstr(ri, "rdna4x_translate_fn_with_error(d.vertexFunction, \"vertex\", dl, &vertexFailure)") : NULL;
            const char *r2 = ri ? strstr(ri, "(void)rdna4_dump_pipeline(d);") : NULL;
            const char *r3 = ri ? strstr(ri, "rdna4_sync_wait(\"render\"") : NULL;
            CHECK("G: render miss: the in-process translation runs before the AIR dump and the daemon wait, which are skipped when it applies",
                  before(r1, r2) && before(r2, r3) && r3 < riEnd && has(ri, riEnd, "if (!inproc) (void)rdna4_dump_pipeline(d);") && has(ri, riEnd, "if (!inproc && rdna4g_syncwait_applies(") && has(ri, riEnd, "(e.code == 41 && !nofb && rdna4x_active())"));
            const char *np = fn_start(src, "- (id)rdna4NewComputePipeline:(id)fn error:(NSError **)error {");
            const char *npEnd = np ? strstr(np, "\n}\n") : NULL;
            CHECK("G: compute translation starts the app deadline after backend initialization", has(np, npEnd, "rdna4_now() + (uint64_t)RDNA4G_SYNC_WAIT_APP_MS * 1000000ull") && before(strstr(np, "const BOOL backendReady = !inproc || rdna4_radv_open(&inprocFailure);"), strstr(np, "rdna4x_translate_fn_with_error(")));
            const char *c1 = np ? strstr(np, "rdna4x_translate_fn_with_error(fn, \"kernel\"") : NULL;
            const char *c2 = np ? strstr(np, "rdna4_sync_wait(\"kernel\"") : NULL;
            CHECK("G: compute miss: in-process first, then (only if not in process) the dump + wait; the placeholder stays the last resort",
                  before(c1, c2) && c2 < npEnd && has(np, npEnd, "&inprocFailure))") &&
                  has(np, npEnd, "if (!inproc && e.code == 41 && rdna4g_syncwait_applies(") && has(np, npEnd, "if (e.code == 41 && rdna4_fallback_ok())"));
            const char *nn = fn_start(src, "- (id)rdna4NewPipeline:(MTLRenderPipelineDescriptor *)d error:(NSError **)error {");
            const char *nnEnd = nn ? strstr(nn, "\n}\n") : NULL;
            CHECK("G: rdna4NewPipeline logs LINKAGE IGNORED first and does not dump after an in-process attempt", has(nn, nnEnd, "rdna4x_linkage_render(d);") && has(nn, nnEnd, "NSError *dump = rdna4x_active() ? e : rdna4_dump_pipeline(d);"));
            CHECK("G: all four compute descriptor methods install linkage on their compiling thread", ({ int sync = 0, async = 0; const char *q = src; while ((q = strstr(q, "rdna4x_linkage_compute(d);"))) { sync++; q++; } q = src; while ((q = strstr(q, "rdna4x_linkage_compute(saved);"))) { async++; q++; } sync == 2 && async == 2; }));
            CHECK("G: the LINKAGE IGNORED line exists", has(src, NULL, "RDNA4LOG(\"LINKAGE IGNORED %s '%s': "));
            CHECK("G: a function with no bitcodeData is logged once per process (CI / stitched instrumentation)", has(src, NULL, "RDNA4_ONCE(\"function '%s' (class %s) has no bitcodeData"));
            const char *touchHit = fn_start(src, "static void rdna4x_touch_hit(NSString *path, NSString *sha) {");
            const char *touchHitEnd = touchHit ? strstr(touchHit, "\n}\n") : NULL;
            CHECK("G: a user-cache hit in a sandbox never attempts a timestamp write",
                  has(src, NULL, "rdna4x_touch_hit(path, sha);") &&
                  has(touchHit, touchHitEnd, "rdna4x_sandboxed()) return;") &&
                  before(strstr(touchHit, "rdna4x_sandboxed()) return;"), strstr(touchHit, "rdna4x_touch(path.fileSystemRepresentation)")));
            free(src);
        }
        char *hdr = slurp(dir, "rdna4_xlate.h");
        CHECK("G: rdna4_xlate.h is readable", hdr != NULL);
        if (hdr) {
            const char *tj = fn_start(hdr, "static inline int rdna4x_process_job(");
            const char *tjEnd = tj ? strstr(tj, "\n}\n") : NULL;
            const char *m1 = tj ? strstr(tj, "rdna4x_probe(dir, sha)") : NULL, *m2 = tj ? strstr(tj, "rdna4x_marker_create(dir, sha)") : NULL, *m3 = tj ? strstr(tj, "dp->air_to_text(") : NULL, *m4 = tj ? strstr(tj, "dp->translate(") : NULL;
            const char *w1 = tj ? strstr(tj, "RDNA4X_EXT_META) || rdna4x_write_atomic") : NULL, *w2 = tj ? strstr(tj, "RDNA4X_EXT_SPV) || rdna4x_write_atomic") : NULL, *rm = tj ? strstr(tj, "rdna4x_marker_remove(dir, sha);") : NULL;
            CHECK("G: job order in the header: probe, marker, reader, translator, meta, spv, marker removed", before(m1, m2) && before(m2, m3) && before(m3, m4) && before(m4, w1) && before(w1, w2) && before(w2, rm) && rm < tjEnd);
            const char *rp = fn_start(hdr, "static inline int rdna4x_reader_print(");
            const char *rpEnd = rp ? strstr(rp, "\n}\n") : NULL;
            CHECK("G: the reader switches Apple's LLVM context to opaque pointers BEFORE it reads (else typed-pointer text the translator refuses)",
                  has(rp, rpEnd, "if (r->SetOpaquePointers) r->SetOpaquePointers(ctx, 1);") && before(strstr(rp, "SetOpaquePointers(ctx, 1)"), strstr(rp, "ParseBitcodeInContext2(")) && has(hdr, NULL, "_ZNK4llvm11LLVMContext17setOpaquePointersEb"));
            free(hdr);
        }
    }

    printf("test-xlate: %d checks, %d failed\n", runs, fails);
    return fails ? 1 : 0;
}
