#include "garnet_argument_summary.h"
#include "argument-summary/garnet_argument_summary_spv.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "ARGUMENT_SUMMARY_PLAN_FAIL %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
    ++checks; \
} while (0)

static unsigned checks;

static rdna4_argument_summary_plan make_plan(
    uint64_t source_size, uint32_t chunk_bytes, uint32_t candidate_count,
    uint32_t page_size, uint64_t generation) {
    rdna4_argument_summary_plan plan;
    rdna4_argument_summary_status status = rdna4_argument_summary_plan_init(
        &plan, source_size, chunk_bytes, candidate_count, page_size, generation);
    CHECK(status == RDNA4_ARGUMENT_SUMMARY_OK);
    return plan;
}

static void complete_all(rdna4_argument_summary_plan *plan,
                         uint32_t *completed, size_t words, int reverse) {
    CHECK(plan->job_count <= SIZE_MAX);
    for (uint64_t step = 0; step < plan->job_count; ++step) {
        uint64_t ordinal = reverse ? plan->job_count - 1u - step : step;
        rdna4_argument_summary_job job;
        CHECK(rdna4_argument_summary_plan_job(plan, ordinal, &job) ==
              RDNA4_ARGUMENT_SUMMARY_OK);
        CHECK(rdna4_argument_summary_plan_complete(
                  plan, &job, plan->generation, completed, words) ==
              RDNA4_ARGUMENT_SUMMARY_OK);
    }
}

static void test_candidate_abi(void) {
    CHECK(sizeof rdna4_argument_summary_cs_spv > 20);
    CHECK((sizeof rdna4_argument_summary_cs_spv) % 4u == 0);
    CHECK(rdna4_argument_summary_cs_spv[0] == 0x03);
    CHECK(rdna4_argument_summary_cs_spv[1] == 0x02);
    CHECK(rdna4_argument_summary_cs_spv[2] == 0x23);
    CHECK(rdna4_argument_summary_cs_spv[3] == 0x07);

    rdna4_argument_summary_candidate low = {
        .lo = UINT32_C(0xffffffff), .hi = UINT32_C(0x52440001),
    };
    rdna4_argument_summary_candidate high = {
        .lo = 0, .hi = UINT32_C(0x52440002),
    };
    CHECK(rdna4_argument_summary_candidate_valid(low));
    CHECK(rdna4_argument_summary_candidate_valid(high));
    CHECK(!rdna4_argument_summary_candidate_valid(
        (rdna4_argument_summary_candidate){ .lo = 0, .hi = 0x52430002 }));
    CHECK(!rdna4_argument_summary_candidate_valid(
        (rdna4_argument_summary_candidate){ .lo = 0, .hi = 0x52450000 }));
    CHECK(rdna4_argument_summary_candidate_compare(low, high) < 0);
    CHECK(rdna4_argument_summary_candidate_compare(high, low) > 0);
    CHECK(rdna4_argument_summary_candidate_compare(low, low) == 0);

    rdna4_argument_summary_candidate same_hi_a = {
        .lo = 1, .hi = UINT32_C(0x52440003),
    };
    rdna4_argument_summary_candidate same_hi_b = {
        .lo = 2, .hi = UINT32_C(0x52440003),
    };
    CHECK(rdna4_argument_summary_candidate_compare(same_hi_a, same_hi_b) < 0);

    CHECK(rdna4_argument_summary_output_words(0) == 0);
    CHECK(rdna4_argument_summary_output_words(1) == 1);
    CHECK(rdna4_argument_summary_output_words(32) == 1);
    CHECK(rdna4_argument_summary_output_words(33) == 2);
    CHECK(rdna4_argument_summary_output_words(UINT32_MAX) ==
          UINT32_C(134217728));

    size_t words = 0;
    CHECK(rdna4_argument_summary_job_bitmap_words(0, &words) && words == 0);
    CHECK(rdna4_argument_summary_job_bitmap_words(32, &words) && words == 1);
    CHECK(rdna4_argument_summary_job_bitmap_words(33, &words) && words == 2);
    CHECK(rdna4_argument_summary_job_bitmap_words(UINT64_MAX, &words));
    CHECK(words == (size_t)(UINT64_MAX / 32u + 1u));

    rdna4_argument_summary_push push = {0};
    CHECK(sizeof push == 32);
    CHECK(offsetof(rdna4_argument_summary_push, source_begin) == 0);
    CHECK(offsetof(rdna4_argument_summary_push, source_count) == 4);
    CHECK(offsetof(rdna4_argument_summary_push, source_end) == 8);
    CHECK(offsetof(rdna4_argument_summary_push, dictionary_begin) == 12);
    CHECK(offsetof(rdna4_argument_summary_push, dictionary_count) == 16);
    CHECK(offsetof(rdna4_argument_summary_push, dictionary_page) == 20);
    CHECK(offsetof(rdna4_argument_summary_push, dictionary_page_count) == 24);
    CHECK(offsetof(rdna4_argument_summary_push, output_word_base) == 28);
}

static void test_chunk_boundaries_and_overlap(void) {
    enum { source_size = 2053, chunk_bytes = 256 };
    unsigned char starts[source_size] = {0};
    rdna4_argument_summary_plan plan = make_plan(
        source_size, chunk_bytes, 1, 32, 77);
    CHECK(plan.chunk_count == 9);
    CHECK(plan.dictionary_page_count == 1);
    CHECK(plan.job_count == 9);

    for (uint64_t ordinal = 0; ordinal < plan.job_count; ++ordinal) {
        rdna4_argument_summary_job job;
        CHECK(rdna4_argument_summary_plan_job(&plan, ordinal, &job) ==
              RDNA4_ARGUMENT_SUMMARY_OK);
        CHECK(job.chunk_index == ordinal);
        CHECK(job.source_offset == ordinal * chunk_bytes);
        CHECK(job.push.source_begin == 0);
        uint64_t remaining = source_size - job.source_offset;
        uint32_t expected_count = remaining < chunk_bytes ?
            (uint32_t)remaining : chunk_bytes;
        uint64_t after = remaining - expected_count;
        uint32_t expected_end = expected_count +
            (after < RDNA4_ARGUMENT_SUMMARY_OVERLAP_BYTES ?
             (uint32_t)after : RDNA4_ARGUMENT_SUMMARY_OVERLAP_BYTES);
        CHECK(job.push.source_count == expected_count);
        CHECK(job.push.source_end == expected_end);
        CHECK(job.source_binding_bytes ==
              ((uint64_t)expected_end + 3u) / 4u * 4u);

        for (uint32_t local = 0; local < job.push.source_count; ++local) {
            if ((uint64_t)local + 8u <= job.push.source_end) {
                uint64_t global = job.source_offset + local;
                CHECK(global + 8u <= source_size);
                CHECK(starts[global] == 0);
                starts[global] = 1;
            }
        }
    }

    for (uint64_t start = 0; start + 8u <= source_size; ++start)
        CHECK(starts[start] == 1);
    for (uint64_t start = source_size - 7u; start < source_size; ++start)
        CHECK(starts[start] == 0);
    CHECK(starts[255] == 1);
    CHECK(starts[256] == 1);
    CHECK(starts[source_size - 8u] == 1);

    size_t word_count = 0;
    CHECK(rdna4_argument_summary_job_bitmap_words(plan.job_count, &word_count));
    CHECK(word_count == 1);
    uint32_t *completed = calloc(word_count, sizeof *completed);
    CHECK(completed != NULL);
    complete_all(&plan, completed, word_count, 1);
    rdna4_argument_summary_publication publication;
    CHECK(rdna4_argument_summary_plan_publish(
              &plan, 77, completed, word_count, &publication) ==
          RDNA4_ARGUMENT_SUMMARY_READY);
    CHECK(publication.completed_jobs == 9);
    CHECK(publication.output_word_count == 1);
    free(completed);
}

static void test_dictionary_pages_and_merge(void) {
    enum { source_size = 1024, chunk_bytes = 512 };
    enum { candidate_count = 1031, page_size = 128 };
    rdna4_argument_summary_plan plan = make_plan(
        source_size, chunk_bytes, candidate_count, page_size,
        UINT64_C(0x123456789abcdef0));
    CHECK(plan.chunk_count == 2);
    CHECK(plan.dictionary_page_count == 9);
    CHECK(plan.job_count == 18);

    unsigned seen[2][9] = {{0}};
    for (uint64_t ordinal = 0; ordinal < plan.job_count; ++ordinal) {
        rdna4_argument_summary_job job;
        CHECK(rdna4_argument_summary_plan_job(&plan, ordinal, &job) ==
              RDNA4_ARGUMENT_SUMMARY_OK);
        uint32_t page = (uint32_t)(ordinal % 9u);
        uint64_t chunk = ordinal / 9u;
        CHECK(job.chunk_index == chunk);
        CHECK(job.push.dictionary_page == page);
        CHECK(job.push.dictionary_page_count == 9);
        CHECK(job.push.dictionary_begin == page * page_size);
        uint32_t expected = candidate_count - page * page_size;
        if (expected > page_size) expected = page_size;
        CHECK(job.push.dictionary_count == expected);
        CHECK(++seen[chunk][page] == 1);
    }
    for (unsigned chunk = 0; chunk < 2; ++chunk)
        for (unsigned page = 0; page < 9; ++page)
            CHECK(seen[chunk][page] == 1);

    size_t words = 0;
    CHECK(rdna4_argument_summary_job_bitmap_words(plan.job_count, &words));
    uint32_t *completed = calloc(words, sizeof *completed);
    CHECK(completed != NULL);
    complete_all(&plan, completed, words, 1);

    uint64_t owners = UINT64_C(0xaaaaaaaaaaaaaaaa);
    rdna4_argument_summary_publication publication;
    CHECK(rdna4_argument_summary_plan_publish(
              &plan, UINT64_C(0x123456789abcdef0), completed, words,
              &publication) == RDNA4_ARGUMENT_SUMMARY_READY);
    if (publication.generation == plan.generation)
        owners = UINT64_C(0xbbbbbbbbbbbbbbbb);
    CHECK(owners == UINT64_C(0xbbbbbbbbbbbbbbbb));
    CHECK(publication.completed_jobs == 18);
    CHECK(publication.candidate_count == candidate_count);
    CHECK(publication.output_word_count == 33);
    free(completed);
}

static void test_partial_never_clears_owners(void) {
    rdna4_argument_summary_plan plan = make_plan(1024, 512, 1031, 128, 9);
    size_t words = 0;
    CHECK(rdna4_argument_summary_job_bitmap_words(plan.job_count, &words));
    uint32_t *completed = calloc(words, sizeof *completed);
    CHECK(completed != NULL);
    for (uint64_t ordinal = 0; ordinal + 1u < plan.job_count; ++ordinal) {
        rdna4_argument_summary_job job;
        CHECK(rdna4_argument_summary_plan_job(&plan, ordinal, &job) ==
              RDNA4_ARGUMENT_SUMMARY_OK);
        CHECK(rdna4_argument_summary_plan_complete(
                  &plan, &job, plan.generation, completed, words) ==
              RDNA4_ARGUMENT_SUMMARY_OK);
    }

    uint64_t owners = UINT64_C(0x1111111111111111);
    rdna4_argument_summary_publication publication;
    memset(&publication, 0xff, sizeof publication);
    CHECK(rdna4_argument_summary_plan_publish(
              &plan, plan.generation, completed, words, &publication) ==
          RDNA4_ARGUMENT_SUMMARY_INCOMPLETE);
    CHECK(publication.generation == 0);
    CHECK(publication.candidate_count == 0);
    CHECK(publication.output_word_count == 0);
    if (publication.generation == plan.generation)
        owners = 0;
    CHECK(owners == UINT64_C(0x1111111111111111));

    rdna4_argument_summary_job last;
    CHECK(rdna4_argument_summary_plan_job(
              &plan, plan.job_count - 1u, &last) ==
          RDNA4_ARGUMENT_SUMMARY_OK);
    CHECK(rdna4_argument_summary_plan_complete(
              &plan, &last, plan.generation, completed, words) ==
          RDNA4_ARGUMENT_SUMMARY_OK);
    CHECK(rdna4_argument_summary_plan_publish(
              &plan, plan.generation, completed, words, &publication) ==
          RDNA4_ARGUMENT_SUMMARY_READY);
    if (publication.generation == plan.generation)
        owners = UINT64_C(0x2222222222222222);
    CHECK(owners == UINT64_C(0x2222222222222222));
    free(completed);
}

static void test_stale_failed_and_obsolete(void) {
    uint64_t owners = UINT64_C(0x3333333333333333);
    rdna4_argument_summary_publication publication;
    memset(&publication, 0, sizeof publication);

    rdna4_argument_summary_plan stale_completion =
        make_plan(512, 256, 4, 32, 100);
    rdna4_argument_summary_job job;
    CHECK(rdna4_argument_summary_plan_job(
              &stale_completion, 0, &job) ==
          RDNA4_ARGUMENT_SUMMARY_OK);
    size_t words = 0;
    CHECK(rdna4_argument_summary_job_bitmap_words(
              stale_completion.job_count, &words));
    uint32_t *completed = calloc(words, sizeof *completed);
    CHECK(completed != NULL);
    CHECK(rdna4_argument_summary_plan_complete(
              &stale_completion, &job, 101, completed, words) ==
          RDNA4_ARGUMENT_SUMMARY_STALE);
    CHECK(completed[0] == 0);
    CHECK(stale_completion.state == RDNA4_ARGUMENT_SUMMARY_PLAN_OBSOLETE);
    CHECK(rdna4_argument_summary_plan_publish(
              &stale_completion, 101, completed, words, &publication) ==
          RDNA4_ARGUMENT_SUMMARY_OBSOLETE);
    if (publication.generation == stale_completion.generation)
        owners = 0;
    CHECK(owners == UINT64_C(0x3333333333333333));
    free(completed);

    rdna4_argument_summary_plan stale_publish =
        make_plan(512, 256, 4, 32, 200);
    words = 0;
    CHECK(rdna4_argument_summary_job_bitmap_words(
              stale_publish.job_count, &words));
    completed = calloc(words, sizeof *completed);
    CHECK(completed != NULL);
    complete_all(&stale_publish, completed, words, 0);
    CHECK(rdna4_argument_summary_plan_publish(
              &stale_publish, 201, completed, words, &publication) ==
          RDNA4_ARGUMENT_SUMMARY_STALE);
    if (publication.generation == stale_publish.generation)
        owners = 0;
    CHECK(owners == UINT64_C(0x3333333333333333));
    free(completed);

    rdna4_argument_summary_plan failed = make_plan(512, 256, 4, 32, 300);
    CHECK(rdna4_argument_summary_plan_fail(&failed, 300) ==
          RDNA4_ARGUMENT_SUMMARY_FAILED);
    CHECK(failed.state == RDNA4_ARGUMENT_SUMMARY_PLAN_FAILED);
    CHECK(rdna4_argument_summary_plan_publish(
              &failed, 300, NULL, 0, &publication) ==
          RDNA4_ARGUMENT_SUMMARY_FAILED);
    if (publication.generation == failed.generation)
        owners = 0;
    CHECK(owners == UINT64_C(0x3333333333333333));

    rdna4_argument_summary_plan obsolete =
        make_plan(512, 256, 4, 32, 400);
    CHECK(rdna4_argument_summary_plan_obsolete(&obsolete) ==
          RDNA4_ARGUMENT_SUMMARY_OBSOLETE);
    CHECK(rdna4_argument_summary_plan_publish(
              &obsolete, 400, NULL, 0, &publication) ==
          RDNA4_ARGUMENT_SUMMARY_OBSOLETE);
    if (publication.generation == obsolete.generation)
        owners = 0;
    CHECK(owners == UINT64_C(0x3333333333333333));
}

static void test_no_truncation_beyond_four_gib(void) {
    const uint64_t four_gib = UINT64_C(1) << 32;
    const uint32_t chunk_bytes = 1024u * 1024u;
    uint64_t source_size = four_gib + 13u;
    rdna4_argument_summary_plan plan = make_plan(
        source_size, chunk_bytes, 2, 32, 500);
    CHECK(plan.chunk_count == 4097);
    CHECK(plan.job_count == 4097);

    rdna4_argument_summary_job first;
    CHECK(rdna4_argument_summary_plan_job(&plan, 0, &first) ==
          RDNA4_ARGUMENT_SUMMARY_OK);
    CHECK(first.source_offset == 0);
    CHECK(first.push.source_count == chunk_bytes);
    CHECK(first.push.source_end == chunk_bytes +
          RDNA4_ARGUMENT_SUMMARY_OVERLAP_BYTES);
    CHECK(first.source_binding_bytes == chunk_bytes + 8u);

    rdna4_argument_summary_job last;
    CHECK(rdna4_argument_summary_plan_job(
              &plan, plan.job_count - 1u, &last) ==
          RDNA4_ARGUMENT_SUMMARY_OK);
    CHECK(last.chunk_index == 4096);
    CHECK(last.source_offset == four_gib);
    CHECK(last.push.source_count == 13);
    CHECK(last.push.source_end == 13);
    CHECK(last.source_binding_bytes == 16);
}

static void test_checked_limits_and_empty_plans(void) {
    rdna4_argument_summary_plan plan;
    CHECK(rdna4_argument_summary_plan_init(
              &plan, 1, 3, 1, 1, 1) ==
          RDNA4_ARGUMENT_SUMMARY_INVALID);
    CHECK(rdna4_argument_summary_plan_init(
              &plan, 1, RDNA4_ARGUMENT_SUMMARY_MAX_CHUNK_BYTES + 1u,
              1, 1, 1) == RDNA4_ARGUMENT_SUMMARY_INVALID);
    CHECK(rdna4_argument_summary_plan_init(
              &plan, 1, 256, 1, 0, 1) ==
          RDNA4_ARGUMENT_SUMMARY_INVALID);
    CHECK(rdna4_argument_summary_plan_init(
              &plan, 1, 256, 1, 16, 1) ==
          RDNA4_ARGUMENT_SUMMARY_INVALID);
    CHECK(rdna4_argument_summary_plan_init(
              &plan, UINT64_MAX, 4, UINT32_MAX, 32, 1) ==
          RDNA4_ARGUMENT_SUMMARY_OVERFLOW);

    plan = make_plan(0, 256, 0, 32, 600);
    CHECK(plan.chunk_count == 0);
    CHECK(plan.job_count == 0);
    rdna4_argument_summary_publication publication;
    CHECK(rdna4_argument_summary_plan_publish(
              &plan, 600, NULL, 0, &publication) ==
          RDNA4_ARGUMENT_SUMMARY_READY);
    CHECK(publication.completed_jobs == 0);
    CHECK(publication.output_word_count == 0);

    plan = make_plan(4096, 256, 0, 32, 601);
    CHECK(plan.chunk_count == 16);
    CHECK(plan.dictionary_page_count == 0);
    CHECK(plan.job_count == 0);
    CHECK(rdna4_argument_summary_plan_publish(
              &plan, 601, NULL, 0, &publication) ==
          RDNA4_ARGUMENT_SUMMARY_READY);
    CHECK(publication.completed_jobs == 0);

    plan = make_plan(16, 16, UINT32_MAX, 32, 602);
    uint32_t expected_pages = UINT32_MAX / 32u + 1u;
    CHECK(plan.dictionary_page_count == expected_pages);
    CHECK(plan.job_count == expected_pages);
    rdna4_argument_summary_job last_page;
    CHECK(rdna4_argument_summary_plan_job(
              &plan, plan.job_count - 1u, &last_page) ==
          RDNA4_ARGUMENT_SUMMARY_OK);
    CHECK(last_page.push.dictionary_page == expected_pages - 1u);
    CHECK(last_page.push.dictionary_page_count == expected_pages);
    CHECK(last_page.push.dictionary_begin ==
          (expected_pages - 1u) * 32u);
    CHECK(last_page.push.dictionary_count == 31);
}

int main(void) {
    test_candidate_abi();
    test_chunk_boundaries_and_overlap();
    test_dictionary_pages_and_merge();
    test_partial_never_clears_owners();
    test_stale_failed_and_obsolete();
    test_no_truncation_beyond_four_gib();
    test_checked_limits_and_empty_plans();
    printf("ARGUMENT_SUMMARY_PLAN_PASS checks=%u gpu_execution=0 partial_5_4=1\n",
           checks);
    return 0;
}
