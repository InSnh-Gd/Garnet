// test-counters-amp.c: pure-C host unit tests + source pins for Task 2.5
// (GPU Timestamp Counter Sampling and Vertex Amplification validation).
// Build/run:
//   cc -std=c11 -D_GNU_SOURCE -O2 -Wall -Wextra -Werror -Isrc/garnet-metal -o "$TMPDIR/test-counters-amp" src/garnet-metal/test-counters-amp.c
//   "$TMPDIR/test-counters-amp" [assembled-dir] [src-dir]
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "garnet_counters.h"

static int fails, checks;
#define CHECK(name, cond, ...) do { \
    int ok_ = (cond) ? 1 : 0; \
    checks++; \
    printf("%s %s: ", ok_ ? "ok  " : "FAIL", name); \
    printf(__VA_ARGS__); \
    printf("\n"); \
    if (!ok_) fails++; \
} while (0)

static char *slurp(const char *dir, const char *name) {
    char p[1024];
    int n = snprintf(p, sizeof p, "%s/%s", dir, name);
    if (n <= 0 || (size_t)n >= sizeof p) return NULL;
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    char *buf = (char *)calloc(1, (size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(buf); return NULL; }
    fclose(f);
    return buf;
}

static void test_sampling_point_gating(void) {
    rdna4dc_metal3_caps off = rdna4dc_metal3_fallback();
    CHECK("sampling points off for NULL and fallback caps",
          !rdna4cnt_supports_sampling_point(NULL, RDNA4CNT_SP_STAGE) &&
          !rdna4cnt_supports_sampling_point(&off, RDNA4CNT_SP_STAGE) &&
          !rdna4cnt_supports_sampling_point(&off, RDNA4CNT_SP_DRAW) &&
          !rdna4cnt_supports_sampling_point(&off, RDNA4CNT_SP_DISPATCH) &&
          !rdna4cnt_supports_sampling_point(&off, RDNA4CNT_SP_TILE_DISPATCH) &&
          !rdna4cnt_supports_sampling_point(&off, RDNA4CNT_SP_BLIT),
          "conservative off");

    rdna4dc_metal3_probe probe = {
        .timestamp_query_funcs = 1,
        .timestamp_compute_and_graphics = 1,
        .timestamp_period_ns = 10.0f,
        .timestamp_valid_bits = 64u,
    };
    rdna4dc_metal3_caps on = rdna4dc_metal3_from_probe(&probe);
    CHECK("probed timestamps gate Stage/Draw/Dispatch/Blit on and Tile/unknown off",
          rdna4cnt_supports_sampling_point(&on, RDNA4CNT_SP_STAGE) &&
          rdna4cnt_supports_sampling_point(&on, RDNA4CNT_SP_DRAW) &&
          rdna4cnt_supports_sampling_point(&on, RDNA4CNT_SP_DISPATCH) &&
          rdna4cnt_supports_sampling_point(&on, RDNA4CNT_SP_BLIT) &&
          !rdna4cnt_supports_sampling_point(&on, RDNA4CNT_SP_TILE_DISPATCH) &&
          !rdna4cnt_supports_sampling_point(&on, 5u) &&
          !rdna4cnt_supports_sampling_point(&on, UINT64_MAX),
          "EX04 tile stays NO");

    rdna4dc_metal3_probe bad_bits = probe;
    bad_bits.timestamp_valid_bits = 0u;
    rdna4dc_metal3_caps mc_bad_bits = rdna4dc_metal3_from_probe(&bad_bits);
    rdna4dc_metal3_probe bad_period = probe;
    bad_period.timestamp_period_ns = 0.0f;
    rdna4dc_metal3_caps mc_bad_period = rdna4dc_metal3_from_probe(&bad_period);
    CHECK("invalid validBits or period keeps all sampling points off",
          !rdna4cnt_supports_sampling_point(&mc_bad_bits, RDNA4CNT_SP_STAGE) &&
          !rdna4cnt_supports_sampling_point(&mc_bad_bits, RDNA4CNT_SP_DRAW) &&
          !rdna4cnt_supports_sampling_point(&mc_bad_period, RDNA4CNT_SP_DISPATCH) &&
          !rdna4cnt_supports_sampling_point(&mc_bad_period, RDNA4CNT_SP_BLIT),
          "fail closed");
}

static void test_descriptor_and_bounds(void) {
    const char *why = NULL;
    CHECK("desc valid Shared/Private edge counts",
          rdna4cnt_validate_desc(1u, RDNA4CNT_STORAGE_SHARED, &why) == RDNA4CNT_DESC_OK && why == NULL &&
          rdna4cnt_validate_desc(RDNA4CNT_MAX_SAMPLES, RDNA4CNT_STORAGE_SHARED, &why) == RDNA4CNT_DESC_OK &&
          rdna4cnt_validate_desc(16u, RDNA4CNT_STORAGE_PRIVATE, &why) == RDNA4CNT_DESC_OK,
          "Shared & Private accepted");

    CHECK("desc rejects 0, oversized, Managed, Memoryless, unknown storage",
          rdna4cnt_validate_desc(0u, RDNA4CNT_STORAGE_SHARED, &why) == RDNA4CNT_DESC_INVALID && why != NULL &&
          rdna4cnt_validate_desc(8u, RDNA4CNT_STORAGE_MANAGED, &why) == RDNA4CNT_DESC_INVALID &&
          rdna4cnt_validate_desc(8u, RDNA4CNT_STORAGE_MEMORYLESS, &why) == RDNA4CNT_DESC_INVALID &&
          rdna4cnt_validate_desc(8u, 99u, &why) == RDNA4CNT_DESC_INVALID &&
          rdna4cnt_validate_desc((uint64_t)RDNA4CNT_MAX_SAMPLES + 1u, RDNA4CNT_STORAGE_SHARED, &why) == RDNA4CNT_DESC_OOM &&
          rdna4cnt_validate_desc(UINT64_MAX, RDNA4CNT_STORAGE_PRIVATE, &why) == RDNA4CNT_DESC_OOM,
          "invalid descriptors rejected");

    CHECK("cpu resolve allowed only for Shared",
          rdna4cnt_resolve_cpu_allowed(RDNA4CNT_STORAGE_SHARED) &&
          !rdna4cnt_resolve_cpu_allowed(RDNA4CNT_STORAGE_PRIVATE) &&
          !rdna4cnt_resolve_cpu_allowed(RDNA4CNT_STORAGE_MANAGED),
          "storage mode check");

    CHECK("sample index & pass index bounds",
          rdna4cnt_index_ok(8u, 0u) && rdna4cnt_index_ok(8u, 7u) &&
          !rdna4cnt_index_ok(8u, 8u) && !rdna4cnt_index_ok(8u, RDNA4CNT_DONT_SAMPLE) &&
          !rdna4cnt_index_ok(0u, 0u) && !rdna4cnt_index_ok((uint64_t)RDNA4CNT_MAX_SAMPLES + 1u, 0u) &&
          rdna4cnt_pass_index_ok(8u, RDNA4CNT_DONT_SAMPLE) &&
          rdna4cnt_pass_index_ok(8u, 0u) && rdna4cnt_pass_index_ok(8u, 7u) &&
          !rdna4cnt_pass_index_ok(8u, 8u),
          "index validation");

    CHECK("resolveCounterRange bounds and overflow rejection",
          rdna4cnt_range_ok(8u, 0u, 8u) && rdna4cnt_range_ok(8u, 0u, 1u) &&
          rdna4cnt_range_ok(8u, 7u, 1u) && rdna4cnt_range_ok(8u, 3u, 5u) &&
          !rdna4cnt_range_ok(8u, 0u, 0u) && !rdna4cnt_range_ok(8u, 8u, 0u) &&
          !rdna4cnt_range_ok(8u, 8u, 1u) && !rdna4cnt_range_ok(8u, 7u, 2u) &&
          !rdna4cnt_range_ok(8u, UINT64_MAX, 1u) && !rdna4cnt_range_ok(8u, 1u, UINT64_MAX) &&
          !rdna4cnt_range_ok(8u, UINT64_MAX - 2u, 4u) && !rdna4cnt_range_ok(0u, 0u, 1u),
          "OOB and wraparound refused");
}

static void test_pool_accounting(void) {
    rdna4cnt_pool_state st;
    CHECK("pool_init rejects bad params",
          !rdna4cnt_pool_init(NULL, 8u, RDNA4CNT_STORAGE_SHARED, 64u, 10.0) &&
          !rdna4cnt_pool_init(&st, 0u, RDNA4CNT_STORAGE_SHARED, 64u, 10.0) &&
          !rdna4cnt_pool_init(&st, 8u, RDNA4CNT_STORAGE_MANAGED, 64u, 10.0) &&
          !rdna4cnt_pool_init(&st, 8u, RDNA4CNT_STORAGE_SHARED, 0u, 10.0) &&
          !rdna4cnt_pool_init(&st, 8u, RDNA4CNT_STORAGE_SHARED, 65u, 10.0) &&
          !rdna4cnt_pool_init(&st, 8u, RDNA4CNT_STORAGE_SHARED, 64u, 0.0) &&
          !rdna4cnt_pool_init(&st, 8u, RDNA4CNT_STORAGE_SHARED, 64u, NAN),
          "bad pool configs rejected");

    CHECK("pool_init 130 slots across bitmap byte boundaries",
          rdna4cnt_pool_init(&st, 130u, RDNA4CNT_STORAGE_SHARED, 64u, 10.0) &&
          st.sampled_bytes == 17u && rdna4cnt_slot_sampled_count(&st) == 0u,
          "bytes=%zu", st.sampled_bytes);

    const uint32_t marks[] = { 0u, 7u, 8u, 63u, 64u, 129u };
    int mark_ok = 1;
    for (size_t i = 0; i < sizeof marks / sizeof marks[0]; i++)
        mark_ok &= rdna4cnt_slot_mark_sampled(&st, marks[i]);
    CHECK("slot mark/check/OOB rejection",
          mark_ok && !rdna4cnt_slot_mark_sampled(&st, 130u) &&
          rdna4cnt_slot_sampled_count(&st) == 6u &&
          rdna4cnt_slot_is_sampled(&st, 0u) && rdna4cnt_slot_is_sampled(&st, 7u) &&
          rdna4cnt_slot_is_sampled(&st, 8u) && rdna4cnt_slot_is_sampled(&st, 63u) &&
          rdna4cnt_slot_is_sampled(&st, 64u) && rdna4cnt_slot_is_sampled(&st, 129u) &&
          !rdna4cnt_slot_is_sampled(&st, 1u) && !rdna4cnt_slot_is_sampled(&st, 9u) &&
          !rdna4cnt_slot_is_sampled(&st, 130u),
          "sampled=%u", rdna4cnt_slot_sampled_count(&st));

    CHECK("slot clear preserves neighbors",
          rdna4cnt_slot_clear(&st, 8u) && !rdna4cnt_slot_clear(&st, 130u) &&
          !rdna4cnt_slot_is_sampled(&st, 8u) && rdna4cnt_slot_is_sampled(&st, 7u) &&
          rdna4cnt_slot_is_sampled(&st, 63u) && rdna4cnt_slot_sampled_count(&st) == 5u,
          "sampled=%u", rdna4cnt_slot_sampled_count(&st));

    rdna4cnt_pool_destroy(&st);
    CHECK("pool_destroy zeroes state cleanly",
          st.sampled_bits == NULL && st.sample_count == 0u && !rdna4cnt_slot_is_sampled(&st, 0u),
          "cleaned");
}

static void test_timestamp_scaling_and_masking(void) {
    // Valid-bits masking (1..64, no shift-by-64 UB).
    CHECK("mask_ticks strips invalid high bits",
          rdna4cnt_mask_ticks(UINT64_C(0xFFFFFFF000001234), 36u) == UINT64_C(0x1234) &&
          rdna4cnt_mask_ticks(UINT64_C(0xFFFFFFFFFFFFFFFF), 1u) == UINT64_C(1) &&
          rdna4cnt_mask_ticks(UINT64_C(0x123456789ABCDEF0), 32u) == UINT64_C(0x9ABCDEF0) &&
          rdna4cnt_mask_ticks(UINT64_C(0xFFFFFFFFFFFFFFFF), 63u) == UINT64_C(0x7FFFFFFFFFFFFFFF) &&
          rdna4cnt_mask_ticks(UINT64_C(0xFEDCBA9876543210), 64u) == UINT64_C(0xFEDCBA9876543210) &&
          rdna4cnt_mask_ticks(UINT64_C(1234), 0u) == 0u &&
          rdna4cnt_mask_ticks(UINT64_C(1234), 65u) == 0u,
          "valid_bits masking");

    // Exact integer period scaling, including >53-bit ticks where double loses low bits.
    const uint64_t big0 = UINT64_C(1) << 60;
    const uint64_t big1 = big0 + UINT64_C(1);
    const uint64_t big7 = big0 + UINT64_C(7);
    const uint64_t ns0 = rdna4cnt_ticks_to_ns(big0, 64u, 10.0);
    const uint64_t ns1 = rdna4cnt_ticks_to_ns(big1, 64u, 10.0);
    const uint64_t ns7 = rdna4cnt_ticks_to_ns(big7, 64u, 10.0);
    CHECK("exact 128-bit integer period preserves low bits beyond 53-bit double precision",
          rdna4cnt_ticks_to_ns(0u, 64u, 10.0) == 0u &&
          rdna4cnt_ticks_to_ns(1u, 64u, 10.0) == 10u &&
          rdna4cnt_ticks_to_ns(1000000u, 64u, 40.0) == 40000000u &&
          ns0 == big0 * UINT64_C(10) &&
          ns1 - ns0 == 10u &&
          ns7 - ns0 == 70u,
          "delta1=%llu delta7=%llu", (unsigned long long)(ns1 - ns0), (unsigned long long)(ns7 - ns0));

    // Fractional periods (96 MHz, 24 MHz, 27 MHz, sub-ns 2 GHz, carry-up edge).
    const uint64_t f96 = rdna4cnt_ticks_to_ns(96000000u, 64u, 1000.0 / 96.0);
    const uint64_t f24 = rdna4cnt_ticks_to_ns(24000000u, 64u, 1000.0 / 24.0);
    const uint64_t f27 = rdna4cnt_ticks_to_ns(27000000u, 64u, 1000.0 / 27.0);
    const uint64_t fhalf = rdna4cnt_ticks_to_ns(1000u, 64u, 0.5);
    const uint64_t fcarry = rdna4cnt_ticks_to_ns(100u, 64u, 1.9999999999999998);
    CHECK("fractional periods accurate to <= 1 ns and carry-safe",
          f96 >= 999999999u && f96 <= 1000000001u &&
          f24 >= 999999999u && f24 <= 1000000001u &&
          f27 >= 999999999u && f27 <= 1000000001u &&
          fhalf == 500u && fcarry == 200u,
          "96MHz=%llu 24MHz=%llu 27MHz=%llu half=%llu carry=%llu",
          (unsigned long long)f96, (unsigned long long)f24, (unsigned long long)f27,
          (unsigned long long)fhalf, (unsigned long long)fcarry);

    // Strict monotonicity across >53-bit ticks under fractional period.
    const uint64_t hi_base = UINT64_C(1) << 54;
    uint64_t prev = rdna4cnt_ticks_to_ns(hi_base, 64u, 1000.0 / 24.0);
    int mono_ok = (prev != RDNA4CNT_ERROR_VALUE);
    for (uint64_t k = 1; k <= 64; k++) {
        uint64_t cur = rdna4cnt_ticks_to_ns(hi_base + k, 64u, 1000.0 / 24.0);
        if (cur == RDNA4CNT_ERROR_VALUE || cur <= prev) mono_ok = 0;
        prev = cur;
    }
    CHECK("fractional scaling strictly monotonic beyond 2^54", mono_ok, "64 consecutive ticks");

    // 36-bit validBits masking applied before scaling.
    CHECK("ticks_to_ns masks high invalid bits before scaling",
          rdna4cnt_ticks_to_ns(UINT64_C(0xABCD000000000064), 36u, 10.0) == 1000u,
          "got=%llu", (unsigned long long)rdna4cnt_ticks_to_ns(UINT64_C(0xABCD000000000064), 36u, 10.0));

    // Overflow saturates to UINT64_MAX - 1 (never UB, never collides with MTLCounterErrorValue).
    CHECK("64-bit overflow saturates safely without UB or error-value collision",
          rdna4cnt_ticks_to_ns(UINT64_MAX, 64u, 10.0) == RDNA4CNT_MAX_VALID_NS &&
          rdna4cnt_ticks_to_ns(UINT64_MAX, 64u, 1.0) == RDNA4CNT_MAX_VALID_NS &&
          rdna4cnt_ticks_to_ns(UINT64_MAX, 64u, 999999999.0) == RDNA4CNT_MAX_VALID_NS &&
          rdna4cnt_ticks_to_ns(UINT64_MAX, 64u, 0.5) < RDNA4CNT_MAX_VALID_NS,
          "saturated to UINT64_MAX-1");

    // Invalid period or validBits returns RDNA4CNT_ERROR_VALUE (~0ULL).
    CHECK("invalid period or validBits returns MTLCounterErrorValue",
          rdna4cnt_ticks_to_ns(100u, 0u, 10.0) == RDNA4CNT_ERROR_VALUE &&
          rdna4cnt_ticks_to_ns(100u, 65u, 10.0) == RDNA4CNT_ERROR_VALUE &&
          rdna4cnt_ticks_to_ns(100u, 64u, 0.0) == RDNA4CNT_ERROR_VALUE &&
          rdna4cnt_ticks_to_ns(100u, 64u, -10.0) == RDNA4CNT_ERROR_VALUE &&
          rdna4cnt_ticks_to_ns(100u, 64u, (double)NAN) == RDNA4CNT_ERROR_VALUE &&
          rdna4cnt_ticks_to_ns(100u, 64u, (double)INFINITY) == RDNA4CNT_ERROR_VALUE &&
          rdna4cnt_ticks_to_ns(100u, 64u, 1e9) == RDNA4CNT_ERROR_VALUE,
          "error sentinel returned");

    // Batch resolve pairs with mixed availability.
    const uint64_t raw_pairs[8] = {
        1000u, 1u,          // slot 0: available -> 10000 ns
        9999u, 0u,          // slot 1: unavailable -> ~0ULL
        1250u, 1u,          // slot 2: available -> 12500 ns
        0u,    1u,          // slot 3: available zero tick -> 0 ns (valid, not error)
    };
    uint64_t resolved[4] = { 1, 1, 1, 1 };
    rdna4cnt_resolve_pairs(raw_pairs, 4u, 64u, 10.0, resolved);
    CHECK("resolve_pairs converts available slots and marks unavailable as error",
          resolved[0] == 10000u &&
          resolved[1] == RDNA4CNT_ERROR_VALUE &&
          resolved[2] == 12500u &&
          resolved[2] >= resolved[0] &&
          resolved[3] == 0u,
          "got=[%llu, %#llx, %llu, %llu]",
          (unsigned long long)resolved[0], (unsigned long long)resolved[1],
          (unsigned long long)resolved[2], (unsigned long long)resolved[3]);
}

static void test_vertex_amplification(void) {
    const char *why = NULL;

    // Device gating: honest baseline max=1, plus parameterized max=2 verification.
    CHECK("device supportsVertexAmplificationCount baseline max=1",
          RDNA4AMP_MAX_SUPPORTED_COUNT == 1u &&
          !rdna4amp_device_supports(0u, RDNA4AMP_MAX_SUPPORTED_COUNT) &&
          rdna4amp_device_supports(1u, RDNA4AMP_MAX_SUPPORTED_COUNT) &&
          !rdna4amp_device_supports(2u, RDNA4AMP_MAX_SUPPORTED_COUNT) &&
          !rdna4amp_device_supports(UINT64_MAX, RDNA4AMP_MAX_SUPPORTED_COUNT) &&
          !rdna4amp_device_supports(1u, 0u),
          "only count==1 supported");
    CHECK("device supportsVertexAmplificationCount parameterized max=2",
          !rdna4amp_device_supports(0u, 2u) &&
          rdna4amp_device_supports(1u, 2u) &&
          rdna4amp_device_supports(2u, 2u) &&
          !rdna4amp_device_supports(3u, 2u),
          "parameterized max=2");

    // Pipeline descriptor maxVertexAmplificationCount checks.
    CHECK("pipeline maxVertexAmplificationCount validation",
          rdna4amp_pipeline_count_ok(1u, 1u, 0, &why) && why == NULL &&
          rdna4amp_pipeline_count_ok(1u, 1u, 1, &why) &&
          !rdna4amp_pipeline_count_ok(0u, 1u, 0, &why) && why != NULL &&
          !rdna4amp_pipeline_count_ok(2u, 1u, 0, &why) && why != NULL &&
          rdna4amp_pipeline_count_ok(2u, 2u, 0, &why) &&
          !rdna4amp_pipeline_count_ok(2u, 2u, 1, &why) && why != NULL,
          "pipeline count and tessellation guard");

    // Encoder setVertexAmplificationCount:viewMappings: validation.
    const rdna4amp_mapping m00 = { 0u, 0u };
    const rdna4amp_mapping m10 = { 1u, 0u };
    const rdna4amp_mapping m01 = { 0u, 1u };
    const rdna4amp_mapping m16 = { 16u, 0u };
    const rdna4amp_mapping m2_ok[2] = { { 0u, 0u }, { 1u, 1u } };
    const rdna4amp_mapping m2_bad_vp[2] = { { 0u, 0u }, { 16u, 0u } };
    const rdna4amp_mapping m2_bad_ly[2] = { { 0u, 0u }, { 0u, 2u } };

    CHECK("encoder set accepts valid count=1 NULL and bounded mappings",
          rdna4amp_validate_encoder_set(1u, NULL, 1u, 16u, 1u, &why) &&
          rdna4amp_validate_encoder_set(1u, &m00, 1u, 1u, 1u, &why) &&
          rdna4amp_validate_encoder_set(1u, &m10, 1u, 16u, 1u, &why) &&
          rdna4amp_validate_encoder_set(1u, &m01, 1u, 16u, 2u, &why),
          "valid count=1 accepted");

    CHECK("encoder set rejects unsupported counts and OOB viewport/layer offsets",
          !rdna4amp_validate_encoder_set(0u, NULL, 1u, 16u, 2u, &why) && why != NULL &&
          !rdna4amp_validate_encoder_set(2u, NULL, 1u, 16u, 2u, &why) &&
          !rdna4amp_validate_encoder_set(2u, m2_ok, 1u, 16u, 2u, &why) &&
          !rdna4amp_validate_encoder_set(1u, &m10, 1u, 1u, 1u, &why) &&
          !rdna4amp_validate_encoder_set(1u, &m16, 1u, 16u, 1u, &why) &&
          !rdna4amp_validate_encoder_set(1u, &m01, 1u, 16u, 1u, &why),
          "invalid counts and offsets rejected");

    CHECK("encoder set validates multi-view mappings when device_max=2",
          rdna4amp_validate_encoder_set(2u, m2_ok, 2u, 16u, 2u, &why) &&
          !rdna4amp_validate_encoder_set(2u, m2_bad_vp, 2u, 16u, 2u, &why) &&
          !rdna4amp_validate_encoder_set(2u, m2_bad_ly, 2u, 16u, 2u, &why),
          "multi-view bounds checked");

    // Draw-time validation against pipeline max and bound viewports/layers.
    CHECK("draw_ok enforces pipeline max and bound viewport/layer limits",
          rdna4amp_draw_ok(1u, 1u, NULL, 1u, 1u, &why) &&
          rdna4amp_draw_ok(1u, 1u, &m10, 2u, 1u, &why) &&
          rdna4amp_draw_ok(1u, 1u, &m01, 1u, 2u, &why) &&
          !rdna4amp_draw_ok(1u, 1u, &m10, 1u, 1u, &why) &&
          !rdna4amp_draw_ok(1u, 1u, &m01, 2u, 1u, &why) &&
          !rdna4amp_draw_ok(2u, 1u, m2_ok, 2u, 2u, &why) &&
          !rdna4amp_draw_ok(0u, 1u, NULL, 1u, 1u, &why) &&
          !rdna4amp_draw_ok(1u, 0u, NULL, 1u, 1u, &why),
          "draw-time bounds enforced");

    // Viewport count shifting helper.
    CHECK("viewport_count preserves legacy unshifted count and shifts cleanly",
          rdna4amp_viewport_count(1u, 1u, 0u, 16u) == 1u &&
          rdna4amp_viewport_count(4u, 2u, 0u, 16u) == 4u &&
          rdna4amp_viewport_count(2u, 2u, 1u, 16u) == 1u &&
          rdna4amp_viewport_count(4u, 1u, 1u, 16u) == 3u &&
          rdna4amp_viewport_count(16u, 16u, 0u, 8u) == 8u,
          "viewport shift math");
}

static void test_source_pins(const char *assembled_dir, const char *src_dir) {
    char *m = slurp(assembled_dir, "GarnetDevice.m");
    CHECK("assembled GarnetDevice.m readable", m != NULL, "%s/GarnetDevice.m", assembled_dir);
    if (m) {
        CHECK("pin: render pipeline validates maxVertexAmplificationCount",
              strstr(m, "rdna4amp_pipeline_count_ok") != NULL &&
              strstr(m, "- (uint32_t)rdna4MaxVertexAmplificationCount") != NULL,
              "pipeline amplification hook");
        CHECK("pin: render encoder wires counters and vertex amplification",
              strstr(m, "- (void)setVertexAmplificationCount:(NSUInteger)count") != NULL &&
              strstr(m, "rdna4amp_validate_encoder_set") != NULL &&
              strstr(m, "rdna4amp_draw_ok") != NULL &&
              strstr(m, "render sampleCountersInBuffer") != NULL &&
              strstr(m, "render-pass-end-fragment") != NULL,
              "render encoder hooks");
        CHECK("pin: compute and blit encoders wire counter sampling and resolve",
              strstr(m, "compute sampleCountersInBuffer") != NULL &&
              strstr(m, "compute-pass-end") != NULL &&
              strstr(m, "blit sampleCountersInBuffer") != NULL &&
              strstr(m, "blit resolveCounters") != NULL &&
              strstr(m, "blit-pass-end") != NULL,
              "compute & blit encoder hooks");
        CHECK("pin: encoder hardening (endEncoding before nil, unresolved retain, null ampViews guard)",
              strstr(m, "ampViews[i] == VK_NULL_HANDLE") != NULL &&
              strstr(m, "if (unresolved) {\n                        (void)CFBridgingRetain(stg);") != NULL &&
              strstr(m, "_ended = YES;\n                [super endEncoding];\n                return nil;") != NULL,
              "verifier hardening invariants");
        free(m);
    }
    if (src_dir && src_dir[0]) {
        char *c = slurp(src_dir, "garnet_commands.m");
        CHECK("garnet_commands.m readable", c != NULL, "%s/garnet_commands.m", src_dir);
        if (c) {
            CHECK("pin: counters impl uses Vulkan 1.0 vkCmdWriteTimestamp and not synchronization2 vkCmdWriteTimestamp2",
                  strstr(c, "VK_QUERY_TYPE_TIMESTAMP") != NULL &&
                  strstr(c, "\"vkCmdWriteTimestamp\"") != NULL &&
                  strstr(c, "vkCmdWriteTimestamp2") == NULL &&
                  strstr(c, "@implementation RDNA4CounterSampleBuffer") != NULL &&
                  strstr(c, "rdna4_device_supports_counter_sampling") != NULL &&
                  strstr(c, "rdna4_device_counter_sets") != NULL &&
                  strstr(c, "rdna4_device_new_counter_sample_buffer") != NULL,
                  "Vulkan timestamp backend & Wave 2B helpers");
            free(c);
        }
    }
}

int main(int argc, char **argv) {
    test_sampling_point_gating();
    test_descriptor_and_bounds();
    test_pool_accounting();
    test_timestamp_scaling_and_masking();
    test_vertex_amplification();
    if (argc > 1 && argv[1][0])
        test_source_pins(argv[1], argc > 2 ? argv[2] : NULL);
    printf("%s test-counters-amp (%d checks, %d failed)\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails ? 1 : 0;
}
