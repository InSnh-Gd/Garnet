// test-xlate-fallback.c: host unit test for two-tier inproc shader cache fallback.
// Verifies:
//   - rdna4x_killed_from_stat_for_app (admitted sandboxed app EPERM does not falsely disable inproc)
//   - Tier-0 primary cache dir selection
//   - Tier-1 <tempDir>/TemporaryItems/org.garnet.xlate/<K> fallback when primary cache is denied
//   - Symlink rejection on TemporaryItems
//   - Tier-2 bounded 64 MiB LRU in-memory cache: hit, miss, negative refusal cache, LRU eviction,
//     MRU promotion on hit, oversized-item refusal without evicting live entries, overwrite accounting
//   - Production source pins across garnet_shaders.m, garnet_pipelines.m, garnet_arguments.m
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "garnet_xlate_fallback.h"

static int g_runs = 0, g_fails = 0;
#define CHECK(name, cond) do { g_runs++; if (!(cond)) { g_fails++; printf("FAIL: %s\n", name); } } while (0)

static void make_sha(char out[65], int idx) {
    snprintf(out, 65, "%064x", idx);
}

static char *slurp(const char *dir, const char *name) {
    char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = (char *)malloc((size_t)n + 1);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    b[n] = 0; fclose(f); return b;
}

int main(int argc, char **argv) {
    const char *srcDir = argc > 1 ? argv[1] : ".";
    const char *tmpRoot = argc > 2 ? argv[2] : "/tmp";

    // 1. Sandbox-aware kill-file check
    CHECK("K1: PRESENT kill file always kills", rdna4x_killed_from_stat_for_app(0, 0, 1) == 1 && rdna4x_killed_from_stat_for_app(0, 0, 0) == 1);
    CHECK("K2: ABSENT kill file never kills", rdna4x_killed_from_stat_for_app(-1, ENOENT, 1) == 0 && rdna4x_killed_from_stat_for_app(-1, ENOENT, 0) == 0);
    CHECK("K3: admitted sandboxed app EPERM/EACCES does NOT disable inproc",
          rdna4x_killed_from_stat_for_app(-1, EPERM, 1) == 0 && rdna4x_killed_from_stat_for_app(-1, EACCES, 1) == 0);
    CHECK("K4: non-app (root tool) EPERM still fails closed",
          rdna4x_killed_from_stat_for_app(-1, EPERM, 0) == 1 && rdna4x_killed_from_stat_for_app(-1, EIO, 1) == 1);

    // 2. Two-tier disk selection (Tier-0 -> Tier-1 TemporaryItems -> Tier-2 memory)
    const char *key = "0123456789abcdef";
    char base[512];
    snprintf(base, sizeof base, "%s/xlate-fb-%d", tmpRoot, (int)getpid());
    mkdir(base, 0700);
    char cdir[600], tdir[600], rodir[600], symdir[600], symtmp[640], realdir[600], outDir[RDNA4X_PATH_MAX];
    snprintf(cdir, sizeof cdir, "%s/cache", base);
    snprintf(tdir, sizeof tdir, "%s/temp", base);
    snprintf(rodir, sizeof rodir, "%s/ro", base);
    snprintf(symdir, sizeof symdir, "%s/symtemp", base);
    snprintf(symtmp, sizeof symtmp, "%s/TemporaryItems", symdir);
    snprintf(realdir, sizeof realdir, "%s/realdir", base);
    mkdir(cdir, 0700); mkdir(tdir, 0700); mkdir(rodir, 0500); mkdir(symdir, 0700); mkdir(realdir, 0700);

    // Tier-0 hit
    int tier = rdna4x_select_disk_cache(cdir, tdir, key, outDir, sizeof outDir);
    CHECK("D1: primary cache dir returns tier 1", tier == 1 && strstr(outDir, "/cache/org.garnet.xlate/0123456789abcdef") != NULL);

    // Tier-1 fallback when primary is unwritable (skip chmod-ro check if running as root, use non-dir file blocker instead)
    char blocker[640];
    snprintf(blocker, sizeof blocker, "%s/blocker", base);
    FILE *bf = fopen(blocker, "w"); if (bf) fclose(bf);
    tier = rdna4x_select_disk_cache(blocker, tdir, key, outDir, sizeof outDir);
    CHECK("D2: blocked primary falls back to Tier-1 TemporaryItems",
          tier == 2 && strstr(outDir, "/temp/TemporaryItems/org.garnet.xlate/0123456789abcdef") != NULL);

    // Symlink at TemporaryItems MUST be rejected
    CHECK("D3: create symlink TemporaryItems", symlink(realdir, symtmp) == 0);
    tier = rdna4x_select_disk_cache(blocker, symdir, key, outDir, sizeof outDir);
    CHECK("D3: symlinked TemporaryItems is rejected (returns 0 -> Tier-2 memory)", tier == 0 && outDir[0] == 0);

    // Both blocked -> Tier-2 memory fallback (returns 0)
    tier = rdna4x_select_disk_cache(blocker, blocker, key, outDir, sizeof outDir);
    CHECK("D4: both primary and TemporaryItems denied returns 0 (Tier-2 memory)", tier == 0);

    // Pre-existing read-only primary dir (simulates Chromium GPU sandbox: stat/EEXIST ok, O_CREAT -> EACCES/EPERM)
    if (geteuid() != 0) {
        char roExistBase[600], roExistParent[640], roExistLeaf[680];
        snprintf(roExistBase, sizeof roExistBase, "%s/ro-exist", base);
        snprintf(roExistParent, sizeof roExistParent, "%s/%s", roExistBase, RDNA4X_CACHE_SUB);
        snprintf(roExistLeaf, sizeof roExistLeaf, "%s/%s", roExistParent, key);
        mkdir(roExistBase, 0700); mkdir(roExistParent, 0700); mkdir(roExistLeaf, 0700);
        chmod(roExistLeaf, 0500);
        tier = rdna4x_select_disk_cache(roExistBase, tdir, key, outDir, sizeof outDir);
        CHECK("D5: pre-existing read-only primary falls back to Tier-1 TemporaryItems",
              tier == 2 && strstr(outDir, "/temp/TemporaryItems/org.garnet.xlate/0123456789abcdef") != NULL);
        tier = rdna4x_select_disk_cache(roExistBase, blocker, key, outDir, sizeof outDir);
        CHECK("D6: pre-existing read-only primary + blocked temp falls back to Tier-2 memory",
              tier == 0 && outDir[0] == 0);
        chmod(roExistLeaf, 0700);
    }

    // 3. Tier-2 bounded memory cache (LRU eviction + negative cache + oversize rejection)
    CHECK("M0: default cap is 64 MiB", RDNA4X_MEMCACHE_CAP == 64u * 1024u * 1024u);
    rdna4x_memcache_t mc;
    rdna4x_memcache_init(&mc, 1024u); // small deterministic cap for eviction testing

    char s1[65], s2[65], s3[65], s4[65], sbig[65];
    make_sha(s1, 1); make_sha(s2, 2); make_sha(s3, 3); make_sha(s4, 4); make_sha(sbig, 99);
    uint8_t spv1[400], spv2[400], spv3[400], bigSpv[1100];
    memset(spv1, 0x11, sizeof spv1);
    memset(spv2, 0x22, sizeof spv2);
    memset(spv3, 0x33, sizeof spv3);
    memset(bigSpv, 0x99, sizeof bigSpv);
    const uint8_t meta1[] = "{\"entry\":\"s1\"}";
    const uint8_t meta2[] = "{\"entry\":\"s2\"}";
    const uint8_t meta3[] = "{\"entry\":\"s3\"}";

    CHECK("M1: put s1 and s2 fit within 1024 B cap",
          rdna4x_memcache_put(&mc, s1, spv1, sizeof spv1, meta1, sizeof meta1 - 1) == 0 &&
          rdna4x_memcache_put(&mc, s2, spv2, sizeof spv2, meta2, sizeof meta2 - 1) == 0 &&
          mc.count == 2 && mc.evictions == 0);

    // Touch s1 so s1 becomes MRU and s2 becomes LRU
    uint8_t *gotSpv = NULL, *gotMeta = NULL; size_t gotSpvLen = 0, gotMetaLen = 0;
    CHECK("M2: get s1 hits and returns exact bytes",
          rdna4x_memcache_get(&mc, s1, &gotSpv, &gotSpvLen, &gotMeta, &gotMetaLen, NULL, 0) == 1 &&
          gotSpvLen == sizeof spv1 && memcmp(gotSpv, spv1, sizeof spv1) == 0 &&
          gotMetaLen == sizeof meta1 - 1 && memcmp(gotMeta, meta1, sizeof meta1 - 1) == 0);
    free(gotSpv); free(gotMeta);

    // Oversized item MUST fail without evicting s1/s2
    CHECK("M3: oversized item (> cap) rejected without evicting live entries",
          rdna4x_memcache_put(&mc, sbig, bigSpv, sizeof bigSpv, meta1, sizeof meta1 - 1) == -1 &&
          mc.count == 2 && mc.evictions == 0);

    // Put s3 -> exceeds 1024 B -> must evict LRU (s2), keeping MRU (s1)
    CHECK("M4: put s3 triggers LRU eviction of s2 while retaining MRU s1",
          rdna4x_memcache_put(&mc, s3, spv3, sizeof spv3, meta3, sizeof meta3 - 1) == 0 &&
          mc.evictions == 1 && mc.totalBytes <= mc.capBytes &&
          rdna4x_memcache_get(&mc, s2, NULL, NULL, NULL, NULL, NULL, 0) == 0 &&
          rdna4x_memcache_get(&mc, s1, NULL, NULL, NULL, NULL, NULL, 0) == 1 &&
          rdna4x_memcache_get(&mc, s3, NULL, NULL, NULL, NULL, NULL, 0) == 1);

    // Negative refusal cache
    char whyBuf[256] = "";
    CHECK("M5: negative refusal cache stores and replays failure reason",
          rdna4x_memcache_put_refusal(&mc, s4, "unsupported opcode") == 0 &&
          rdna4x_memcache_get(&mc, s4, NULL, NULL, NULL, NULL, whyBuf, sizeof whyBuf) == -1 &&
          strcmp(whyBuf, "unsupported opcode") == 0);

    // Overwrite s3 with smaller entry updates totalBytes cleanly
    const size_t beforeBytes = mc.totalBytes;
    CHECK("M6: overwrite updates byte accounting cleanly",
          rdna4x_memcache_put(&mc, s3, spv3, 40, meta3, sizeof meta3 - 1) == 0 &&
          mc.totalBytes == beforeBytes - (sizeof spv3 - 40));

    rdna4x_memcache_destroy(&mc);

    // 4. Production source pins
    char *sh = slurp(srcDir, "garnet_shaders.m");
    char *pl = slurp(srcDir, "garnet_pipelines.m");
    char *ag = slurp(srcDir, "garnet_arguments.m");
    CHECK("P1: garnet_shaders.m wires sandbox kill check, two-tier select, one-way sandbox latch, and memory cache",
          sh && strstr(sh, "rdna4x_killed_from_stat_for_app(") != NULL &&
          strstr(sh, "rdna4x_select_disk_cache(") != NULL &&
          strstr(sh, "rdna4x_memcache_init(") != NULL &&
          strstr(sh, "rdna4g_sb_check_latch(RDNA4XL.sandboxedLatched") != NULL);
    CHECK("P2: garnet_pipelines.m checks Tier-2 memory cache in rdna4_spv_lookup_impl",
          pl && strstr(pl, "rdna4x_memcache_active()") != NULL &&
          strstr(pl, "rdna4x_memcache_lookup(") != NULL);
    CHECK("P3: garnet_arguments.m supports Tier-2 memory cache in rdna4_gpu_argument_spv",
          ag && strstr(ag, "rdna4x_memcache_active()") != NULL &&
          strstr(ag, "rdna4x_memcache_lookup(") != NULL);
    free(sh); free(pl); free(ag);

    // Cleanup temp fixture
    unlink(symtmp); unlink(blocker);
    printf("test-xlate-fallback: %d checks, %d failed\n", g_runs, g_fails);
    return g_fails ? 1 : 0;
}
