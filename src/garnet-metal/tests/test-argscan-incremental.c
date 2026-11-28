// Host regression tests for incremental/dirty-range argscan scanning
// and pure-C word scanning in garnet_argument_scan.h / garnet_arguments.m.
//
// Verifies 100% exact residency equivalence against a word-by-word full-buffer
// oracle across:
//   1. Empty live sampler set fast path (zero bytes read)
//   2. 16 MiB incremental dirty-range cache (clean chunk skip, single-chunk
//      add/overwrite/remove, cross-chunk dirty span, mark_all_dirty)
//   3. Partial work ranges, unaligned buffer tails, and dirty-bit preservation
//   4. Coherent MTLStorageModeShared / Managed direct CPU writes bypassing
//      didModifyRange: (coherent_direct_writes = true)
//   5. Strided work and non-8-byte-aligned starting offsets
//   6. Wide token spans (>= 4096, binary-search path) and > 512 live samplers
//      (heap match bitset path)
//   7. Randomized fuzz equivalence (200 rounds)
//   8. 16 MiB throughput sanity check
//   9. Production source pins on garnet_arguments.m / garnet_resources.m /
//      garnet_metal_internal.h

#define _POSIX_C_SOURCE 200809L

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "garnet_argument_scan.h"

static int g_checks = 0;
static int g_fails = 0;

#define CHECK(name, cond, fmt, ...) do { \
    g_checks++; \
    if (!(cond)) { \
        g_fails++; \
        fprintf(stderr, "FAIL [%s:%d] %s: " fmt "\n", \
                __FILE__, __LINE__, (name), ##__VA_ARGS__); \
    } \
} while (0)

static uint64_t make_token(uint64_t id) {
    return (RDNA4_AS_TOKEN_PREFIX << RDNA4_AS_TOKEN_PREFIX_SHIFT) |
           (id & UINT64_C(0x0000FFFFFFFFFFFF));
}

static void write_u64(uint8_t *buf, uint64_t off, uint64_t val) {
    memcpy(buf + off, &val, sizeof(val));
}

static uint64_t prng_next(uint64_t *state) {
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

static void oracle_scan_work(const uint8_t *bytes, uint64_t byte_len,
                             const rdna4_as_work *work,
                             const uint64_t *live_tokens, size_t live_count,
                             bool *found_out) {
    memset(found_out, 0, live_count * sizeof(*found_out));
    if (!work->count || !live_count) return;
    uint64_t off = work->offset;
    for (uint64_t i = 0; i < work->count; ++i) {
        if (off <= byte_len && RDNA4_AS_TOKEN_BYTES <= byte_len - off) {
            uint64_t w = 0;
            memcpy(&w, bytes + off, sizeof(w));
            for (size_t j = 0; j < live_count; ++j) {
                if (w == live_tokens[j]) found_out[j] = true;
            }
        }
        if (i + 1u < work->count) off += work->stride;
    }
}

static bool match_set_equals_oracle(const rdna4_as_match_set *set,
                                    const bool *oracle, size_t live_count,
                                    size_t *mismatch_idx) {
    for (size_t i = 0; i < live_count; ++i) {
        bool got = rdna4_as_match_set_has(set, (uint32_t)i);
        if (got != oracle[i]) {
            if (mismatch_idx) *mismatch_idx = i;
            return false;
        }
    }
    return true;
}

static rdna4_as_work make_dense_work(uint64_t offset, uint64_t word_count) {
    rdna4_as_work w;
    memset(&w, 0, sizeof(w));
    w.root = 0;
    w.kind = RDNA4_AS_KIND_UNKNOWN;
    w.flags = RDNA4_AS_TRANSPORT;
    w.mode = RDNA4_AS_SCAN_CPU_MAPPED;
    w.offset = offset;
    w.stride = RDNA4_AS_TOKEN_BYTES;
    w.count = word_count;
    w.width = RDNA4_AS_TOKEN_BYTES;
    return w;
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void test_empty_live_set(void) {
    const uint64_t len = 1024u * 1024u;
    uint8_t *buf = (uint8_t *)calloc(1, len);
    CHECK("alloc 1 MiB buf", buf != NULL, "calloc failed");
    if (!buf) return;
    write_u64(buf, 0, make_token(1));
    write_u64(buf, 4096, make_token(2));

    rdna4_as_live_table live;
    CHECK("empty live table build",
          rdna4_as_live_table_build(&live, NULL, 0) == RDNA4_AS_OK,
          "status failed");
    CHECK("empty live table count == 0", live.count == 0, "count=%zu", live.count);

    rdna4_as_inc_cache cache;
    CHECK("empty test cache init",
          rdna4_as_inc_cache_init(&cache, len) == RDNA4_AS_OK,
          "init failed");
    rdna4_as_match_set matches;
    CHECK("empty match set init", rdna4_as_match_set_init(&matches, 0), "init failed");

    rdna4_as_work work = make_dense_work(0, len / RDNA4_AS_TOKEN_BYTES);
    CHECK("empty live scan returns OK",
          rdna4_as_inc_cache_scan_work(&cache, buf, len, &work, &live, false, &matches) == RDNA4_AS_OK,
          "scan failed");
    CHECK("empty live scan touches zero chunks",
          cache.dirty_chunks_scanned == 0 && cache.clean_chunks_skipped == 0,
          "dirty=%" PRIu64 " clean=%" PRIu64,
          cache.dirty_chunks_scanned, cache.clean_chunks_skipped);

    rdna4_as_match_set_destroy(&matches);
    rdna4_as_inc_cache_destroy(&cache);
    free(buf);
}

static void test_incremental_16mb_lifecycle(void) {
    const uint64_t len = 16u * 1024u * 1024u;
    const uint64_t chunks = len / RDNA4_AS_INC_CHUNK_BYTES; // 4096
    uint8_t *buf = (uint8_t *)calloc(1, len);
    CHECK("alloc 16 MiB buf", buf != NULL, "calloc failed");
    if (!buf) return;

    // Populate float16 / non-live 0x5244 decoys across the buffer.
    for (uint64_t off = 0; off < len; off += 1024u)
        write_u64(buf, off, make_token(0x900000u + (off >> 10)));

    // Build 16 live samplers (bitset fast path since span < 4096).
    const size_t live_n = 16;
    uint64_t live_tokens[16];
    rdna4_as_live_entry entries[16];
    for (size_t i = 0; i < live_n; ++i) {
        live_tokens[i] = make_token(100u + (uint64_t)i * 7u);
        entries[i] = (rdna4_as_live_entry){ .token = live_tokens[i], .value_index = (uint32_t)i };
    }
    rdna4_as_live_table live;
    CHECK("16 MiB live table build",
          rdna4_as_live_table_build(&live, entries, live_n) == RDNA4_AS_OK && live.use_bitset,
          "build failed or bitset disabled");

    // Seed tokens at key boundary positions.
    write_u64(buf, 0, live_tokens[0]);
    write_u64(buf, 4088, live_tokens[1]);
    write_u64(buf, 4096, live_tokens[2]);
    write_u64(buf, 4104, live_tokens[3]);
    write_u64(buf, len - 8u, live_tokens[4]);

    rdna4_as_inc_cache cache;
    CHECK("16 MiB inc cache init",
          rdna4_as_inc_cache_init(&cache, len) == RDNA4_AS_OK && cache.chunk_count == chunks,
          "init failed");

    rdna4_as_work full = make_dense_work(0, len / RDNA4_AS_TOKEN_BYTES);
    bool oracle[16];
    rdna4_as_match_set matches;
    size_t mm = 0;

    // 1. Cold scan: scans all 4096 chunks once.
    CHECK("cold match_set_init", rdna4_as_match_set_init(&matches, live_n), "init failed");
    oracle_scan_work(buf, len, &full, live_tokens, live_n, oracle);
    CHECK("16 MiB cold scan OK",
          rdna4_as_inc_cache_scan_work(&cache, buf, len, &full, &live, false, &matches) == RDNA4_AS_OK,
          "scan failed");
    CHECK("16 MiB cold scan matches oracle",
          match_set_equals_oracle(&matches, oracle, live_n, &mm),
          "mismatch at idx %zu", mm);
    CHECK("16 MiB cold scan refreshed all 4096 chunks",
          cache.dirty_chunks_scanned == chunks && cache.clean_chunks_skipped == 0,
          "dirty=%" PRIu64 " clean=%" PRIu64,
          cache.dirty_chunks_scanned, cache.clean_chunks_skipped);
    rdna4_as_match_set_destroy(&matches);

    // 2. Warm repeat scan (0 dirty chunks): skips all 4096 chunks.
    CHECK("warm match_set_init", rdna4_as_match_set_init(&matches, live_n), "init failed");
    double t0 = now_ms();
    CHECK("16 MiB warm scan OK",
          rdna4_as_inc_cache_scan_work(&cache, buf, len, &full, &live, false, &matches) == RDNA4_AS_OK,
          "scan failed");
    double warm_ms = now_ms() - t0;
    CHECK("16 MiB warm scan matches oracle",
          match_set_equals_oracle(&matches, oracle, live_n, &mm),
          "mismatch at idx %zu", mm);
    CHECK("16 MiB warm scan skipped all 4096 chunks",
          cache.dirty_chunks_scanned == chunks && cache.clean_chunks_skipped == chunks,
          "dirty=%" PRIu64 " clean=%" PRIu64,
          cache.dirty_chunks_scanned, cache.clean_chunks_skipped);
    rdna4_as_match_set_destroy(&matches);

    // 3. Partial single-chunk add at chunk 1000.
    const uint64_t off_1000 = 1000u * RDNA4_AS_INC_CHUNK_BYTES + 16u;
    write_u64(buf, off_1000, live_tokens[9]);
    rdna4_as_inc_cache_mark_dirty(&cache, off_1000, 8u);
    CHECK("add match_set_init", rdna4_as_match_set_init(&matches, live_n), "init failed");
    oracle_scan_work(buf, len, &full, live_tokens, live_n, oracle);
    CHECK("16 MiB partial add scan OK",
          rdna4_as_inc_cache_scan_work(&cache, buf, len, &full, &live, false, &matches) == RDNA4_AS_OK,
          "scan failed");
    CHECK("16 MiB partial add matches oracle",
          match_set_equals_oracle(&matches, oracle, live_n, &mm),
          "mismatch at idx %zu", mm);
    CHECK("16 MiB partial add scanned exactly 1 dirty chunk",
          cache.dirty_chunks_scanned == chunks + 1u &&
          cache.clean_chunks_skipped == chunks + (chunks - 1u),
          "dirty=%" PRIu64 " clean=%" PRIu64,
          cache.dirty_chunks_scanned, cache.clean_chunks_skipped);
    rdna4_as_match_set_destroy(&matches);

    // 4. Overwrite chunk 1000 token with a different live token.
    write_u64(buf, off_1000, live_tokens[12]);
    rdna4_as_inc_cache_mark_dirty(&cache, off_1000, 8u);
    CHECK("overwrite match_set_init", rdna4_as_match_set_init(&matches, live_n), "init failed");
    oracle_scan_work(buf, len, &full, live_tokens, live_n, oracle);
    CHECK("16 MiB partial overwrite scan OK",
          rdna4_as_inc_cache_scan_work(&cache, buf, len, &full, &live, false, &matches) == RDNA4_AS_OK,
          "scan failed");
    CHECK("16 MiB partial overwrite matches oracle",
          match_set_equals_oracle(&matches, oracle, live_n, &mm),
          "mismatch at idx %zu", mm);
    CHECK("16 MiB overwrite dropped old token 9 and found 12",
          !rdna4_as_match_set_has(&matches, 9) && rdna4_as_match_set_has(&matches, 12),
          "has9=%d has12=%d",
          rdna4_as_match_set_has(&matches, 9), rdna4_as_match_set_has(&matches, 12));
    rdna4_as_match_set_destroy(&matches);

    // 5. Remove chunk 1000 token.
    write_u64(buf, off_1000, 0);
    rdna4_as_inc_cache_mark_dirty(&cache, off_1000, 8u);
    CHECK("remove match_set_init", rdna4_as_match_set_init(&matches, live_n), "init failed");
    oracle_scan_work(buf, len, &full, live_tokens, live_n, oracle);
    CHECK("16 MiB partial remove scan OK",
          rdna4_as_inc_cache_scan_work(&cache, buf, len, &full, &live, false, &matches) == RDNA4_AS_OK,
          "scan failed");
    CHECK("16 MiB partial remove matches oracle",
          match_set_equals_oracle(&matches, oracle, live_n, &mm),
          "mismatch at idx %zu", mm);
    rdna4_as_match_set_destroy(&matches);

    // 6. Cross-chunk dirty span covering byte 4088..4104 (chunk 0 + chunk 1).
    uint64_t dirty_before = cache.dirty_chunks_scanned;
    write_u64(buf, 4088, live_tokens[14]);
    write_u64(buf, 4096, live_tokens[15]);
    rdna4_as_inc_cache_mark_dirty(&cache, 4088, 16u);
    CHECK("cross-chunk match_set_init", rdna4_as_match_set_init(&matches, live_n), "init failed");
    oracle_scan_work(buf, len, &full, live_tokens, live_n, oracle);
    CHECK("cross-chunk scan OK",
          rdna4_as_inc_cache_scan_work(&cache, buf, len, &full, &live, false, &matches) == RDNA4_AS_OK,
          "scan failed");
    CHECK("cross-chunk scan matches oracle",
          match_set_equals_oracle(&matches, oracle, live_n, &mm),
          "mismatch at idx %zu", mm);
    CHECK("cross-chunk rescanned exactly 2 chunks",
          cache.dirty_chunks_scanned == dirty_before + 2u,
          "delta=%" PRIu64, cache.dirty_chunks_scanned - dirty_before);
    rdna4_as_match_set_destroy(&matches);

    // 7. Coherent scan timing on the same 16 MiB buffer.
    CHECK("coherent bench match_set_init", rdna4_as_match_set_init(&matches, live_n), "init failed");
    double tc0 = now_ms();
    CHECK("16 MiB coherent scan OK",
          rdna4_as_inc_cache_scan_work(&cache, buf, len, &full, &live, true, &matches) == RDNA4_AS_OK,
          "coherent scan failed");
    double coh_ms = now_ms() - tc0;
    CHECK("16 MiB coherent scan matches oracle",
          match_set_equals_oracle(&matches, oracle, live_n, &mm),
          "mismatch at idx %zu", mm);
    rdna4_as_match_set_destroy(&matches);

    // 8. mark_all_dirty invalidates all 4096 chunks.
    uint64_t dirty_before_all = cache.dirty_chunks_scanned;
    rdna4_as_inc_cache_mark_all_dirty(&cache);
    CHECK("mark_all match_set_init", rdna4_as_match_set_init(&matches, live_n), "init failed");
    CHECK("mark_all scan OK",
          rdna4_as_inc_cache_scan_work(&cache, buf, len, &full, &live, false, &matches) == RDNA4_AS_OK,
          "scan failed");
    CHECK("mark_all rescanned all 4096 chunks",
          cache.dirty_chunks_scanned == dirty_before_all + chunks,
          "delta=%" PRIu64, cache.dirty_chunks_scanned - dirty_before_all);
    rdna4_as_match_set_destroy(&matches);

    printf("info: 16 MiB warm incremental=%.3f ms, coherent full=%.3f ms\n", warm_ms, coh_ms);

    rdna4_as_inc_cache_destroy(&cache);
    free(buf);
}

static void test_partial_work_and_unaligned_tail(void) {
    // 3 chunks + 20 leftover bytes (2 full 8-byte words + 4 trailing bytes).
    const uint64_t len = 3u * RDNA4_AS_INC_CHUNK_BYTES + 20u;
    uint8_t *buf = (uint8_t *)calloc(1, len);
    CHECK("alloc unaligned tail buf", buf != NULL, "calloc failed");
    if (!buf) return;

    const size_t live_n = 8;
    uint64_t live_tokens[8];
    rdna4_as_live_entry entries[8];
    for (size_t i = 0; i < live_n; ++i) {
        live_tokens[i] = make_token(500u + (uint64_t)i);
        entries[i] = (rdna4_as_live_entry){ .token = live_tokens[i], .value_index = (uint32_t)i };
    }
    rdna4_as_live_table live;
    CHECK("partial work live build",
          rdna4_as_live_table_build(&live, entries, live_n) == RDNA4_AS_OK, "build failed");

    // Place tokens in chunk 0 (before subrange), chunk 0 (inside subrange),
    // chunk 1 (full middle), chunk 2 (inside trailing partial), chunk 2 (after subrange),
    // and chunk 3 (unaligned tail word 0 & word 1).
    write_u64(buf, 16u, live_tokens[0]);                  // outside subrange
    write_u64(buf, 512u, live_tokens[1]);                 // inside leading partial
    write_u64(buf, 4096u + 64u, live_tokens[2]);          // middle full chunk
    write_u64(buf, 2u * 4096u + 32u, live_tokens[3]);     // inside trailing partial
    write_u64(buf, 2u * 4096u + 1024u, live_tokens[4]);   // outside subrange
    write_u64(buf, 3u * 4096u + 0u, live_tokens[5]);      // tail chunk word 0
    write_u64(buf, 3u * 4096u + 8u, live_tokens[6]);      // tail chunk word 1

    rdna4_as_inc_cache cache;
    CHECK("tail cache init",
          rdna4_as_inc_cache_init(&cache, len) == RDNA4_AS_OK && cache.chunk_count == 4u,
          "init failed");

    // Subrange [256, 2*4096 + 256): partial chunk 0, full chunk 1, partial chunk 2.
    rdna4_as_work sub = make_dense_work(256u, (2u * 4096u) / RDNA4_AS_TOKEN_BYTES);
    bool oracle[8];
    rdna4_as_match_set matches;
    size_t mm = 0;

    CHECK("sub match init", rdna4_as_match_set_init(&matches, live_n), "init failed");
    oracle_scan_work(buf, len, &sub, live_tokens, live_n, oracle);
    CHECK("subrange scan OK",
          rdna4_as_inc_cache_scan_work(&cache, buf, len, &sub, &live, false, &matches) == RDNA4_AS_OK,
          "scan failed");
    CHECK("subrange scan matches oracle without leaking out-of-range tokens",
          match_set_equals_oracle(&matches, oracle, live_n, &mm),
          "mismatch at idx %zu", mm);
    CHECK("subrange refreshed only full middle chunk (chunk 1), kept chunk 0 & 2 dirty",
          cache.dirty_chunks_scanned == 1u &&
          (cache.dirty_bits[0] & 1u) != 0 &&
          (cache.dirty_bits[0] & 2u) == 0 &&
          (cache.dirty_bits[0] & 4u) != 0,
          "dirty_scanned=%" PRIu64 " bits=0x%" PRIx64,
          cache.dirty_chunks_scanned, cache.dirty_bits[0]);
    rdna4_as_match_set_destroy(&matches);

    // Now scan full buffer [0, len - (len%8)): must discover chunk 0 token 0,
    // chunk 2 token 4, and tail chunk tokens 5 & 6!
    rdna4_as_work full = make_dense_work(0, len / RDNA4_AS_TOKEN_BYTES);
    CHECK("full tail match init", rdna4_as_match_set_init(&matches, live_n), "init failed");
    oracle_scan_work(buf, len, &full, live_tokens, live_n, oracle);
    CHECK("full tail scan OK",
          rdna4_as_inc_cache_scan_work(&cache, buf, len, &full, &live, false, &matches) == RDNA4_AS_OK,
          "scan failed");
    CHECK("full tail scan matches oracle (includes tail words & preserves chunk 0/2 dirty)",
          match_set_equals_oracle(&matches, oracle, live_n, &mm),
          "mismatch at idx %zu", mm);
    CHECK("full tail scan skipped already-clean chunk 1 and refreshed chunks 0, 2, 3",
          cache.dirty_chunks_scanned == 4u && cache.clean_chunks_skipped == 1u,
          "dirty=%" PRIu64 " clean=%" PRIu64,
          cache.dirty_chunks_scanned, cache.clean_chunks_skipped);
    rdna4_as_match_set_destroy(&matches);

    // Single-chunk subrange inside chunk 0 [64, 600): must not leak token at offset 16.
    rdna4_as_work single = make_dense_work(64u, (600u - 64u) / RDNA4_AS_TOKEN_BYTES);
    CHECK("single match init", rdna4_as_match_set_init(&matches, live_n), "init failed");
    oracle_scan_work(buf, len, &single, live_tokens, live_n, oracle);
    CHECK("single-chunk subrange OK",
          rdna4_as_inc_cache_scan_work(&cache, buf, len, &single, &live, false, &matches) == RDNA4_AS_OK,
          "scan failed");
    CHECK("single-chunk subrange matches oracle",
          match_set_equals_oracle(&matches, oracle, live_n, &mm),
          "mismatch at idx %zu", mm);
    rdna4_as_match_set_destroy(&matches);

    rdna4_as_inc_cache_destroy(&cache);
    free(buf);
}

static void test_coherent_shared_direct_writes(void) {
    // Simulates MTLStorageModeShared / coherent Managed buffers where the app
    // writes directly via [buf contents] WITHOUT calling didModifyRange:.
    const uint64_t len = 4u * 1024u * 1024u;
    uint8_t *buf = (uint8_t *)calloc(1, len);
    CHECK("alloc 4 MiB shared buf", buf != NULL, "calloc failed");
    if (!buf) return;

    const size_t live_n = 12;
    uint64_t live_tokens[12];
    rdna4_as_live_entry entries[12];
    for (size_t i = 0; i < live_n; ++i) {
        live_tokens[i] = make_token(2000u + (uint64_t)i * 3u);
        entries[i] = (rdna4_as_live_entry){ .token = live_tokens[i], .value_index = (uint32_t)i };
    }
    rdna4_as_live_table live;
    CHECK("shared live build",
          rdna4_as_live_table_build(&live, entries, live_n) == RDNA4_AS_OK, "build failed");

    rdna4_as_inc_cache cache;
    CHECK("shared cache init",
          rdna4_as_inc_cache_init(&cache, len) == RDNA4_AS_OK, "init failed");

    rdna4_as_work full = make_dense_work(0, len / RDNA4_AS_TOKEN_BYTES);
    bool oracle[12];
    rdna4_as_match_set matches;
    size_t mm = 0;

    // Prime cache clean via non-coherent scan (all dirty_bits = 0).
    CHECK("prime match init", rdna4_as_match_set_init(&matches, live_n), "init failed");
    CHECK("prime scan OK",
          rdna4_as_inc_cache_scan_work(&cache, buf, len, &full, &live, false, &matches) == RDNA4_AS_OK,
          "prime failed");
    rdna4_as_match_set_destroy(&matches);

    // Direct CPU writes bypassing mark_dirty (mimicking raw [buf contents] writes).
    write_u64(buf, 777u * 4096u + 128u, live_tokens[3]);
    write_u64(buf, 1023u * 4096u + 4088u, live_tokens[11]);
    CHECK("shared add match init", rdna4_as_match_set_init(&matches, live_n), "init failed");
    oracle_scan_work(buf, len, &full, live_tokens, live_n, oracle);
    CHECK("shared direct write scan OK",
          rdna4_as_inc_cache_scan_work(&cache, buf, len, &full, &live, true, &matches) == RDNA4_AS_OK,
          "scan failed");
    CHECK("shared direct writes detected immediately without didModifyRange:",
          match_set_equals_oracle(&matches, oracle, live_n, &mm),
          "mismatch at idx %zu", mm);
    CHECK("coherent_words_scanned accounted",
          cache.coherent_words_scanned == len / RDNA4_AS_TOKEN_BYTES,
          "words=%" PRIu64, cache.coherent_words_scanned);
    rdna4_as_match_set_destroy(&matches);

    // Direct overwrite + removal without mark_dirty.
    write_u64(buf, 777u * 4096u + 128u, live_tokens[8]);
    write_u64(buf, 1023u * 4096u + 4088u, 0);
    CHECK("shared mutate match init", rdna4_as_match_set_init(&matches, live_n), "init failed");
    oracle_scan_work(buf, len, &full, live_tokens, live_n, oracle);
    CHECK("shared direct mutate scan OK",
          rdna4_as_inc_cache_scan_work(&cache, buf, len, &full, &live, true, &matches) == RDNA4_AS_OK,
          "scan failed");
    CHECK("shared direct overwrite & removal match oracle 100%",
          match_set_equals_oracle(&matches, oracle, live_n, &mm),
          "mismatch at idx %zu", mm);
    rdna4_as_match_set_destroy(&matches);

    rdna4_as_inc_cache_destroy(&cache);
    free(buf);
}

static void test_strided_and_unaligned_offset(void) {
    const uint64_t len = 128u * 1024u;
    uint8_t *buf = (uint8_t *)calloc(1, len);
    CHECK("alloc strided buf", buf != NULL, "calloc failed");
    if (!buf) return;

    const size_t live_n = 6;
    uint64_t live_tokens[6];
    rdna4_as_live_entry entries[6];
    for (size_t i = 0; i < live_n; ++i) {
        live_tokens[i] = make_token(3000u + (uint64_t)i);
        entries[i] = (rdna4_as_live_entry){ .token = live_tokens[i], .value_index = (uint32_t)i };
    }
    rdna4_as_live_table live;
    CHECK("strided live build",
          rdna4_as_live_table_build(&live, entries, live_n) == RDNA4_AS_OK, "build failed");

    // Place tokens on-stride (offset = 4 + k*32) and off-stride (offset = 12 + k*32).
    write_u64(buf, 4u + 0u * 32u, live_tokens[0]);
    write_u64(buf, 12u + 0u * 32u, live_tokens[1]); // off-stride decoy
    write_u64(buf, 4u + 150u * 32u, live_tokens[2]);
    write_u64(buf, 4u + 4095u * 32u, live_tokens[3]);

    rdna4_as_work work = {
        .root = 0,
        .kind = RDNA4_AS_KIND_SAMPLER,
        .flags = 0,
        .mode = RDNA4_AS_SCAN_CPU_MAPPED,
        .offset = 4u,
        .stride = 32u,
        .count = 4096u,
        .width = RDNA4_AS_TOKEN_BYTES,
    };

    rdna4_as_inc_cache cache;
    CHECK("strided cache init",
          rdna4_as_inc_cache_init(&cache, len) == RDNA4_AS_OK, "init failed");

    bool oracle[6];
    oracle_scan_work(buf, len, &work, live_tokens, live_n, oracle);

    for (int mode = 0; mode < 2; ++mode) {
        rdna4_as_match_set matches;
        size_t mm = 0;
        CHECK("strided match init", rdna4_as_match_set_init(&matches, live_n), "init failed");
        CHECK("strided scan OK",
              rdna4_as_inc_cache_scan_work(&cache, buf, len, &work, &live, mode == 1, &matches) == RDNA4_AS_OK,
              "scan failed mode=%d", mode);
        CHECK("strided unaligned-offset scan matches oracle",
              match_set_equals_oracle(&matches, oracle, live_n, &mm),
              "mode=%d mismatch idx=%zu", mode, mm);
        rdna4_as_match_set_destroy(&matches);
    }

    rdna4_as_inc_cache_destroy(&cache);
    free(buf);
}

static void test_wide_span_and_large_live_set(void) {
    // > 512 live samplers (exercises heap-allocated rdna4_as_match_set) AND
    // span >= 4096 (exercises binary-search path without bitset).
    const size_t live_n = 700;
    uint64_t *live_tokens = (uint64_t *)calloc(live_n, sizeof(*live_tokens));
    rdna4_as_live_entry *entries = (rdna4_as_live_entry *)calloc(live_n, sizeof(*entries));
    bool *oracle = (bool *)calloc(live_n, sizeof(*oracle));
    const uint64_t len = 256u * 1024u;
    uint8_t *buf = (uint8_t *)calloc(1, len);
    CHECK("alloc wide span buffers",
          live_tokens && entries && oracle && buf, "alloc failed");
    if (!live_tokens || !entries || !oracle || !buf) goto done;

    for (size_t i = 0; i < live_n; ++i) {
        // Deliberately unsorted input to verify qsort ordering.
        size_t perm = (i * 293u + 17u) % live_n;
        live_tokens[i] = make_token(UINT64_C(1) + (uint64_t)perm * UINT64_C(4099));
        entries[i] = (rdna4_as_live_entry){ .token = live_tokens[i], .value_index = (uint32_t)i };
    }
    rdna4_as_live_table live;
    CHECK("wide live table build",
          rdna4_as_live_table_build(&live, entries, live_n) == RDNA4_AS_OK && !live.use_bitset,
          "build failed or unexpected bitset");

    // Seed 70 scattered live tokens + near-miss decoys (token +/- 1).
    for (size_t k = 0; k < 70; ++k) {
        size_t idx = (k * 11u) % live_n;
        uint64_t base = (uint64_t)k * 3072u;
        write_u64(buf, base, live_tokens[idx]);
        write_u64(buf, base + 8u, live_tokens[idx] + 1u);
        write_u64(buf, base + 16u, live_tokens[idx] - 1u);
    }

    rdna4_as_inc_cache cache;
    CHECK("wide cache init",
          rdna4_as_inc_cache_init(&cache, len) == RDNA4_AS_OK, "init failed");

    rdna4_as_work full = make_dense_work(0, len / RDNA4_AS_TOKEN_BYTES);
    oracle_scan_work(buf, len, &full, live_tokens, live_n, oracle);

    for (int pass = 0; pass < 3; ++pass) {
        rdna4_as_match_set matches;
        size_t mm = 0;
        CHECK("wide match init",
              rdna4_as_match_set_init(&matches, live_n) && matches.bits != matches.inline_bits,
              "expected heap match_set for 700 samplers");
        bool coherent = (pass == 2);
        CHECK("wide scan OK",
              rdna4_as_inc_cache_scan_work(&cache, buf, len, &full, &live, coherent, &matches) == RDNA4_AS_OK,
              "pass=%d failed", pass);
        CHECK("wide scan matches oracle",
              match_set_equals_oracle(&matches, oracle, live_n, &mm),
              "pass=%d mismatch idx=%zu", pass, mm);
        rdna4_as_match_set_destroy(&matches);
    }

    rdna4_as_inc_cache_destroy(&cache);
done:
    free(buf);
    free(oracle);
    free(entries);
    free(live_tokens);
}

static void test_fuzz_equivalence(void) {
    const uint64_t len = 64u * 1024u;
    uint8_t *buf = (uint8_t *)calloc(1, len);
    CHECK("alloc fuzz buf", buf != NULL, "calloc failed");
    if (!buf) return;

    const size_t live_n = 32;
    uint64_t live_tokens[32];
    rdna4_as_live_entry entries[32];
    for (size_t i = 0; i < live_n; ++i) {
        live_tokens[i] = make_token(50000u + (uint64_t)i * 19u);
        entries[i] = (rdna4_as_live_entry){ .token = live_tokens[i], .value_index = (uint32_t)i };
    }
    rdna4_as_live_table live;
    CHECK("fuzz live build",
          rdna4_as_live_table_build(&live, entries, live_n) == RDNA4_AS_OK, "build failed");

    rdna4_as_inc_cache cache;
    CHECK("fuzz cache init",
          rdna4_as_inc_cache_init(&cache, len) == RDNA4_AS_OK, "init failed");

    uint64_t rng = UINT64_C(0x9e3779b97f4a7c15);
    bool oracle[32];
    bool all_ok = true;
    for (int round = 0; round < 200 && all_ok; ++round) {
        // Apply 1..8 random word mutations.
        int nmut = 1 + (int)(prng_next(&rng) % 8u);
        bool coherent_only = (round % 4 == 3);
        for (int m = 0; m < nmut; ++m) {
            uint64_t word_idx = prng_next(&rng) % (len / RDNA4_AS_TOKEN_BYTES);
            uint64_t off = word_idx * RDNA4_AS_TOKEN_BYTES;
            uint64_t kind = prng_next(&rng) % 4u;
            uint64_t val = 0;
            if (kind == 0) val = live_tokens[prng_next(&rng) % live_n];
            else if (kind == 1) val = make_token(999999u + (prng_next(&rng) & 0xfffu));
            else if (kind == 2) val = prng_next(&rng);
            write_u64(buf, off, val);
            if (!coherent_only)
                rdna4_as_inc_cache_mark_dirty(&cache, off, RDNA4_AS_TOKEN_BYTES);
        }

        // Choose either full buffer or random aligned subrange.
        uint64_t start_word = (round % 3 == 0) ? 0u
            : (prng_next(&rng) % (len / RDNA4_AS_TOKEN_BYTES));
        uint64_t max_words = (len / RDNA4_AS_TOKEN_BYTES) - start_word;
        uint64_t count = (round % 3 == 0) ? max_words
            : (1u + (prng_next(&rng) % max_words));
        rdna4_as_work work = make_dense_work(start_word * RDNA4_AS_TOKEN_BYTES, count);

        oracle_scan_work(buf, len, &work, live_tokens, live_n, oracle);
        rdna4_as_match_set matches;
        if (!rdna4_as_match_set_init(&matches, live_n)) {
            all_ok = false;
            break;
        }
        rdna4_as_status st = rdna4_as_inc_cache_scan_work(
            &cache, buf, len, &work, &live, coherent_only, &matches);
        size_t mm = 0;
        if (st != RDNA4_AS_OK || !match_set_equals_oracle(&matches, oracle, live_n, &mm)) {
            fprintf(stderr, "fuzz mismatch round=%d coherent=%d st=%d idx=%zu\n",
                    round, coherent_only ? 1 : 0, (int)st, mm);
            all_ok = false;
        }
        rdna4_as_match_set_destroy(&matches);
        if (coherent_only) {
            // Re-sync cache dirty state after bypassing mark_dirty in coherent round.
            rdna4_as_inc_cache_mark_all_dirty(&cache);
        }
    }
    CHECK("200-round fuzz equivalence matches oracle 100%", all_ok, "fuzz failed");

    rdna4_as_inc_cache_destroy(&cache);
    free(buf);
}

static char *read_source_file(const char *name) {
    char path[512];
    snprintf(path, sizeof(path), "src/garnet-metal/%s", name);
    FILE *f = fopen(path, "rb");
    if (!f) f = fopen(name, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    char *buf = (char *)malloc((size_t)n + 1u);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

static void test_source_pins(void) {
    char *args = read_source_file("garnet_argument_encoder.m");
    char *argHdr = read_source_file("garnet_arguments_internal.h");
    char *res = read_source_file("garnet_resources.m");
    char *hdr = read_source_file("garnet_metal_internal.h");
    CHECK("source files readable", args && argHdr && res && hdr, "missing source file(s)");
    if (args && argHdr) {
        CHECK("garnet_argument_encoder.m has zero-sampler atomic fast path",
              strstr(args, "rdna4_argument_sampler_registered") != NULL,
              "missing rdna4_argument_sampler_registered");
        CHECK("garnet_arguments_internal.h uses ARC-safe void *dictRetained in RDNA4ArgumentLiveContext",
              strstr(argHdr, "void *dictRetained;") != NULL &&
              strstr(argHdr, "NSDictionary<NSNumber *, id> *dict;") == NULL &&
              strstr(args, "NSDictionary<NSNumber *, id> *dict;") == NULL,
              "ARC-unsafe ObjC pointer in C struct");
        CHECK("garnet_argument_encoder.m verifies recorded slots in O(slots.count)",
              strstr(args, "owners.slots.allKeys") != NULL,
              "missing O(slots) verification");
        CHECK("garnet_argument_encoder.m removes per-word ObjC visitor rdna4_argument_scope_visit_token",
              strstr(args, "rdna4_argument_scope_visit_token") == NULL,
              "old per-word ObjC visitor still present");
        CHECK("garnet_argument_encoder.m uses pure-C strided/incremental scanners in scope work",
              strstr(args, "rdna4_as_inc_cache_scan_work(") != NULL &&
              strstr(args, "rdna4_as_scan_strided_words(") != NULL,
              "missing pure-C scanner wiring");
        CHECK("garnet_argument_encoder.m uses dedicated associated-object key for incremental cache",
              strstr(args, "rdna4_argument_inc_cache_key") != NULL &&
              strstr(args, "rdna4_argument_note_dirty_range") != NULL,
              "missing incremental cache key or dirty notifier");
    }
    if (res) {
        CHECK("garnet_resources.m wires didModifyRange: to rdna4_argument_note_dirty_range",
              strstr(res, "rdna4_argument_note_dirty_range(self, r)") != NULL,
              "didModifyRange: missing dirty notification");
    }
    if (hdr) {
        CHECK("garnet_metal_internal.h declares rdna4_argument_note_dirty_range",
              strstr(hdr, "rdna4_argument_note_dirty_range(id buffer, NSRange range)") != NULL,
              "missing extern declaration");
    }
    free(args);
    free(argHdr);
    free(res);
    free(hdr);
}

int main(void) {
    test_empty_live_set();
    test_incremental_16mb_lifecycle();
    test_partial_work_and_unaligned_tail();
    test_coherent_shared_direct_writes();
    test_strided_and_unaligned_offset();
    test_wide_span_and_large_live_set();
    test_fuzz_equivalence();
    test_source_pins();

    if (g_fails) {
        fprintf(stderr, "FAIL: %d/%d checks failed\n", g_fails, g_checks);
        return 1;
    }
    printf("PASS: test-argscan-incremental (%d checks)\n", g_checks);
    return 0;
}
