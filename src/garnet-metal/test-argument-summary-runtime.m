/*
 * Host-only checks for rdna4_argument_summary.inc.
 *
 * This is a .m file for the production test location, but the exercised
 * runtime-planning half is deliberately plain C. It needs no Foundation,
 * Metal, Vulkan loader, GPU, queue, or VM. The simulated pipeline callbacks
 * cover creation order, failure at every stage, rollback order and retry.
 */

#define RDNA4_ARGUMENT_SUMMARY_RUNTIME_TEST 1
#include "garnet_arguments.m"

#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "ARGUMENT_SUMMARY_RUNTIME_FAIL %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
    ++checks; \
} while (0)

static unsigned checks;

static rdna4_argument_summary_runtime_limits test_limits(void) {
    return (rdna4_argument_summary_runtime_limits){
        .max_storage_buffer_range = 4096,
        .min_storage_buffer_offset_alignment = 256,
        .max_compute_workgroup_count_x = 65535,
        .max_compute_workgroup_invocations = 64,
        .max_compute_workgroup_size_x = 64,
        .requested_chunk_bytes = 1024u * 1024u,
        .requested_dictionary_page = 1024,
    };
}

static rdna4_argument_summary_runtime_plan make_plan(
    uint64_t source_size, uint32_t candidate_count, uint64_t generation) {
    rdna4_argument_summary_runtime_limits limits = test_limits();
    rdna4_argument_summary_runtime_plan plan;
    CHECK(rdna4_argument_summary_runtime_plan_init(
              &plan, &limits, source_size, candidate_count, generation) ==
          RDNA4_ARGUMENT_SUMMARY_OK);
    return plan;
}

static void test_plan_chunk_alignment_and_padding(void) {
    rdna4_argument_summary_runtime_limits limits = test_limits();
    rdna4_argument_summary_runtime_plan plan;
    CHECK(rdna4_argument_summary_runtime_plan_init(
              &plan, &limits, 10000, 5000, 42) ==
          RDNA4_ARGUMENT_SUMMARY_OK);
    CHECK(plan.generation == 42);
    CHECK(plan.chunk_bytes == 3840);
    CHECK(plan.chunk_bytes % 256u == 0);
    CHECK(plan.chunk_count == 3);
    CHECK(plan.dictionary_page_size == 512);
    CHECK(plan.dictionary_page_count == 10);
    CHECK(plan.job_count == 30);
    CHECK(plan.source_binding_bytes == 3848);
    CHECK(plan.dictionary_binding_bytes == 4096);
    CHECK(plan.output_page_bytes == 64);
    CHECK(plan.output_word_count == 157);
    CHECK(plan.source_binding_bytes <= limits.max_storage_buffer_range);
    CHECK(plan.dictionary_binding_bytes <= limits.max_storage_buffer_range);
    CHECK(plan.output_page_bytes <= limits.max_storage_buffer_range);

    rdna4_argument_summary_runtime_job first;
    CHECK(rdna4_argument_summary_runtime_job_get(
              &plan, 0, &first) == RDNA4_ARGUMENT_SUMMARY_OK);
    CHECK(first.page.page_index == 0);
    CHECK(first.chunk_index == 0);
    CHECK(first.source_offset == 0);
    CHECK(first.source_count == 3840);
    CHECK(first.source_overlap == 7);
    CHECK(first.source_end == 3847);
    CHECK(first.source_copy_bytes == 3847);
    CHECK(first.source_binding_bytes == 3848);
    CHECK(first.push.source_begin == 0);
    CHECK(first.push.source_count == 3840);
    CHECK(first.push.source_end == 3847);
    CHECK(first.push.dictionary_begin == 0);
    CHECK(first.push.dictionary_count == 512);
    CHECK(first.push.dictionary_page == 0);
    CHECK(first.push.dictionary_page_count == 10);
    CHECK(first.push.output_word_base == 0);

    rdna4_argument_summary_runtime_job last_chunk;
    CHECK(rdna4_argument_summary_runtime_job_get(
              &plan, 2, &last_chunk) == RDNA4_ARGUMENT_SUMMARY_OK);
    CHECK(last_chunk.source_offset == 7680);
    CHECK(last_chunk.source_count == 2320);
    CHECK(last_chunk.source_overlap == 0);
    CHECK(last_chunk.source_end == 2320);
    CHECK(last_chunk.source_binding_bytes == 2320);

    rdna4_argument_summary_runtime_job tail;
    CHECK(rdna4_argument_summary_runtime_job_get(
              &plan, 29, &tail) == RDNA4_ARGUMENT_SUMMARY_OK);
    CHECK(tail.page.page_index == 9);
    CHECK(tail.chunk_index == 2);
    CHECK(tail.page.dictionary_begin == 4608);
    CHECK(tail.page.dictionary_count == 392);
    CHECK(tail.page.dictionary_bytes == 3136);
    CHECK(tail.page.output_word_base == 144);
    CHECK(tail.page.output_word_count == 13);
    CHECK(tail.push.output_word_base == 144);

    rdna4_argument_summary_source_copy copy;
    CHECK(rdna4_argument_summary_runtime_source_copy(
              &tail, 123, 10000, &copy) ==
          RDNA4_ARGUMENT_SUMMARY_OK);
    CHECK(copy.source_offset == 7803);
    CHECK(copy.copy_bytes == 2320);
    CHECK(copy.source_offset + copy.copy_bytes == 10123);
    CHECK(copy.source_binding_bytes == 2320);
    CHECK(rdna4_argument_summary_runtime_source_copy(
              &tail, 123, 9999, &copy) ==
          RDNA4_ARGUMENT_SUMMARY_OVERFLOW);

    rdna4_argument_summary_scratch scratch =
        rdna4_argument_summary_runtime_scratch(&plan);
    CHECK(scratch.source_staging_bytes == plan.source_binding_bytes);
    CHECK(scratch.dictionary_staging_bytes ==
          plan.dictionary_binding_bytes);
    CHECK(scratch.output_staging_bytes == plan.output_page_bytes);
    CHECK(scratch.source_staging_bytes <= limits.max_storage_buffer_range);
    CHECK(scratch.dictionary_staging_bytes <=
          limits.max_storage_buffer_range);
    CHECK(scratch.output_staging_bytes <= limits.max_storage_buffer_range);
}

static void test_pages_cover_bits_without_crossing_words(void) {
    rdna4_argument_summary_runtime_plan plan =
        make_plan(64, 5000, 1);
    uint32_t next_word = 0;
    uint32_t seen_candidates = 0;
    for (uint32_t index = 0; index < plan.dictionary_page_count; ++index) {
        rdna4_argument_summary_runtime_page page;
        CHECK(rdna4_argument_summary_runtime_page_get(
                  &plan, index, &page) == RDNA4_ARGUMENT_SUMMARY_OK);
        CHECK(page.dictionary_begin % 32u == 0);
        CHECK(page.output_word_base == next_word);
        CHECK(page.output_word_base + page.output_word_count <=
              plan.output_word_count);
        CHECK(page.dictionary_bytes <=
              plan.dictionary_binding_bytes);
        CHECK(page.dictionary_bytes ==
              page.dictionary_count * sizeof(
                  rdna4_argument_summary_candidate));
        next_word += page.output_word_count;
        seen_candidates += page.dictionary_count;
    }
    CHECK(seen_candidates == 5000);
    CHECK(next_word == plan.output_word_count);
}

static void test_dispatch_partition_is_not_truncated(void) {
    const uint32_t max_groups = 65535;
    const uint32_t source_count =
        max_groups * RDNA4_ARGUMENT_SUMMARY_WORKGROUP_SIZE + 100u;
    rdna4_argument_summary_dispatch_cursor cursor;
    CHECK(rdna4_argument_summary_runtime_dispatch_init(
              &cursor, source_count, max_groups) ==
          RDNA4_ARGUMENT_SUMMARY_OK);
    rdna4_argument_summary_dispatch first;
    CHECK(rdna4_argument_summary_runtime_dispatch_next(&cursor, &first));
    CHECK(first.source_begin == 0);
    CHECK(first.source_count ==
          max_groups * RDNA4_ARGUMENT_SUMMARY_WORKGROUP_SIZE);
    CHECK(first.group_count == max_groups);
    rdna4_argument_summary_dispatch second;
    CHECK(rdna4_argument_summary_runtime_dispatch_next(&cursor, &second));
    CHECK(second.source_begin == first.source_count);
    CHECK(second.source_count == 100);
    CHECK(second.group_count == 2);
    CHECK(!rdna4_argument_summary_runtime_dispatch_next(&cursor, &second));
    CHECK((uint64_t)first.source_count + second.source_count ==
          source_count);

    CHECK(rdna4_argument_summary_runtime_dispatch_init(
              &cursor, 65, 1) == RDNA4_ARGUMENT_SUMMARY_OK);
    CHECK(rdna4_argument_summary_runtime_dispatch_next(&cursor, &first));
    CHECK(first.source_count == 64 && first.group_count == 1);
    CHECK(rdna4_argument_summary_runtime_dispatch_next(&cursor, &second));
    CHECK(second.source_begin == 64);
    CHECK(second.source_count == 1 && second.group_count == 1);
    CHECK(rdna4_argument_summary_runtime_dispatch_init(
              &cursor, 1, 0) == RDNA4_ARGUMENT_SUMMARY_INVALID);
}

static void test_large_source_is_chunked_not_truncated(void) {
    rdna4_argument_summary_runtime_limits limits = test_limits();
    const uint64_t four_gib = UINT64_C(1) << 32;
    uint64_t source_size = (four_gib / 3840u + 1u) * 3840u + 13u;
    rdna4_argument_summary_runtime_plan plan;
    CHECK(rdna4_argument_summary_runtime_plan_init(
              &plan, &limits, source_size, 1, 7) ==
          RDNA4_ARGUMENT_SUMMARY_OK);
    CHECK(plan.chunk_bytes == 3840);
    CHECK(plan.chunk_count ==
          1u + (source_size - 1u) / plan.chunk_bytes);
    rdna4_argument_summary_runtime_job last;
    CHECK(rdna4_argument_summary_runtime_job_get(
              &plan, plan.chunk_count - 1u, &last) ==
          RDNA4_ARGUMENT_SUMMARY_OK);
    CHECK(last.source_offset + last.source_count == source_size);
    CHECK(last.source_count == 13);
    CHECK(last.source_overlap == 0);
    CHECK(last.source_end == 13);
    CHECK(last.source_binding_bytes == 16);
    CHECK(last.source_binding_bytes <= limits.max_storage_buffer_range);
}

static void test_empty_and_invalid_plans(void) {
    rdna4_argument_summary_runtime_limits limits = test_limits();
    rdna4_argument_summary_runtime_plan plan;
    CHECK(rdna4_argument_summary_runtime_plan_init(
              &plan, &limits, 0, 0, 9) ==
          RDNA4_ARGUMENT_SUMMARY_OK);
    CHECK(plan.chunk_count == 0);
    CHECK(plan.dictionary_page_count == 0);
    CHECK(plan.job_count == 0);
    rdna4_argument_summary_scratch scratch =
        rdna4_argument_summary_runtime_scratch(&plan);
    CHECK(scratch.source_staging_bytes == 0);
    CHECK(scratch.dictionary_staging_bytes == 0);
    CHECK(scratch.output_staging_bytes == 0);

    CHECK(rdna4_argument_summary_runtime_plan_init(
              &plan, &limits, 0, 5000, 10) ==
          RDNA4_ARGUMENT_SUMMARY_OK);
    CHECK(plan.chunk_count == 0);
    CHECK(plan.dictionary_page_count == 10);
    CHECK(plan.job_count == 0);
    scratch = rdna4_argument_summary_runtime_scratch(&plan);
    CHECK(scratch.source_staging_bytes == 0);
    CHECK(scratch.dictionary_staging_bytes == 0);
    CHECK(scratch.output_staging_bytes == plan.output_page_bytes);

    rdna4_argument_summary_runtime_limits invalid = limits;
    invalid.max_storage_buffer_range = 0;
    CHECK(rdna4_argument_summary_runtime_plan_init(
              &plan, &invalid, 1, 1, 1) ==
          RDNA4_ARGUMENT_SUMMARY_INVALID);
    invalid = limits;
    invalid.min_storage_buffer_offset_alignment = 3;
    CHECK(rdna4_argument_summary_runtime_plan_init(
              &plan, &invalid, 1, 1, 1) ==
          RDNA4_ARGUMENT_SUMMARY_INVALID);
    invalid = limits;
    invalid.max_compute_workgroup_count_x = 0;
    CHECK(rdna4_argument_summary_runtime_plan_init(
              &plan, &invalid, 1, 1, 1) ==
          RDNA4_ARGUMENT_SUMMARY_INVALID);
    invalid = limits;
    invalid.max_compute_workgroup_invocations = 63;
    CHECK(rdna4_argument_summary_runtime_plan_init(
              &plan, &invalid, 1, 1, 1) ==
          RDNA4_ARGUMENT_SUMMARY_INVALID);
    invalid = limits;
    invalid.max_compute_workgroup_size_x = 63;
    CHECK(rdna4_argument_summary_runtime_plan_init(
              &plan, &invalid, 1, 1, 1) ==
          RDNA4_ARGUMENT_SUMMARY_INVALID);
    invalid = limits;
    invalid.requested_dictionary_page = 16;
    CHECK(rdna4_argument_summary_runtime_plan_init(
              &plan, &invalid, 1, 1, 1) ==
          RDNA4_ARGUMENT_SUMMARY_INVALID);
    invalid = limits;
    invalid.max_storage_buffer_range = 255;
    CHECK(rdna4_argument_summary_runtime_plan_init(
              &plan, &invalid, 1, 1, 1) ==
          RDNA4_ARGUMENT_SUMMARY_INVALID);
}

typedef struct {
    unsigned fail_stage;
    unsigned create_count[4];
    unsigned destroy_count[4];
    unsigned destroy_order[4];
    unsigned destroy_total;
    uintptr_t next_handle;
} pipeline_mock;

static int32_t mock_create(
    pipeline_mock *mock, unsigned stage, uintptr_t *handle) {
    ++mock->create_count[stage];
    if (mock->fail_stage == stage) {
        *handle = 0;
        return -1;
    }
    *handle = ++mock->next_handle;
    return 0;
}

static int32_t mock_create_shader(void *context, uintptr_t *handle) {
    return mock_create(context, 0, handle);
}

static int32_t mock_create_descriptor_layout(
    void *context, uintptr_t *handle) {
    return mock_create(context, 1, handle);
}

static int32_t mock_create_pipeline_layout(
    void *context, uintptr_t shader, uintptr_t descriptor_layout,
    uintptr_t *handle) {
    pipeline_mock *mock = context;
    CHECK(shader != 0 && descriptor_layout != 0);
    return mock_create(mock, 2, handle);
}

static int32_t mock_create_pipeline(
    void *context, uintptr_t shader, uintptr_t pipeline_layout,
    uintptr_t *handle) {
    pipeline_mock *mock = context;
    CHECK(shader != 0 && pipeline_layout != 0);
    return mock_create(mock, 3, handle);
}

static void mock_destroy(
    pipeline_mock *mock, unsigned stage, uintptr_t handle) {
    CHECK(handle != 0);
    ++mock->destroy_count[stage];
    CHECK(mock->destroy_total < 4);
    mock->destroy_order[mock->destroy_total++] = stage;
}

static void mock_destroy_shader(void *context, uintptr_t handle) {
    mock_destroy(context, 0, handle);
}

static void mock_destroy_descriptor_layout(
    void *context, uintptr_t handle) {
    mock_destroy(context, 1, handle);
}

static void mock_destroy_pipeline_layout(
    void *context, uintptr_t handle) {
    mock_destroy(context, 2, handle);
}

static void mock_destroy_pipeline(void *context, uintptr_t handle) {
    mock_destroy(context, 3, handle);
}

static rdna4_argument_summary_pipeline_ops mock_ops(void) {
    return (rdna4_argument_summary_pipeline_ops){
        .create_shader = mock_create_shader,
        .create_descriptor_layout = mock_create_descriptor_layout,
        .create_pipeline_layout = mock_create_pipeline_layout,
        .create_pipeline = mock_create_pipeline,
        .destroy_shader = mock_destroy_shader,
        .destroy_descriptor_layout = mock_destroy_descriptor_layout,
        .destroy_pipeline_layout = mock_destroy_pipeline_layout,
        .destroy_pipeline = mock_destroy_pipeline,
    };
}

static void reset_mock(pipeline_mock *mock) {
    memset(mock, 0, sizeof(*mock));
    mock->fail_stage = UINT32_MAX;
}

static void test_lazy_pipeline_creation_and_rollback(void) {
    pipeline_mock mock;
    reset_mock(&mock);
    rdna4_argument_summary_pipeline_ops ops = mock_ops();
    ops.context = &mock;
    rdna4_argument_summary_pipeline_state state = {0};

    for (unsigned fail = 0; fail < 4; ++fail) {
        reset_mock(&mock);
        mock.fail_stage = fail;
        memset(&state, 0, sizeof state);
        CHECK(rdna4_argument_summary_pipeline_ensure(
                  &state, &ops, 0x1234) ==
              RDNA4_ARGUMENT_SUMMARY_FAILED);
        CHECK(!state.ready);
        CHECK(state.shader == 0 && state.descriptor_layout == 0);
        CHECK(state.pipeline_layout == 0 && state.pipeline == 0);
        CHECK(mock.destroy_total == fail);
        for (unsigned index = 0; index < fail; ++index)
            CHECK(mock.destroy_order[index] == fail - 1u - index);
    }

    reset_mock(&mock);
    CHECK(rdna4_argument_summary_pipeline_ensure(
              &state, &ops, 0x1234) ==
          RDNA4_ARGUMENT_SUMMARY_OK);
    CHECK(state.ready && state.device_key == 0x1234);
    CHECK(state.shader == 1 && state.descriptor_layout == 2);
    CHECK(state.pipeline_layout == 3 && state.pipeline == 4);
    CHECK(mock.create_count[0] == 1 && mock.create_count[1] == 1);
    CHECK(mock.create_count[2] == 1 && mock.create_count[3] == 1);
    CHECK(rdna4_argument_summary_pipeline_ensure(
              &state, &ops, 0x1234) ==
          RDNA4_ARGUMENT_SUMMARY_OK);
    CHECK(mock.create_count[0] == 1 && mock.create_count[3] == 1);
    CHECK(rdna4_argument_summary_pipeline_ensure(
              &state, &ops, 0x5678) ==
          RDNA4_ARGUMENT_SUMMARY_FAILED);
    CHECK(mock.destroy_total == 0);

    rdna4_argument_summary_pipeline_release(&state, &ops);
    CHECK(!state.ready);
    CHECK(mock.destroy_total == 4);
    CHECK(mock.destroy_order[0] == 3);
    CHECK(mock.destroy_order[1] == 2);
    CHECK(mock.destroy_order[2] == 1);
    CHECK(mock.destroy_order[3] == 0);

    rdna4_argument_summary_pipeline_ops missing = ops;
    missing.create_pipeline = NULL;
    CHECK(rdna4_argument_summary_pipeline_ensure(
              &state, &missing, 0x1234) ==
          RDNA4_ARGUMENT_SUMMARY_INVALID);
}

static void complete_jobs(
    rdna4_argument_summary_completion *completion,
    const rdna4_argument_summary_runtime_plan *plan) {
    for (uint64_t index = 0; index < plan->job_count; ++index)
        CHECK(rdna4_argument_summary_completion_job_encoded(completion) ==
              RDNA4_ARGUMENT_SUMMARY_OK);
}

static void test_completion_and_publication_gates(void) {
    rdna4_argument_summary_runtime_plan plan =
        make_plan(10000, 5000, 55);
    rdna4_argument_summary_completion completion;
    rdna4_argument_summary_publication publication;
    rdna4_argument_summary_completion_init(&completion, &plan);

    CHECK(rdna4_argument_summary_completion_fence(
              &completion, true, 55) ==
          RDNA4_ARGUMENT_SUMMARY_INCOMPLETE);
    CHECK(rdna4_argument_summary_completion_finish_encode(
              &completion, false) ==
          RDNA4_ARGUMENT_SUMMARY_FAILED);
    CHECK(rdna4_argument_summary_completion_publish(
              &completion, &plan, 55, &publication) ==
          RDNA4_ARGUMENT_SUMMARY_FAILED);
    CHECK(publication.generation == 0);

    rdna4_argument_summary_completion_init(&completion, &plan);
    CHECK(rdna4_argument_summary_completion_finish_encode(
              &completion, true) ==
          RDNA4_ARGUMENT_SUMMARY_INCOMPLETE);
    CHECK(rdna4_argument_summary_completion_publish(
              &completion, &plan, 55, &publication) ==
          RDNA4_ARGUMENT_SUMMARY_FAILED);

    rdna4_argument_summary_completion_init(&completion, &plan);
    complete_jobs(&completion, &plan);
    CHECK(rdna4_argument_summary_completion_finish_encode(
              &completion, true) ==
          RDNA4_ARGUMENT_SUMMARY_OK);
    CHECK(rdna4_argument_summary_completion_publish(
              &completion, &plan, 55, &publication) ==
          RDNA4_ARGUMENT_SUMMARY_INCOMPLETE);
    CHECK(publication.generation == 0);
    CHECK(rdna4_argument_summary_completion_fence(
              &completion, false, 55) ==
          RDNA4_ARGUMENT_SUMMARY_FAILED);
    CHECK(rdna4_argument_summary_completion_publish(
              &completion, &plan, 55, &publication) ==
          RDNA4_ARGUMENT_SUMMARY_FAILED);

    rdna4_argument_summary_completion_init(&completion, &plan);
    complete_jobs(&completion, &plan);
    CHECK(rdna4_argument_summary_completion_finish_encode(
              &completion, true) ==
          RDNA4_ARGUMENT_SUMMARY_OK);
    CHECK(rdna4_argument_summary_completion_fence(
              &completion, true, 56) ==
          RDNA4_ARGUMENT_SUMMARY_STALE);
    CHECK(rdna4_argument_summary_completion_publish(
              &completion, &plan, 56, &publication) ==
          RDNA4_ARGUMENT_SUMMARY_OBSOLETE);
    CHECK(publication.generation == 0);

    rdna4_argument_summary_completion_init(&completion, &plan);
    complete_jobs(&completion, &plan);
    CHECK(rdna4_argument_summary_completion_finish_encode(
              &completion, true) ==
          RDNA4_ARGUMENT_SUMMARY_OK);
    CHECK(rdna4_argument_summary_completion_fence(
              &completion, true, 55) ==
          RDNA4_ARGUMENT_SUMMARY_COMPLETE);
    CHECK(rdna4_argument_summary_completion_publish(
              &completion, &plan, 55, &publication) ==
          RDNA4_ARGUMENT_SUMMARY_READY);
    CHECK(publication.generation == 55);
    CHECK(publication.source_size == 10000);
    CHECK(publication.completed_jobs == 30);
    CHECK(publication.candidate_count == 5000);
    CHECK(publication.output_word_count == 157);
    CHECK(publication.output_bytes == 157u * sizeof(uint32_t));

    rdna4_argument_summary_runtime_plan empty =
        make_plan(0, 0, 77);
    rdna4_argument_summary_completion_init(&completion, &empty);
    CHECK(rdna4_argument_summary_completion_finish_encode(
              &completion, true) ==
          RDNA4_ARGUMENT_SUMMARY_OK);
    CHECK(rdna4_argument_summary_completion_fence(
              &completion, true, 77) ==
          RDNA4_ARGUMENT_SUMMARY_COMPLETE);
    CHECK(rdna4_argument_summary_completion_publish(
              &completion, &empty, 77, &publication) ==
          RDNA4_ARGUMENT_SUMMARY_READY);
    CHECK(publication.completed_jobs == 0);
    CHECK(publication.output_word_count == 0);
}

typedef struct {
    const rdna4_argument_summary_runtime_plan *plan;
    const unsigned char *source;
    uint32_t source_size;
    const rdna4_argument_summary_candidate *dictionary;
    uint32_t dictionary_count;
    unsigned char *source_scratch;
    unsigned char *dictionary_scratch;
    uint32_t *output_scratch;
    uint32_t *output;
    uint32_t output_offset_words;
    uint32_t output_capacity_words;
    uint32_t max_groups;
    unsigned bind_count;
    unsigned dictionary_copies;
    unsigned source_copies;
    unsigned output_fills;
    unsigned output_copies;
    unsigned dispatch_count;
    unsigned fill_barriers;
    unsigned source_barriers;
    unsigned compute_barriers;
    unsigned publication_barriers;
    uint32_t max_seen_groups;
} summary_simulator;

static uint32_t load_le32(const unsigned char *bytes) {
    return (uint32_t)bytes[0] |
        (uint32_t)bytes[1] << 8u |
        (uint32_t)bytes[2] << 16u |
        (uint32_t)bytes[3] << 24u;
}

static void store_le32(unsigned char *bytes, uint32_t value) {
    bytes[0] = (unsigned char)value;
    bytes[1] = (unsigned char)(value >> 8u);
    bytes[2] = (unsigned char)(value >> 16u);
    bytes[3] = (unsigned char)(value >> 24u);
}

static void store_token(unsigned char *source,
                        rdna4_argument_summary_candidate token,
                        uint32_t offset) {
    store_le32(source + offset, token.lo);
    store_le32(source + offset + 4u, token.hi);
}

static void sim_bind(void *opaque) {
    summary_simulator *sim = opaque;
    ++sim->bind_count;
}

static void sim_copy_dictionary(
    void *opaque, const rdna4_argument_summary_runtime_page *page) {
    summary_simulator *sim = opaque;
    CHECK(page->dictionary_offset + page->dictionary_bytes <=
          (uint64_t)sim->dictionary_count *
              sizeof(rdna4_argument_summary_candidate));
    CHECK(page->dictionary_bytes <= sim->plan->dictionary_binding_bytes);
    memcpy(sim->dictionary_scratch,
           (const unsigned char *)sim->dictionary +
               page->dictionary_offset,
           page->dictionary_bytes);
    ++sim->dictionary_copies;
}

static void sim_fill_output(
    void *opaque, const rdna4_argument_summary_runtime_page *page,
    uint32_t bytes) {
    (void)page;
    summary_simulator *sim = opaque;
    CHECK(bytes <= sim->plan->output_page_bytes);
    memset(sim->output_scratch, 0, bytes);
    ++sim->output_fills;
}

static void sim_copy_source(
    void *opaque, const rdna4_argument_summary_runtime_job *job,
    const rdna4_argument_summary_source_copy *copy) {
    summary_simulator *sim = opaque;
    CHECK(job != NULL);
    CHECK(copy->source_offset + copy->copy_bytes <= sim->source_size);
    CHECK(copy->copy_bytes <= sim->plan->source_binding_bytes);
    CHECK(copy->source_binding_bytes <= sim->plan->source_binding_bytes);
    memcpy(sim->source_scratch, sim->source + copy->source_offset,
           copy->copy_bytes);
    ++sim->source_copies;
}

static int32_t sim_find_candidate(
    const summary_simulator *sim, const rdna4_argument_summary_push *push,
    rdna4_argument_summary_candidate token) {
    int32_t low = 0;
    int32_t high = (int32_t)push->dictionary_count;
    while (low < high) {
        int32_t middle = low + (high - low) / 2;
        rdna4_argument_summary_candidate candidate;
        memcpy(&candidate,
               sim->dictionary_scratch +
                   (size_t)middle *
                       sizeof(rdna4_argument_summary_candidate),
               sizeof candidate);
        if (rdna4_argument_summary_candidate_compare(candidate, token) < 0)
            low = middle + 1;
        else
            high = middle;
    }
    if (low < (int32_t)push->dictionary_count) {
        rdna4_argument_summary_candidate candidate;
        memcpy(&candidate,
               sim->dictionary_scratch +
                   (size_t)low *
                       sizeof(rdna4_argument_summary_candidate),
               sizeof candidate);
        if (rdna4_argument_summary_candidate_compare(candidate, token) == 0)
            return (int32_t)push->dictionary_begin + low;
    }
    return -1;
}

static void sim_dispatch(
    void *opaque, const rdna4_argument_summary_runtime_job *job,
    const rdna4_argument_summary_dispatch *dispatch,
    const rdna4_argument_summary_push *push) {
    summary_simulator *sim = opaque;
    CHECK(dispatch->group_count > 0);
    CHECK(dispatch->group_count <= sim->max_groups);
    CHECK(dispatch->source_begin + dispatch->source_count <=
          job->source_count);
    CHECK(push->source_begin == dispatch->source_begin);
    CHECK(push->source_count == dispatch->source_count);
    CHECK(push->source_end == job->source_end);
    CHECK(push->dictionary_begin == job->page.dictionary_begin);
    CHECK(push->dictionary_count == job->page.dictionary_count);
    CHECK(push->dictionary_page == job->page.page_index);
    CHECK(push->output_word_base == job->page.output_word_base);
    if (dispatch->group_count > sim->max_seen_groups)
        sim->max_seen_groups = dispatch->group_count;

    for (uint32_t index = 0; index < push->source_count; ++index) {
        uint32_t position = push->source_begin + index;
        if (position > push->source_end - 8u)
            continue;
        rdna4_argument_summary_candidate token = {
            .lo = load_le32(sim->source_scratch + position),
            .hi = load_le32(sim->source_scratch + position + 4u),
        };
        if (!rdna4_argument_summary_candidate_valid(token))
            continue;
        int32_t found = sim_find_candidate(sim, push, token);
        if (found < 0)
            continue;
        uint32_t global_word = (uint32_t)found >> 5u;
        CHECK(global_word >= push->output_word_base);
        uint32_t local_word = global_word - push->output_word_base;
        CHECK(local_word < sim->plan->dictionary_page_size / 32u);
        sim->output_scratch[local_word] |=
            UINT32_C(1) << ((uint32_t)found & 31u);
    }
    ++sim->dispatch_count;
}

static void sim_barrier(
    void *opaque, uint32_t source_stage, uint32_t source_access,
    uint32_t destination_stage, uint32_t destination_access) {
    summary_simulator *sim = opaque;
    if (source_stage == RDNA4_ARGUMENT_SUMMARY_STAGE_TRANSFER &&
        source_access == RDNA4_ARGUMENT_SUMMARY_ACCESS_TRANSFER_WRITE &&
        destination_stage ==
            (RDNA4_ARGUMENT_SUMMARY_STAGE_TRANSFER |
             RDNA4_ARGUMENT_SUMMARY_STAGE_COMPUTE)) {
        ++sim->fill_barriers;
    } else if (source_stage == RDNA4_ARGUMENT_SUMMARY_STAGE_TRANSFER &&
               destination_stage ==
                   RDNA4_ARGUMENT_SUMMARY_STAGE_COMPUTE) {
        ++sim->source_barriers;
    } else if (source_stage == RDNA4_ARGUMENT_SUMMARY_STAGE_COMPUTE &&
               destination_stage ==
                   RDNA4_ARGUMENT_SUMMARY_STAGE_TRANSFER) {
        ++sim->compute_barriers;
    } else {
        CHECK(source_stage == RDNA4_ARGUMENT_SUMMARY_STAGE_TRANSFER);
        CHECK(destination_stage &
              RDNA4_ARGUMENT_SUMMARY_STAGE_HOST);
        CHECK(destination_access &
              RDNA4_ARGUMENT_SUMMARY_ACCESS_HOST_READ);
        ++sim->publication_barriers;
    }
}

static void sim_copy_output(
    void *opaque, const rdna4_argument_summary_runtime_page *page,
    uint32_t bytes) {
    summary_simulator *sim = opaque;
    uint32_t byte_offset = page->output_word_base * sizeof(uint32_t);
    CHECK(byte_offset + bytes <=
          sim->plan->output_word_count * sizeof(uint32_t));
    CHECK(sim->output_offset_words * sizeof(uint32_t) + byte_offset +
              bytes <=
          sim->output_capacity_words * sizeof(uint32_t));
    memcpy((unsigned char *)sim->output +
               sim->output_offset_words * sizeof(uint32_t) +
               byte_offset,
           sim->output_scratch, bytes);
    ++sim->output_copies;
}

static void test_strict_runtime_operation_simulation(void) {
    enum {
        source_size = 10000,
        candidate_count = 2048,
        output_offset_words = 8,
        output_capacity_words = 96,
    };
    rdna4_argument_summary_runtime_limits limits = test_limits();
    limits.max_compute_workgroup_count_x = 3;
    rdna4_argument_summary_runtime_plan plan;
    CHECK(rdna4_argument_summary_runtime_plan_init(
              &plan, &limits, source_size, candidate_count, 1234) ==
          RDNA4_ARGUMENT_SUMMARY_OK);
    CHECK(plan.chunk_count == 3);
    CHECK(plan.dictionary_page_count == 4);
    CHECK(plan.job_count == 12);

    unsigned char *source = calloc(source_size, 1);
    rdna4_argument_summary_candidate *dictionary =
        calloc(candidate_count, sizeof(*dictionary));
    CHECK(source && dictionary);
    for (uint32_t index = 0; index < candidate_count; ++index) {
        dictionary[index].lo =
            UINT32_C(0x10000000) ^ (index * UINT32_C(2654435761));
        dictionary[index].hi =
            UINT32_C(0x52440000) | index;
    }
    const struct {
        uint32_t position;
        uint32_t index;
    } hits[] = {
        { 0, 0 },
        { 8, 511 },
        { 3837, 512 },
        { 3848, 513 },
        { 4095, 1024 },
        { 5001, 1500 },
        { 7000, 42 },
        { 9992, 2047 },
    };
    for (size_t index = 0; index < sizeof hits / sizeof hits[0]; ++index)
        store_token(source, dictionary[hits[index].index],
                    hits[index].position);
    rdna4_argument_summary_candidate absent = {
        .lo = UINT32_C(0xdeadbeef),
        .hi = UINT32_C(0x5244ffff),
    };
    store_token(source, absent, 9000);

    uint32_t *output = malloc(
        output_capacity_words * sizeof(*output));
    uint32_t *expected_output = calloc(
        plan.output_word_count, sizeof(*expected_output));
    CHECK(output && expected_output);
    for (uint32_t index = 0; index < output_capacity_words; ++index)
        output[index] = UINT32_C(0xa5a5a5a5);
    for (uint32_t position = 0;
         position + 8u <= (uint32_t)source_size; ++position) {
        rdna4_argument_summary_candidate token = {
            .lo = load_le32(source + position),
            .hi = load_le32(source + position + 4u),
        };
        if (!rdna4_argument_summary_candidate_valid(token))
            continue;
        uint32_t low = 0, high = candidate_count;
        while (low < high) {
            uint32_t middle = low + (high - low) / 2u;
            if (rdna4_argument_summary_candidate_compare(
                    dictionary[middle], token) < 0)
                low = middle + 1u;
            else
                high = middle;
        }
        if (low < candidate_count &&
            rdna4_argument_summary_candidate_compare(
                dictionary[low], token) == 0) {
            expected_output[low / 32u] |=
                UINT32_C(1) << (low & 31u);
        }
    }
    for (size_t hit = 0; hit < sizeof hits / sizeof hits[0]; ++hit)
        CHECK((expected_output[hits[hit].index / 32u] &
               (UINT32_C(1) << (hits[hit].index & 31u))) != 0);

    summary_simulator sim = {
        .plan = &plan,
        .source = source,
        .source_size = source_size,
        .dictionary = dictionary,
        .dictionary_count = candidate_count,
        .source_scratch = calloc(plan.source_binding_bytes, 1),
        .dictionary_scratch = calloc(plan.dictionary_binding_bytes, 1),
        .output_scratch =
            calloc(plan.output_page_bytes / sizeof(uint32_t),
                   sizeof(uint32_t)),
        .output = output,
        .output_offset_words = output_offset_words,
        .output_capacity_words = output_capacity_words,
        .max_groups = limits.max_compute_workgroup_count_x,
    };
    CHECK(sim.source_scratch && sim.dictionary_scratch &&
          sim.output_scratch);
    rdna4_argument_summary_sink sink = {
        .context = &sim,
        .bind = sim_bind,
        .copy_dictionary = sim_copy_dictionary,
        .fill_output = sim_fill_output,
        .copy_source = sim_copy_source,
        .dispatch = sim_dispatch,
        .barrier = sim_barrier,
        .copy_output = sim_copy_output,
    };
    rdna4_argument_summary_completion completion;
    rdna4_argument_summary_completion_init(&completion, &plan);
    CHECK(rdna4_argument_summary_runtime_record(
              &plan, &completion, &sink,
              limits.max_compute_workgroup_count_x) ==
          RDNA4_ARGUMENT_SUMMARY_OK);
    CHECK(completion.encoded_jobs == plan.job_count);
    CHECK(sim.bind_count == 1);
    CHECK(sim.dictionary_copies == plan.dictionary_page_count);
    CHECK(sim.output_fills == plan.dictionary_page_count);
    CHECK(sim.output_copies == plan.dictionary_page_count);
    CHECK(sim.source_copies == plan.job_count);
    CHECK(sim.dispatch_count > plan.job_count);
    CHECK(sim.max_seen_groups == limits.max_compute_workgroup_count_x);
    CHECK(sim.fill_barriers == plan.dictionary_page_count);
    CHECK(sim.source_barriers == plan.job_count);
    CHECK(sim.compute_barriers == plan.job_count);
    CHECK(sim.publication_barriers == plan.dictionary_page_count);

    for (uint32_t index = 0; index < output_offset_words; ++index)
        CHECK(output[index] == UINT32_C(0xa5a5a5a5));
    for (uint32_t index = 0; index < plan.output_word_count; ++index)
        CHECK(output[output_offset_words + index] ==
              expected_output[index]);
    for (uint32_t index =
             output_offset_words + plan.output_word_count;
         index < output_capacity_words; ++index)
        CHECK(output[index] == UINT32_C(0xa5a5a5a5));

    CHECK(rdna4_argument_summary_completion_finish_encode(
              &completion, true) ==
          RDNA4_ARGUMENT_SUMMARY_OK);
    CHECK(rdna4_argument_summary_completion_fence(
              &completion, true, 1234) ==
          RDNA4_ARGUMENT_SUMMARY_COMPLETE);
    rdna4_argument_summary_publication publication;
    CHECK(rdna4_argument_summary_completion_publish(
              &completion, &plan, 1234, &publication) ==
          RDNA4_ARGUMENT_SUMMARY_READY);
    CHECK(publication.completed_jobs == plan.job_count);
    CHECK(publication.output_word_count == plan.output_word_count);
    CHECK(publication.output_bytes ==
          plan.output_word_count * sizeof(uint32_t));

    free(sim.output_scratch);
    free(sim.dictionary_scratch);
    free(sim.source_scratch);
    free(output);
    free(expected_output);
    free(dictionary);
    free(source);
}

int main(void) {
    test_plan_chunk_alignment_and_padding();
    test_pages_cover_bits_without_crossing_words();
    test_dispatch_partition_is_not_truncated();
    test_large_source_is_chunked_not_truncated();
    test_empty_and_invalid_plans();
    test_lazy_pipeline_creation_and_rollback();
    test_completion_and_publication_gates();
    test_strict_runtime_operation_simulation();
    printf("ARGUMENT_SUMMARY_RUNTIME_PASS checks=%u simulated_vulkan=1 "
           "gpu_execution=0 partial_5_4=1\n", checks);
    return 0;
}
