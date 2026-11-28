// garnet_argument_summary.m - GarnetMetal modular compilation unit: GPU argument summary runtime.
#if !defined(GARNET_INTERNAL) && !defined(GARNET_METAL_MODULAR_BUILD)
#define GARNET_INTERNAL static
#endif
#if !defined(RDNA4_ARGUMENT_SUMMARY_RUNTIME_TEST)
#ifdef GARNET_METAL_MODULAR_BUILD
#include "garnet_metal_internal.h"
#endif
#include "garnet_arguments_internal.h"
#endif

/*
 * Runtime foundation for the Private live-token summary shader.
 *
 * Include this file from GarnetDevice.m after RDNA4Buffer is declared. The
 * planning/completion half is pure C so test-argument-summary-runtime.m can
 * exercise it without Vulkan, Metal, Foundation, or a GPU. The bridge half
 * records into an existing VkCommandBuffer and deliberately creates no queue,
 * submit, wait, or owner-retirement path.
 *
 * GarnetDevice.m integration point, inside rdna4EndCurrentSegment:
 *
 *   rdna4_full_barrier(_curSegment->vk);
 *   RDNA4ArgumentSummaryAttempt *attempt = nil;
 *   if (!rdna4_argument_summary_encode(task, _curSegment->vk, &attempt, err))
 *       return NO;                       // abort/fail this segment before end
 *   [segment retain the attempt through its fence completion];
 *   vkEndCommandBuffer(_curSegment->vk);
 *
 * After that existing fence completes:
 *
 *   status = rdna4_argument_summary_attempt_complete(attempt, fence_ok,
 *                                                     live_generation);
 *   if (status == RDNA4_ARGUMENT_SUMMARY_COMPLETE)
 *       status = rdna4_argument_summary_attempt_publish(
 *           attempt, live_generation, &publication);
 *
 * Only READY authorizes the lifetime owner to replace owners. The attempt
 * holds the task and every scratch resource; releasing it before the segment
 * fence is invalid. Device teardown must call
 * rdna4_argument_summary_pipeline_destroy() before destroying RDNA4R.dev.
 */

#ifndef RDNA4_ARGUMENT_SUMMARY_RUNTIME_INC
#define RDNA4_ARGUMENT_SUMMARY_RUNTIME_INC

#include "garnet_argument_summary.h"

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define RDNA4_ARGUMENT_SUMMARY_DICTIONARY_ALIGN 32u
#define RDNA4_ARGUMENT_SUMMARY_MIN_COPY_ALIGN 4u

typedef struct {
    uint32_t max_storage_buffer_range;
    uint32_t min_storage_buffer_offset_alignment;
    uint32_t max_compute_workgroup_count_x;
    uint32_t max_compute_workgroup_invocations;
    uint32_t max_compute_workgroup_size_x;
    /* Zero selects the corresponding RDNA4_ARGUMENT_SUMMARY_DEFAULT_* value. */
    uint32_t requested_chunk_bytes;
    uint32_t requested_dictionary_page;
} rdna4_argument_summary_runtime_limits;

typedef struct {
    uint64_t generation;
    uint64_t source_size;
    uint64_t chunk_count;
    uint64_t job_count;
    uint32_t chunk_bytes;
    uint32_t candidate_count;
    uint32_t dictionary_page_size;
    uint32_t dictionary_page_count;
    uint32_t output_word_count;
    uint32_t source_binding_bytes;
    uint32_t dictionary_binding_bytes;
    uint32_t output_page_bytes;
} rdna4_argument_summary_runtime_plan;

typedef struct {
    uint64_t source_offset;
    uint64_t dictionary_offset;
    uint32_t page_index;
    uint32_t dictionary_begin;
    uint32_t dictionary_count;
    uint32_t dictionary_bytes;
    uint32_t output_word_base;
    uint32_t output_word_count;
} rdna4_argument_summary_runtime_page;

typedef struct {
    uint64_t job_index;
    uint64_t chunk_index;
    uint64_t source_offset;
    uint32_t source_count;
    uint32_t source_overlap;
    uint32_t source_end;
    uint32_t source_copy_bytes;
    uint32_t source_binding_bytes;
    rdna4_argument_summary_runtime_page page;
    rdna4_argument_summary_push push;
} rdna4_argument_summary_runtime_job;

typedef struct {
    uint32_t source_staging_bytes;
    uint32_t dictionary_staging_bytes;
    uint32_t output_staging_bytes;
} rdna4_argument_summary_scratch;

typedef struct {
    uint32_t source_begin;
    uint32_t source_count;
    uint32_t group_count;
} rdna4_argument_summary_dispatch;

typedef struct {
    uint32_t source_count;
    uint32_t max_groups;
    uint32_t next_begin;
} rdna4_argument_summary_dispatch_cursor;

static inline bool rdna4_argument_summary_runtime_pow2(uint32_t value) {
    return value && !(value & (value - 1u));
}

static inline uint64_t rdna4_argument_summary_runtime_align_down(
    uint64_t value, uint64_t alignment) {
    return value & ~(alignment - 1u);
}

static inline bool rdna4_argument_summary_runtime_add_u64(
    uint64_t a, uint64_t b, uint64_t *out) {
    if (a > UINT64_MAX - b) return false;
    if (out) *out = a + b;
    return true;
}

static inline bool rdna4_argument_summary_runtime_mul_u64(
    uint64_t a, uint64_t b, uint64_t *out) {
    if (a && b > UINT64_MAX / a) return false;
    if (out) *out = a * b;
    return true;
}

static inline bool rdna4_argument_summary_runtime_round_words(
    uint64_t bytes, uint32_t *out) {
    if (bytes > UINT32_MAX - 3u) return false;
    if (out) *out = (uint32_t)((bytes + 3u) & ~UINT64_C(3));
    return true;
}

/*
 * Runtime job order is dictionary page outer, source chunk inner. That order
 * lets one attempt reuse one dictionary-page scratch and one output-page
 * scratch across every chunk and every page without per-dispatch allocations.
 */
static inline rdna4_argument_summary_status
rdna4_argument_summary_runtime_plan_init(
    rdna4_argument_summary_runtime_plan *plan,
    const rdna4_argument_summary_runtime_limits *limits,
    uint64_t source_size, uint32_t candidate_count, uint64_t generation) {
    if (!plan || !limits || !limits->max_storage_buffer_range ||
        !limits->max_compute_workgroup_count_x ||
        limits->max_compute_workgroup_invocations <
            RDNA4_ARGUMENT_SUMMARY_WORKGROUP_SIZE ||
        limits->max_compute_workgroup_size_x <
            RDNA4_ARGUMENT_SUMMARY_WORKGROUP_SIZE)
        return RDNA4_ARGUMENT_SUMMARY_INVALID;

    uint32_t alignment = limits->min_storage_buffer_offset_alignment;
    if (!alignment) alignment = RDNA4_ARGUMENT_SUMMARY_MIN_COPY_ALIGN;
    if (!rdna4_argument_summary_runtime_pow2(alignment))
        return RDNA4_ARGUMENT_SUMMARY_INVALID;
    if (alignment < RDNA4_ARGUMENT_SUMMARY_MIN_COPY_ALIGN)
        alignment = RDNA4_ARGUMENT_SUMMARY_MIN_COPY_ALIGN;

    uint64_t max_payload = limits->max_storage_buffer_range >
            RDNA4_ARGUMENT_SUMMARY_OVERLAP_BYTES
        ? (uint64_t)limits->max_storage_buffer_range -
              RDNA4_ARGUMENT_SUMMARY_OVERLAP_BYTES
        : 0;
    uint32_t requested_chunk = limits->requested_chunk_bytes
        ? limits->requested_chunk_bytes
        : RDNA4_ARGUMENT_SUMMARY_DEFAULT_CHUNK_BYTES;
    uint64_t chunk_bytes = requested_chunk < max_payload
        ? requested_chunk : max_payload;
    chunk_bytes = rdna4_argument_summary_runtime_align_down(
        chunk_bytes, alignment);
    if (!chunk_bytes || chunk_bytes > UINT32_MAX)
        return RDNA4_ARGUMENT_SUMMARY_INVALID;

    uint32_t source_binding_bytes = 0;
    if (!rdna4_argument_summary_runtime_round_words(
            chunk_bytes + RDNA4_ARGUMENT_SUMMARY_OVERLAP_BYTES,
            &source_binding_bytes) ||
        source_binding_bytes > limits->max_storage_buffer_range)
        return RDNA4_ARGUMENT_SUMMARY_OVERFLOW;

    uint64_t page_capacity =
        (uint64_t)limits->max_storage_buffer_range /
        sizeof(rdna4_argument_summary_candidate);
    page_capacity = rdna4_argument_summary_runtime_align_down(
        page_capacity, RDNA4_ARGUMENT_SUMMARY_DICTIONARY_ALIGN);
    if (page_capacity > UINT32_MAX)
        page_capacity = UINT32_MAX;
    uint32_t requested_page = limits->requested_dictionary_page
        ? limits->requested_dictionary_page
        : RDNA4_ARGUMENT_SUMMARY_DEFAULT_DICTIONARY_PAGE;
    uint32_t dictionary_page_size = requested_page < page_capacity
        ? requested_page : (uint32_t)page_capacity;
    dictionary_page_size = (uint32_t)rdna4_argument_summary_runtime_align_down(
        dictionary_page_size, RDNA4_ARGUMENT_SUMMARY_DICTIONARY_ALIGN);
    if (!dictionary_page_size)
        return RDNA4_ARGUMENT_SUMMARY_INVALID;

    uint64_t dictionary_binding_bytes =
        (uint64_t)dictionary_page_size *
        sizeof(rdna4_argument_summary_candidate);
    uint32_t output_page_bytes =
        dictionary_page_size / RDNA4_ARGUMENT_SUMMARY_DICTIONARY_ALIGN * 4u;
    if (dictionary_binding_bytes > limits->max_storage_buffer_range ||
        output_page_bytes > limits->max_storage_buffer_range)
        return RDNA4_ARGUMENT_SUMMARY_OVERFLOW;

    uint64_t chunk_count = source_size
        ? 1u + (source_size - 1u) / chunk_bytes
        : 0u;
    uint64_t page_count64 = candidate_count
        ? 1u + ((uint64_t)candidate_count - 1u) / dictionary_page_size
        : 0u;
    if (page_count64 > UINT32_MAX)
        return RDNA4_ARGUMENT_SUMMARY_OVERFLOW;
    uint64_t job_count = 0;
    if (chunk_count && page_count64 &&
        !rdna4_argument_summary_runtime_mul_u64(
            chunk_count, page_count64, &job_count))
        return RDNA4_ARGUMENT_SUMMARY_OVERFLOW;

    *plan = (rdna4_argument_summary_runtime_plan){
        .generation = generation,
        .source_size = source_size,
        .chunk_count = chunk_count,
        .job_count = job_count,
        .chunk_bytes = (uint32_t)chunk_bytes,
        .candidate_count = candidate_count,
        .dictionary_page_size = dictionary_page_size,
        .dictionary_page_count = (uint32_t)page_count64,
        .output_word_count =
            rdna4_argument_summary_output_words(candidate_count),
        .source_binding_bytes = source_binding_bytes,
        .dictionary_binding_bytes = (uint32_t)dictionary_binding_bytes,
        .output_page_bytes = output_page_bytes,
    };
    return RDNA4_ARGUMENT_SUMMARY_OK;
}

static inline rdna4_argument_summary_status
rdna4_argument_summary_runtime_page_get(
    const rdna4_argument_summary_runtime_plan *plan, uint32_t page_index,
    rdna4_argument_summary_runtime_page *page) {
    if (!plan || !page || page_index >= plan->dictionary_page_count)
        return RDNA4_ARGUMENT_SUMMARY_INVALID;
    uint64_t begin =
        (uint64_t)page_index * plan->dictionary_page_size;
    if (begin >= plan->candidate_count)
        return RDNA4_ARGUMENT_SUMMARY_OVERFLOW;
    uint64_t remaining = (uint64_t)plan->candidate_count - begin;
    uint64_t count = remaining < plan->dictionary_page_size
        ? remaining : plan->dictionary_page_size;
    uint64_t byte_offset = 0;
    if (!rdna4_argument_summary_runtime_mul_u64(
            begin, sizeof(rdna4_argument_summary_candidate), &byte_offset) ||
        count > UINT32_MAX ||
        count * sizeof(rdna4_argument_summary_candidate) > UINT32_MAX)
        return RDNA4_ARGUMENT_SUMMARY_OVERFLOW;

    *page = (rdna4_argument_summary_runtime_page){
        .dictionary_offset = byte_offset,
        .page_index = page_index,
        .dictionary_begin = (uint32_t)begin,
        .dictionary_count = (uint32_t)count,
        .dictionary_bytes =
            (uint32_t)(count * sizeof(rdna4_argument_summary_candidate)),
        .output_word_base =
            (uint32_t)(begin / RDNA4_ARGUMENT_SUMMARY_DICTIONARY_ALIGN),
        .output_word_count =
            (uint32_t)((count + 31u) / RDNA4_ARGUMENT_SUMMARY_DICTIONARY_ALIGN),
    };
    return RDNA4_ARGUMENT_SUMMARY_OK;
}

static inline rdna4_argument_summary_status
rdna4_argument_summary_runtime_job_get(
    const rdna4_argument_summary_runtime_plan *plan, uint64_t job_index,
    rdna4_argument_summary_runtime_job *job) {
    if (!plan || !job || !plan->chunk_count ||
        !plan->dictionary_page_count || job_index >= plan->job_count)
        return RDNA4_ARGUMENT_SUMMARY_INVALID;

    uint64_t page_index = job_index / plan->chunk_count;
    uint64_t chunk_index = job_index % plan->chunk_count;
    if (page_index > UINT32_MAX)
        return RDNA4_ARGUMENT_SUMMARY_OVERFLOW;
    rdna4_argument_summary_runtime_page page;
    rdna4_argument_summary_status status =
        rdna4_argument_summary_runtime_page_get(
            plan, (uint32_t)page_index, &page);
    if (status != RDNA4_ARGUMENT_SUMMARY_OK)
        return status;

    uint64_t source_offset = 0;
    if (!rdna4_argument_summary_runtime_mul_u64(
            chunk_index, plan->chunk_bytes, &source_offset) ||
        source_offset >= plan->source_size)
        return RDNA4_ARGUMENT_SUMMARY_OVERFLOW;
    uint64_t remaining = plan->source_size - source_offset;
    uint64_t source_count = remaining < plan->chunk_bytes
        ? remaining : plan->chunk_bytes;
    uint64_t after = remaining - source_count;
    uint64_t overlap = after < RDNA4_ARGUMENT_SUMMARY_OVERLAP_BYTES
        ? after : RDNA4_ARGUMENT_SUMMARY_OVERLAP_BYTES;
    uint64_t source_end = source_count + overlap;
    uint32_t binding_bytes = 0;
    if (source_count > UINT32_MAX || source_end > UINT32_MAX ||
        overlap > UINT32_MAX ||
        !rdna4_argument_summary_runtime_round_words(
            source_end, &binding_bytes))
        return RDNA4_ARGUMENT_SUMMARY_OVERFLOW;

    *job = (rdna4_argument_summary_runtime_job){
        .job_index = job_index,
        .chunk_index = chunk_index,
        .source_offset = source_offset,
        .source_count = (uint32_t)source_count,
        .source_overlap = (uint32_t)overlap,
        .source_end = (uint32_t)source_end,
        .source_copy_bytes = (uint32_t)source_end,
        .source_binding_bytes = binding_bytes,
        .page = page,
        .push = {
            .source_begin = 0,
            .source_count = (uint32_t)source_count,
            .source_end = (uint32_t)source_end,
            .dictionary_begin = page.dictionary_begin,
            .dictionary_count = page.dictionary_count,
            .dictionary_page = page.page_index,
            .dictionary_page_count = plan->dictionary_page_count,
            .output_word_base = page.output_word_base,
        },
    };
    return RDNA4_ARGUMENT_SUMMARY_OK;
}

typedef struct {
    uint64_t source_offset;
    uint32_t source_binding_bytes;
    uint32_t copy_bytes;
} rdna4_argument_summary_source_copy;

static inline rdna4_argument_summary_status
rdna4_argument_summary_runtime_source_copy(
    const rdna4_argument_summary_runtime_job *job,
    uint64_t source_range_offset, uint64_t source_range_size,
    rdna4_argument_summary_source_copy *copy) {
    if (!job || !copy || source_range_offset > UINT64_MAX - job->source_offset)
        return RDNA4_ARGUMENT_SUMMARY_INVALID;
    uint64_t begin = source_range_offset + job->source_offset;
    uint64_t range_end = 0;
    uint64_t copy_end = 0;
    if (!rdna4_argument_summary_runtime_add_u64(
            source_range_offset, source_range_size, &range_end) ||
        !rdna4_argument_summary_runtime_add_u64(
            begin, job->source_copy_bytes, &copy_end) ||
        begin > range_end || copy_end > range_end)
        return RDNA4_ARGUMENT_SUMMARY_OVERFLOW;
    *copy = (rdna4_argument_summary_source_copy){
        .source_offset = begin,
        .source_binding_bytes = job->source_binding_bytes,
        .copy_bytes = job->source_copy_bytes,
    };
    return RDNA4_ARGUMENT_SUMMARY_OK;
}

static inline rdna4_argument_summary_scratch
rdna4_argument_summary_runtime_scratch(
    const rdna4_argument_summary_runtime_plan *plan) {
    if (!plan) return (rdna4_argument_summary_scratch){0};
    return (rdna4_argument_summary_scratch){
        .source_staging_bytes =
            plan->job_count ? plan->source_binding_bytes : 0,
        .dictionary_staging_bytes =
            plan->job_count ? plan->dictionary_binding_bytes : 0,
        .output_staging_bytes =
            plan->dictionary_page_count ? plan->output_page_bytes : 0,
    };
}

static inline rdna4_argument_summary_status
rdna4_argument_summary_runtime_dispatch_init(
    rdna4_argument_summary_dispatch_cursor *cursor, uint32_t source_count,
    uint32_t max_groups) {
    if (!cursor || !max_groups) return RDNA4_ARGUMENT_SUMMARY_INVALID;
    *cursor = (rdna4_argument_summary_dispatch_cursor){
        .source_count = source_count,
        .max_groups = max_groups,
    };
    return RDNA4_ARGUMENT_SUMMARY_OK;
}

static inline bool rdna4_argument_summary_runtime_dispatch_next(
    rdna4_argument_summary_dispatch_cursor *cursor,
    rdna4_argument_summary_dispatch *dispatch) {
    if (!cursor || !dispatch || cursor->next_begin >= cursor->source_count)
        return false;
    uint64_t group_bytes =
        (uint64_t)cursor->max_groups *
        RDNA4_ARGUMENT_SUMMARY_WORKGROUP_SIZE;
    uint64_t remaining = cursor->source_count - cursor->next_begin;
    uint64_t count = remaining < group_bytes ? remaining : group_bytes;
    if (count > UINT32_MAX)
        count = UINT32_MAX;
    *dispatch = (rdna4_argument_summary_dispatch){
        .source_begin = cursor->next_begin,
        .source_count = (uint32_t)count,
        .group_count =
            (uint32_t)((count + RDNA4_ARGUMENT_SUMMARY_WORKGROUP_SIZE - 1u) /
                       RDNA4_ARGUMENT_SUMMARY_WORKGROUP_SIZE),
    };
    cursor->next_begin += (uint32_t)count;
    return true;
}

/*
 * Pipeline creation is represented as callbacks so the rollback path can be
 * exercised without Vulkan. A retry after any failed stage starts from zero.
 */
typedef struct {
    void *context;
    int32_t (*create_shader)(void *context, uintptr_t *handle);
    int32_t (*create_descriptor_layout)(void *context, uintptr_t *handle);
    int32_t (*create_pipeline_layout)(void *context, uintptr_t shader,
                                      uintptr_t descriptor_layout,
                                      uintptr_t *handle);
    int32_t (*create_pipeline)(void *context, uintptr_t shader,
                               uintptr_t pipeline_layout, uintptr_t *handle);
    void (*destroy_shader)(void *context, uintptr_t handle);
    void (*destroy_descriptor_layout)(void *context, uintptr_t handle);
    void (*destroy_pipeline_layout)(void *context, uintptr_t handle);
    void (*destroy_pipeline)(void *context, uintptr_t handle);
} rdna4_argument_summary_pipeline_ops;

typedef struct {
    uintptr_t device_key;
    uintptr_t shader;
    uintptr_t descriptor_layout;
    uintptr_t pipeline_layout;
    uintptr_t pipeline;
    bool ready;
} rdna4_argument_summary_pipeline_state;

static inline void rdna4_argument_summary_pipeline_release(
    rdna4_argument_summary_pipeline_state *state,
    const rdna4_argument_summary_pipeline_ops *ops) {
    if (!state) return;
    if (ops) {
        if (state->pipeline && ops->destroy_pipeline)
            ops->destroy_pipeline(ops->context, state->pipeline);
        if (state->pipeline_layout && ops->destroy_pipeline_layout)
            ops->destroy_pipeline_layout(
                ops->context, state->pipeline_layout);
        if (state->descriptor_layout && ops->destroy_descriptor_layout)
            ops->destroy_descriptor_layout(
                ops->context, state->descriptor_layout);
        if (state->shader && ops->destroy_shader)
            ops->destroy_shader(ops->context, state->shader);
    }
    memset(state, 0, sizeof(*state));
}

static inline bool rdna4_argument_summary_pipeline_ops_valid(
    const rdna4_argument_summary_pipeline_ops *ops) {
    return ops && ops->create_shader && ops->create_descriptor_layout &&
        ops->create_pipeline_layout && ops->create_pipeline &&
        ops->destroy_shader && ops->destroy_descriptor_layout &&
        ops->destroy_pipeline_layout && ops->destroy_pipeline;
}

static inline rdna4_argument_summary_status
rdna4_argument_summary_pipeline_ensure(
    rdna4_argument_summary_pipeline_state *state,
    const rdna4_argument_summary_pipeline_ops *ops, uintptr_t device_key) {
    if (!state || !rdna4_argument_summary_pipeline_ops_valid(ops) ||
        !device_key)
        return RDNA4_ARGUMENT_SUMMARY_INVALID;
    if (state->ready) {
        return state->device_key == device_key
            ? RDNA4_ARGUMENT_SUMMARY_OK
            : RDNA4_ARGUMENT_SUMMARY_FAILED;
    }
    if (state->shader || state->descriptor_layout ||
        state->pipeline_layout || state->pipeline)
        rdna4_argument_summary_pipeline_release(state, ops);

    int32_t result = ops->create_shader(ops->context, &state->shader);
    if (result != 0 || !state->shader) goto fail;
    result = ops->create_descriptor_layout(
        ops->context, &state->descriptor_layout);
    if (result != 0 || !state->descriptor_layout) goto fail;
    result = ops->create_pipeline_layout(
        ops->context, state->shader, state->descriptor_layout,
        &state->pipeline_layout);
    if (result != 0 || !state->pipeline_layout) goto fail;
    result = ops->create_pipeline(
        ops->context, state->shader, state->pipeline_layout,
        &state->pipeline);
    if (result != 0 || !state->pipeline) goto fail;

    state->device_key = device_key;
    state->ready = true;
    return RDNA4_ARGUMENT_SUMMARY_OK;

fail:
    rdna4_argument_summary_pipeline_release(state, ops);
    return RDNA4_ARGUMENT_SUMMARY_FAILED;
}

typedef enum {
    RDNA4_ARGUMENT_SUMMARY_COMPLETION_ENCODING = 0,
    RDNA4_ARGUMENT_SUMMARY_COMPLETION_ENCODED,
    RDNA4_ARGUMENT_SUMMARY_COMPLETION_COMPLETE,
    RDNA4_ARGUMENT_SUMMARY_COMPLETION_FAILED,
    RDNA4_ARGUMENT_SUMMARY_COMPLETION_OBSOLETE,
} rdna4_argument_summary_completion_state;

typedef struct {
    uint64_t generation;
    uint64_t job_count;
    uint64_t encoded_jobs;
    rdna4_argument_summary_completion_state state;
} rdna4_argument_summary_completion;

static inline void rdna4_argument_summary_completion_init(
    rdna4_argument_summary_completion *completion,
    const rdna4_argument_summary_runtime_plan *plan) {
    if (!completion) return;
    *completion = (rdna4_argument_summary_completion){
        .generation = plan ? plan->generation : 0,
        .job_count = plan ? plan->job_count : 0,
        .state = RDNA4_ARGUMENT_SUMMARY_COMPLETION_ENCODING,
    };
}

static inline rdna4_argument_summary_status
rdna4_argument_summary_completion_job_encoded(
    rdna4_argument_summary_completion *completion) {
    if (!completion ||
        completion->state != RDNA4_ARGUMENT_SUMMARY_COMPLETION_ENCODING)
        return RDNA4_ARGUMENT_SUMMARY_INVALID;
    if (completion->encoded_jobs >= completion->job_count)
        return RDNA4_ARGUMENT_SUMMARY_OVERFLOW;
    ++completion->encoded_jobs;
    return RDNA4_ARGUMENT_SUMMARY_OK;
}

static inline rdna4_argument_summary_status
rdna4_argument_summary_completion_finish_encode(
    rdna4_argument_summary_completion *completion, bool success) {
    if (!completion ||
        completion->state != RDNA4_ARGUMENT_SUMMARY_COMPLETION_ENCODING)
        return RDNA4_ARGUMENT_SUMMARY_INVALID;
    if (!success) {
        completion->state = RDNA4_ARGUMENT_SUMMARY_COMPLETION_FAILED;
        return RDNA4_ARGUMENT_SUMMARY_FAILED;
    }
    if (completion->encoded_jobs != completion->job_count) {
        completion->state = RDNA4_ARGUMENT_SUMMARY_COMPLETION_FAILED;
        return RDNA4_ARGUMENT_SUMMARY_INCOMPLETE;
    }
    completion->state = RDNA4_ARGUMENT_SUMMARY_COMPLETION_ENCODED;
    return RDNA4_ARGUMENT_SUMMARY_OK;
}

static inline rdna4_argument_summary_status
rdna4_argument_summary_completion_fence(
    rdna4_argument_summary_completion *completion, bool fence_succeeded,
    uint64_t current_generation) {
    if (!completion) return RDNA4_ARGUMENT_SUMMARY_INVALID;
    if (completion->state == RDNA4_ARGUMENT_SUMMARY_COMPLETION_FAILED)
        return RDNA4_ARGUMENT_SUMMARY_FAILED;
    if (completion->state == RDNA4_ARGUMENT_SUMMARY_COMPLETION_OBSOLETE)
        return RDNA4_ARGUMENT_SUMMARY_OBSOLETE;
    if (completion->state == RDNA4_ARGUMENT_SUMMARY_COMPLETION_COMPLETE) {
        if (current_generation != completion->generation) {
            completion->state = RDNA4_ARGUMENT_SUMMARY_COMPLETION_OBSOLETE;
            return RDNA4_ARGUMENT_SUMMARY_STALE;
        }
        return RDNA4_ARGUMENT_SUMMARY_COMPLETE;
    }
    if (completion->state != RDNA4_ARGUMENT_SUMMARY_COMPLETION_ENCODED)
        return RDNA4_ARGUMENT_SUMMARY_INCOMPLETE;
    if (!fence_succeeded) {
        completion->state = RDNA4_ARGUMENT_SUMMARY_COMPLETION_FAILED;
        return RDNA4_ARGUMENT_SUMMARY_FAILED;
    }
    if (current_generation != completion->generation) {
        completion->state = RDNA4_ARGUMENT_SUMMARY_COMPLETION_OBSOLETE;
        return RDNA4_ARGUMENT_SUMMARY_STALE;
    }
    completion->state = RDNA4_ARGUMENT_SUMMARY_COMPLETION_COMPLETE;
    return RDNA4_ARGUMENT_SUMMARY_COMPLETE;
}

static inline rdna4_argument_summary_status
rdna4_argument_summary_completion_publish(
    rdna4_argument_summary_completion *completion,
    const rdna4_argument_summary_runtime_plan *plan,
    uint64_t current_generation,
    rdna4_argument_summary_publication *publication) {
    if (!completion || !plan || !publication)
        return RDNA4_ARGUMENT_SUMMARY_INVALID;
    *publication = (rdna4_argument_summary_publication){0};
    if (completion->state == RDNA4_ARGUMENT_SUMMARY_COMPLETION_FAILED)
        return RDNA4_ARGUMENT_SUMMARY_FAILED;
    if (completion->state == RDNA4_ARGUMENT_SUMMARY_COMPLETION_OBSOLETE)
        return RDNA4_ARGUMENT_SUMMARY_OBSOLETE;
    if (current_generation != completion->generation ||
        current_generation != plan->generation) {
        completion->state = RDNA4_ARGUMENT_SUMMARY_COMPLETION_OBSOLETE;
        return RDNA4_ARGUMENT_SUMMARY_STALE;
    }
    if (completion->state != RDNA4_ARGUMENT_SUMMARY_COMPLETION_COMPLETE)
        return RDNA4_ARGUMENT_SUMMARY_INCOMPLETE;
    if (completion->encoded_jobs != completion->job_count ||
        completion->job_count != plan->job_count)
        return RDNA4_ARGUMENT_SUMMARY_INCOMPLETE;
    *publication = (rdna4_argument_summary_publication){
        .generation = plan->generation,
        .source_size = plan->source_size,
        .completed_jobs = plan->job_count,
        .candidate_count = plan->candidate_count,
        .output_word_count = plan->output_word_count,
        .output_bytes =
            plan->output_word_count * (uint32_t)sizeof(uint32_t),
    };
    return RDNA4_ARGUMENT_SUMMARY_READY;
}

/* Vulkan bit values used by the host simulator and the bridge. */
#define RDNA4_ARGUMENT_SUMMARY_STAGE_TRANSFER 0x00001000u
#define RDNA4_ARGUMENT_SUMMARY_STAGE_COMPUTE 0x00000800u
#define RDNA4_ARGUMENT_SUMMARY_STAGE_HOST 0x00004000u
#define RDNA4_ARGUMENT_SUMMARY_ACCESS_TRANSFER_READ 0x00000800u
#define RDNA4_ARGUMENT_SUMMARY_ACCESS_TRANSFER_WRITE 0x00001000u
#define RDNA4_ARGUMENT_SUMMARY_ACCESS_SHADER_READ 0x00000020u
#define RDNA4_ARGUMENT_SUMMARY_ACCESS_SHADER_WRITE 0x00000040u
#define RDNA4_ARGUMENT_SUMMARY_ACCESS_HOST_READ 0x00002000u

/*
 * The bridge and the host simulator consume this exact operation sequence.
 * All fallible planning happens before recording, so sink callbacks are void.
 */
typedef struct {
    void *context;
    void (*bind)(void *context);
    void (*copy_dictionary)(void *context,
                            const rdna4_argument_summary_runtime_page *page);
    void (*fill_output)(void *context,
                        const rdna4_argument_summary_runtime_page *page,
                        uint32_t bytes);
    void (*copy_source)(void *context,
                        const rdna4_argument_summary_runtime_job *job,
                        const rdna4_argument_summary_source_copy *copy);
    void (*dispatch)(void *context,
                     const rdna4_argument_summary_runtime_job *job,
                     const rdna4_argument_summary_dispatch *dispatch,
                     const rdna4_argument_summary_push *push);
    void (*barrier)(void *context, uint32_t source_stage,
                    uint32_t source_access, uint32_t destination_stage,
                    uint32_t destination_access);
    void (*copy_output)(void *context,
                        const rdna4_argument_summary_runtime_page *page,
                        uint32_t bytes);
} rdna4_argument_summary_sink;

static inline bool rdna4_argument_summary_sink_valid(
    const rdna4_argument_summary_sink *sink) {
    return sink && sink->bind && sink->copy_dictionary &&
        sink->fill_output && sink->copy_source && sink->dispatch &&
        sink->barrier && sink->copy_output;
}

static inline rdna4_argument_summary_status
rdna4_argument_summary_runtime_record(
    const rdna4_argument_summary_runtime_plan *plan,
    rdna4_argument_summary_completion *completion,
    const rdna4_argument_summary_sink *sink, uint32_t max_groups) {
    if (!plan || !completion || !rdna4_argument_summary_sink_valid(sink) ||
        completion->state != RDNA4_ARGUMENT_SUMMARY_COMPLETION_ENCODING ||
        !max_groups)
        return RDNA4_ARGUMENT_SUMMARY_INVALID;

    if (plan->job_count)
        sink->bind(sink->context);
    for (uint32_t page_index = 0;
         page_index < plan->dictionary_page_count; ++page_index) {
        rdna4_argument_summary_runtime_page page;
        rdna4_argument_summary_status status =
            rdna4_argument_summary_runtime_page_get(
                plan, page_index, &page);
        if (status != RDNA4_ARGUMENT_SUMMARY_OK)
            return status;
        if (plan->job_count)
            sink->copy_dictionary(sink->context, &page);

        uint32_t output_bytes =
            page.output_word_count * (uint32_t)sizeof(uint32_t);
        sink->fill_output(sink->context, &page, output_bytes);
        sink->barrier(
            sink->context,
            RDNA4_ARGUMENT_SUMMARY_STAGE_TRANSFER,
            RDNA4_ARGUMENT_SUMMARY_ACCESS_TRANSFER_WRITE,
            RDNA4_ARGUMENT_SUMMARY_STAGE_TRANSFER |
                RDNA4_ARGUMENT_SUMMARY_STAGE_COMPUTE,
            RDNA4_ARGUMENT_SUMMARY_ACCESS_TRANSFER_READ |
                RDNA4_ARGUMENT_SUMMARY_ACCESS_SHADER_READ |
                RDNA4_ARGUMENT_SUMMARY_ACCESS_SHADER_WRITE);

        for (uint64_t chunk_index = 0;
             chunk_index < plan->chunk_count; ++chunk_index) {
            uint64_t job_index =
                (uint64_t)page_index * plan->chunk_count + chunk_index;
            rdna4_argument_summary_runtime_job job;
            status = rdna4_argument_summary_runtime_job_get(
                plan, job_index, &job);
            if (status != RDNA4_ARGUMENT_SUMMARY_OK)
                return status;
            rdna4_argument_summary_source_copy copy;
            status = rdna4_argument_summary_runtime_source_copy(
                &job, 0, plan->source_size, &copy);
            if (status != RDNA4_ARGUMENT_SUMMARY_OK)
                return status;
            sink->copy_source(sink->context, &job, &copy);
            sink->barrier(
                sink->context,
                RDNA4_ARGUMENT_SUMMARY_STAGE_TRANSFER,
                RDNA4_ARGUMENT_SUMMARY_ACCESS_TRANSFER_WRITE,
                RDNA4_ARGUMENT_SUMMARY_STAGE_COMPUTE,
                RDNA4_ARGUMENT_SUMMARY_ACCESS_SHADER_READ);

            rdna4_argument_summary_dispatch_cursor cursor;
            status = rdna4_argument_summary_runtime_dispatch_init(
                &cursor, job.source_count, max_groups);
            if (status != RDNA4_ARGUMENT_SUMMARY_OK)
                return status;
            rdna4_argument_summary_dispatch dispatch;
            while (rdna4_argument_summary_runtime_dispatch_next(
                       &cursor, &dispatch)) {
                rdna4_argument_summary_push push = job.push;
                push.source_begin = dispatch.source_begin;
                push.source_count = dispatch.source_count;
                sink->dispatch(sink->context, &job, &dispatch, &push);
            }
            sink->barrier(
                sink->context,
                RDNA4_ARGUMENT_SUMMARY_STAGE_COMPUTE,
                RDNA4_ARGUMENT_SUMMARY_ACCESS_SHADER_READ |
                    RDNA4_ARGUMENT_SUMMARY_ACCESS_SHADER_WRITE,
                RDNA4_ARGUMENT_SUMMARY_STAGE_TRANSFER,
                RDNA4_ARGUMENT_SUMMARY_ACCESS_TRANSFER_READ |
                    RDNA4_ARGUMENT_SUMMARY_ACCESS_TRANSFER_WRITE);
            status = rdna4_argument_summary_completion_job_encoded(completion);
            if (status != RDNA4_ARGUMENT_SUMMARY_OK)
                return status;
        }

        sink->copy_output(sink->context, &page, output_bytes);
        sink->barrier(
            sink->context,
            RDNA4_ARGUMENT_SUMMARY_STAGE_TRANSFER,
            RDNA4_ARGUMENT_SUMMARY_ACCESS_TRANSFER_READ |
                RDNA4_ARGUMENT_SUMMARY_ACCESS_TRANSFER_WRITE,
            RDNA4_ARGUMENT_SUMMARY_STAGE_TRANSFER |
                RDNA4_ARGUMENT_SUMMARY_STAGE_HOST,
            RDNA4_ARGUMENT_SUMMARY_ACCESS_TRANSFER_WRITE |
                RDNA4_ARGUMENT_SUMMARY_ACCESS_HOST_READ);
    }
    return RDNA4_ARGUMENT_SUMMARY_OK;
}

#if !defined(RDNA4_ARGUMENT_SUMMARY_RUNTIME_TEST)

#include "argument-summary/garnet_argument_summary_spv.h"

#include <pthread.h>


static pthread_mutex_t rdna4_argument_summary_pipeline_lock =
    PTHREAD_MUTEX_INITIALIZER;
static rdna4_argument_summary_pipeline_state rdna4_argument_summary_pipeline;

static NSError *rdna4_argument_summary_runtime_error(
    rdna4_argument_summary_status status, NSString *message) {
    return rdna4_err(49, [NSString stringWithFormat:@"argument summary (%d): %@",
                          (int)status, message]);
}

static int32_t rdna4_argument_summary_create_shader(
    void *context, uintptr_t *handle) {
    (void)context;
    VkShaderModule module = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof rdna4_argument_summary_cs_spv,
        .pCode = (const uint32_t *)(const void *)rdna4_argument_summary_cs_spv,
    };
    VkResult result = vkCreateShaderModule(
        RDNA4R.dev, &info, NULL, &module);
    *handle = (uintptr_t)module;
    return (int32_t)result;
}

static int32_t rdna4_argument_summary_create_descriptor_layout(
    void *context, uintptr_t *handle) {
    (void)context;
    VkDescriptorSetLayoutBinding bindings[3] = {
        {
            .binding = RDNA4_ARGUMENT_SUMMARY_BINDING_SOURCE,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        },
        {
            .binding = RDNA4_ARGUMENT_SUMMARY_BINDING_DICTIONARY,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        },
        {
            .binding = RDNA4_ARGUMENT_SUMMARY_BINDING_OUTPUT,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        },
    };
    VkDescriptorSetLayoutCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 3,
        .pBindings = bindings,
    };
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    VkResult result = vkCreateDescriptorSetLayout(
        RDNA4R.dev, &info, NULL, &layout);
    *handle = (uintptr_t)layout;
    return (int32_t)result;
}

static int32_t rdna4_argument_summary_create_pipeline_layout(
    void *context, uintptr_t shader, uintptr_t descriptor_layout,
    uintptr_t *handle) {
    (void)context;
    (void)shader;
    VkDescriptorSetLayout set = (VkDescriptorSetLayout)descriptor_layout;
    VkPushConstantRange push = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = sizeof(rdna4_argument_summary_push),
    };
    VkPipelineLayoutCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &set,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push,
    };
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkResult result = vkCreatePipelineLayout(
        RDNA4R.dev, &info, NULL, &layout);
    *handle = (uintptr_t)layout;
    return (int32_t)result;
}

static int32_t rdna4_argument_summary_create_pipeline(
    void *context, uintptr_t shader, uintptr_t pipeline_layout,
    uintptr_t *handle) {
    (void)context;
    VkComputePipelineCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .layout = (VkPipelineLayout)pipeline_layout,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = (VkShaderModule)shader,
            .pName = "main",
        },
    };
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkResult result = vkCreateComputePipelines(
        RDNA4R.dev, RDNA4R.pc, 1, &info, NULL, &pipeline);
    *handle = (uintptr_t)pipeline;
    return (int32_t)result;
}

static void rdna4_argument_summary_destroy_shader(
    void *context, uintptr_t handle) {
    (void)context;
    vkDestroyShaderModule(RDNA4R.dev, (VkShaderModule)handle, NULL);
}

static void rdna4_argument_summary_destroy_descriptor_layout(
    void *context, uintptr_t handle) {
    (void)context;
    vkDestroyDescriptorSetLayout(
        RDNA4R.dev, (VkDescriptorSetLayout)handle, NULL);
}

static void rdna4_argument_summary_destroy_pipeline_layout(
    void *context, uintptr_t handle) {
    (void)context;
    vkDestroyPipelineLayout(
        RDNA4R.dev, (VkPipelineLayout)handle, NULL);
}

static void rdna4_argument_summary_destroy_pipeline(
    void *context, uintptr_t handle) {
    (void)context;
    vkDestroyPipeline(RDNA4R.dev, (VkPipeline)handle, NULL);
}

static const rdna4_argument_summary_pipeline_ops
rdna4_argument_summary_vulkan_pipeline_ops = {
    .create_shader = rdna4_argument_summary_create_shader,
    .create_descriptor_layout =
        rdna4_argument_summary_create_descriptor_layout,
    .create_pipeline_layout = rdna4_argument_summary_create_pipeline_layout,
    .create_pipeline = rdna4_argument_summary_create_pipeline,
    .destroy_shader = rdna4_argument_summary_destroy_shader,
    .destroy_descriptor_layout =
        rdna4_argument_summary_destroy_descriptor_layout,
    .destroy_pipeline_layout = rdna4_argument_summary_destroy_pipeline_layout,
    .destroy_pipeline = rdna4_argument_summary_destroy_pipeline,
};

static rdna4_argument_summary_status
rdna4_argument_summary_ensure_pipeline(NSError **error) {
    rdna4_argument_summary_status status;
    pthread_mutex_lock(&rdna4_argument_summary_pipeline_lock);
    status = rdna4_argument_summary_pipeline_ensure(
        &rdna4_argument_summary_pipeline,
        &rdna4_argument_summary_vulkan_pipeline_ops,
        (uintptr_t)RDNA4R.dev);
    pthread_mutex_unlock(&rdna4_argument_summary_pipeline_lock);
    if (status != RDNA4_ARGUMENT_SUMMARY_OK && error)
        *error = rdna4_argument_summary_runtime_error(
            status, @"lazy shader/descriptor/pipeline creation failed");
    return status;
}

/*
 * RDNA4R has no vkDestroyDevice call site. Keep this teardown hook available
 * for a future explicit device teardown without registering an owner, atexit,
 * or another lifetime path. Until then it is intentionally unreferenced.
 */
static void __attribute__((unused))
rdna4_argument_summary_pipeline_destroy(void) {
    pthread_mutex_lock(&rdna4_argument_summary_pipeline_lock);
    rdna4_argument_summary_pipeline_release(
        &rdna4_argument_summary_pipeline,
        &rdna4_argument_summary_vulkan_pipeline_ops);
    pthread_mutex_unlock(&rdna4_argument_summary_pipeline_lock);
}

static BOOL rdna4_argument_summary_range_fits(
    NSUInteger length, uint64_t offset, uint64_t size) {
    return offset <= length && size <= (uint64_t)length - offset;
}

static BOOL rdna4_argument_summary_ranges_overlap(
    uint64_t a_offset, uint64_t a_size,
    uint64_t b_offset, uint64_t b_size) {
    if (!a_size || !b_size) return NO;
    return a_offset < b_offset + b_size &&
        b_offset < a_offset + a_size;
}

@implementation RDNA4ArgumentSummaryTask
- (instancetype)initWithSourceBuffer:(RDNA4Buffer *)sourceBuffer
                          sourceOffset:(NSUInteger)sourceOffset
                            sourceSize:(NSUInteger)sourceSize
                    dictionaryBuffer:(RDNA4Buffer *)dictionaryBuffer
                    dictionaryOffset:(NSUInteger)dictionaryOffset
                      candidateCount:(uint32_t)candidateCount
                        outputBuffer:(RDNA4Buffer *)outputBuffer
                        outputOffset:(NSUInteger)outputOffset
                          generation:(uint64_t)generation
                              limits:(const rdna4_argument_summary_runtime_limits *)limits
                  requestedChunkBytes:(uint32_t)requestedChunkBytes
             requestedDictionaryPage:(uint32_t)requestedDictionaryPage
                               error:(NSError **)error {
    self = [super init];
    if (!self) return nil;
    if (!sourceBuffer || ![sourceBuffer isKindOfClass:[RDNA4Buffer class]] ||
        !outputBuffer || ![outputBuffer isKindOfClass:[RDNA4Buffer class]] ||
        (candidateCount && (!dictionaryBuffer ||
         ![dictionaryBuffer isKindOfClass:[RDNA4Buffer class]]))) {
        if (error) *error = rdna4_argument_summary_runtime_error(
            RDNA4_ARGUMENT_SUMMARY_INVALID,
            @"source/output/dictionary buffer has the wrong type");
        return nil;
    }
    if (!rdna4_argument_summary_range_fits(
            sourceBuffer.length, sourceOffset, sourceSize)) {
        if (error) *error = rdna4_argument_summary_runtime_error(
            RDNA4_ARGUMENT_SUMMARY_INVALID,
            @"source range is outside the source buffer");
        return nil;
    }
    uint64_t dictionaryBytes =
        (uint64_t)candidateCount *
        sizeof(rdna4_argument_summary_candidate);
    if ((candidateCount &&
         !rdna4_argument_summary_range_fits(
             dictionaryBuffer.length, dictionaryOffset,
             dictionaryBytes))) {
        if (error) *error = rdna4_argument_summary_runtime_error(
            RDNA4_ARGUMENT_SUMMARY_INVALID,
            @"dictionary range is outside the dictionary buffer");
        return nil;
    }
    id device = sourceBuffer.device;
    if ((candidateCount && dictionaryBuffer.device != device) ||
        outputBuffer.device != device) {
        if (error) *error = rdna4_argument_summary_runtime_error(
            RDNA4_ARGUMENT_SUMMARY_INVALID,
            @"summary buffers belong to different devices");
        return nil;
    }
    if (!outputBuffer.contents || [outputBuffer rdna4HostReadOnly] ||
        outputBuffer.storageMode == MTLStorageModePrivate) {
        if (error) *error = rdna4_argument_summary_runtime_error(
            RDNA4_ARGUMENT_SUMMARY_INVALID,
            @"summary output must be a host-coherent Shared/Managed buffer");
        return nil;
    }
    if (outputOffset % 4u) {
        if (error) *error = rdna4_argument_summary_runtime_error(
            RDNA4_ARGUMENT_SUMMARY_INVALID,
            @"summary output range must begin on a 32-bit word");
        return nil;
    }

    rdna4_argument_summary_runtime_limits localLimits;
    if (!limits) {
        localLimits = (rdna4_argument_summary_runtime_limits){
            .max_storage_buffer_range =
                RDNA4R.lim.maxStorageBufferRange,
            .min_storage_buffer_offset_alignment =
                (uint32_t)RDNA4R.lim.minStorageBufferOffsetAlignment,
            .max_compute_workgroup_count_x =
                RDNA4R.lim.maxComputeWorkGroupCount[0],
            .max_compute_workgroup_invocations =
                RDNA4R.lim.maxComputeWorkGroupInvocations,
            .max_compute_workgroup_size_x =
                RDNA4R.lim.maxComputeWorkGroupSize[0],
            .requested_chunk_bytes = requestedChunkBytes,
            .requested_dictionary_page = requestedDictionaryPage,
        };
    } else {
        localLimits = *limits;
        if (requestedChunkBytes)
            localLimits.requested_chunk_bytes = requestedChunkBytes;
        if (requestedDictionaryPage)
            localLimits.requested_dictionary_page =
                requestedDictionaryPage;
    }
    limits = &localLimits;
    rdna4_argument_summary_status status =
        rdna4_argument_summary_runtime_plan_init(
            &_plan, limits, sourceSize, candidateCount, generation);
    if (status != RDNA4_ARGUMENT_SUMMARY_OK) {
        if (error) *error = rdna4_argument_summary_runtime_error(
            status, @"invalid chunk/page limits or plan overflow");
        return nil;
    }
    uint64_t outputBytes =
        (uint64_t)_plan.output_word_count * sizeof(uint32_t);
    if (!rdna4_argument_summary_range_fits(
            outputBuffer.length, outputOffset, outputBytes)) {
        if (error) *error = rdna4_argument_summary_runtime_error(
            RDNA4_ARGUMENT_SUMMARY_INVALID,
            @"output bitset is outside the output buffer");
        return nil;
    }
    if ((outputBuffer == sourceBuffer &&
         rdna4_argument_summary_ranges_overlap(
             outputOffset, outputBytes, sourceOffset, sourceSize)) ||
        (outputBuffer == dictionaryBuffer && candidateCount &&
         rdna4_argument_summary_ranges_overlap(
             outputOffset, outputBytes, dictionaryOffset,
             dictionaryBytes))) {
        if (error) *error = rdna4_argument_summary_runtime_error(
            RDNA4_ARGUMENT_SUMMARY_INVALID,
            @"output bitset overlaps a read-only summary range");
        return nil;
    }

    _sourceBuffer = sourceBuffer;
    _dictionaryBuffer = dictionaryBuffer;
    _outputBuffer = outputBuffer;
    _sourceOffset = sourceOffset;
    _dictionaryOffset = dictionaryOffset;
    _outputOffset = outputOffset;
    return self;
}

- (uint64_t)generation { return _plan.generation; }
- (uint32_t)outputWordCount { return _plan.output_word_count; }
- (RDNA4Buffer *)sourceBuffer { return _sourceBuffer; }
- (RDNA4Buffer *)dictionaryBuffer { return _dictionaryBuffer; }
- (RDNA4Buffer *)outputBuffer { return _outputBuffer; }
- (uint64_t)sourceOffset { return _sourceOffset; }
- (uint64_t)dictionaryOffset { return _dictionaryOffset; }
- (uint64_t)outputOffset { return _outputOffset; }
- (const rdna4_argument_summary_runtime_plan *)runtimePlan { return &_plan; }
@end

GARNET_INTERNAL RDNA4ArgumentSummaryTask *rdna4_argument_summary_task_new(
    RDNA4Buffer *sourceBuffer, NSUInteger sourceOffset,
    NSUInteger sourceSize, RDNA4Buffer *dictionaryBuffer,
    NSUInteger dictionaryOffset, uint32_t candidateCount,
    RDNA4Buffer *outputBuffer, NSUInteger outputOffset,
    uint64_t generation,
    const rdna4_argument_summary_runtime_limits *limits,
    uint32_t requestedChunkBytes, uint32_t requestedDictionaryPage,
    NSError **error) {
    return [[RDNA4ArgumentSummaryTask alloc]
        initWithSourceBuffer:sourceBuffer
                sourceOffset:sourceOffset
                  sourceSize:sourceSize
          dictionaryBuffer:dictionaryBuffer
          dictionaryOffset:dictionaryOffset
            candidateCount:candidateCount
              outputBuffer:outputBuffer
              outputOffset:outputOffset
                generation:generation
                    limits:limits
        requestedChunkBytes:requestedChunkBytes
   requestedDictionaryPage:requestedDictionaryPage
                     error:error];
}

static RDNA4Buffer *rdna4_argument_summary_private_scratch(
    id device, uint32_t bytes, NSError **error) {
    if (!bytes) return nil;
    NSUInteger options = MTLResourceStorageModePrivate;
    if (RDNA4P.on) options |= RDNA4_OPT_NOZERO;
    return [[RDNA4Buffer alloc] initWithDevice:device
                                        length:bytes
                                       options:options
                                         error:error];
}

static BOOL rdna4_argument_summary_make_descriptor_set(
    RDNA4ArgumentSummaryAttempt *attempt, VkDevice device, NSError **error) {
    VkDescriptorPoolSize size = {
        .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 3,
    };
    VkDescriptorPoolCreateInfo poolInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &size,
    };
    VkResult result = vkCreateDescriptorPool(
        device, &poolInfo, NULL, &attempt->_descriptorPool);
    if (result != VK_SUCCESS) {
        if (error) *error = rdna4_argument_summary_runtime_error(
            RDNA4_ARGUMENT_SUMMARY_FAILED,
            [NSString stringWithFormat:
                @"vkCreateDescriptorPool(summary) = %d", result]);
        return NO;
    }
    VkDescriptorSetLayout setLayout =
        (VkDescriptorSetLayout)
            rdna4_argument_summary_pipeline.descriptor_layout;
    VkDescriptorSetAllocateInfo allocate = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = attempt->_descriptorPool,
        .descriptorSetCount = 1,
        .pSetLayouts = &setLayout,
    };
    result = vkAllocateDescriptorSets(
        device, &allocate, &attempt->_descriptorSet);
    if (result != VK_SUCCESS) {
        if (error) *error = rdna4_argument_summary_runtime_error(
            RDNA4_ARGUMENT_SUMMARY_FAILED,
            [NSString stringWithFormat:
                @"vkAllocateDescriptorSets(summary) = %d", result]);
        return NO;
    }

    RDNA4ArgumentSummaryTask *task = attempt->_task;
    VkDescriptorBufferInfo buffers[3] = {
        {
            .buffer = [attempt->_sourceScratch vkBuffer],
            .offset = [attempt->_sourceScratch rdna4VkOffset],
            .range = task->_plan.source_binding_bytes,
        },
        {
            .buffer = [attempt->_dictionaryScratch vkBuffer],
            .offset = [attempt->_dictionaryScratch rdna4VkOffset],
            .range = task->_plan.dictionary_binding_bytes,
        },
        {
            .buffer = [attempt->_outputScratch vkBuffer],
            .offset = [attempt->_outputScratch rdna4VkOffset],
            .range = task->_plan.output_page_bytes,
        },
    };
    VkWriteDescriptorSet writes[3];
    for (uint32_t i = 0; i < 3; ++i) {
        writes[i] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = attempt->_descriptorSet,
            .dstBinding = i,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &buffers[i],
        };
    }
    vkUpdateDescriptorSets(device, 3, writes, 0, NULL);
    return YES;
}

@implementation RDNA4ArgumentSummaryAttempt
- (instancetype)initWithTask:(RDNA4ArgumentSummaryTask *)task
                      device:(VkDevice)device
                       error:(NSError **)error {
    self = [super init];
    if (!self) return nil;
    _task = task;
    rdna4_argument_summary_completion_init(&_completion, &task->_plan);
    rdna4_argument_summary_scratch scratch =
        rdna4_argument_summary_runtime_scratch(&task->_plan);
    id metalDevice = task->_sourceBuffer.device;

    if (scratch.source_staging_bytes) {
        _sourceScratch = rdna4_argument_summary_private_scratch(
            metalDevice, scratch.source_staging_bytes, error);
        if (!_sourceScratch) return nil;
    }
    if (scratch.dictionary_staging_bytes) {
        _dictionaryScratch = rdna4_argument_summary_private_scratch(
            metalDevice, scratch.dictionary_staging_bytes, error);
        if (!_dictionaryScratch) return nil;
    }
    if (scratch.output_staging_bytes) {
        _outputScratch = rdna4_argument_summary_private_scratch(
            metalDevice, scratch.output_staging_bytes, error);
        if (!_outputScratch) return nil;
    }
    if (task->_plan.job_count &&
        !rdna4_argument_summary_make_descriptor_set(self, device, error))
        return nil;
    return self;
}

- (void)dealloc {
    if (_descriptorPool && RDNA4R.ok)
        vkDestroyDescriptorPool(RDNA4R.dev, _descriptorPool, NULL);
}

- (uint64_t)generation { return _completion.generation; }
- (uint64_t)jobCount { return _completion.job_count; }
- (uint64_t)encodedJobs { return _completion.encoded_jobs; }
@end

static BOOL rdna4_argument_summary_preflight(
    RDNA4ArgumentSummaryTask *task, NSError **error) {
    const rdna4_argument_summary_runtime_plan *plan = &task->_plan;
    rdna4_argument_summary_scratch scratch =
        rdna4_argument_summary_runtime_scratch(plan);
    if (scratch.source_staging_bytes > plan->source_binding_bytes ||
        scratch.dictionary_staging_bytes > plan->dictionary_binding_bytes ||
        scratch.output_staging_bytes > plan->output_page_bytes) {
        if (error) *error = rdna4_argument_summary_runtime_error(
            RDNA4_ARGUMENT_SUMMARY_INVALID,
            @"scratch sizes exceed their descriptor ranges");
        return NO;
    }
    for (uint32_t page_index = 0;
         page_index < plan->dictionary_page_count; ++page_index) {
        rdna4_argument_summary_runtime_page page = {0};
        rdna4_argument_summary_status status =
            rdna4_argument_summary_runtime_page_get(
                plan, page_index, &page);
        uint32_t pageBytes = status == RDNA4_ARGUMENT_SUMMARY_OK
            ? page.output_word_count * (uint32_t)sizeof(uint32_t) : 0;
        uint64_t dictionaryBegin =
            task->_dictionaryOffset + page.dictionary_offset;
        uint64_t outputBegin =
            task->_outputOffset +
            (uint64_t)page.output_word_base * sizeof(uint32_t);
        if (status != RDNA4_ARGUMENT_SUMMARY_OK ||
            (page.dictionary_bytes &&
             !rdna4_argument_summary_range_fits(
                 task->_dictionaryBuffer.length, dictionaryBegin,
                 page.dictionary_bytes)) ||
            (pageBytes &&
             !rdna4_argument_summary_range_fits(
                 task->_outputBuffer.length, outputBegin, pageBytes)) ||
            page.output_word_base + page.output_word_count >
                plan->output_word_count) {
            if (error) *error = rdna4_argument_summary_runtime_error(
                RDNA4_ARGUMENT_SUMMARY_INVALID,
                @"dictionary/output page is outside its final range");
            return NO;
        }
    }
    for (uint64_t index = 0; index < plan->job_count; ++index) {
        rdna4_argument_summary_runtime_job job;
        rdna4_argument_summary_status status =
            rdna4_argument_summary_runtime_job_get(plan, index, &job);
        if (status != RDNA4_ARGUMENT_SUMMARY_OK) {
            if (error) *error = rdna4_argument_summary_runtime_error(
                status, @"invalid runtime job");
            return NO;
        }
        rdna4_argument_summary_source_copy copy;
        status = rdna4_argument_summary_runtime_source_copy(
            &job, task->_sourceOffset,
            plan->source_size, &copy);
        if (status != RDNA4_ARGUMENT_SUMMARY_OK ||
            copy.source_binding_bytes > plan->source_binding_bytes ||
            copy.copy_bytes > copy.source_binding_bytes) {
            if (error) *error = rdna4_argument_summary_runtime_error(
                RDNA4_ARGUMENT_SUMMARY_INVALID,
                @"source copy or padded binding is out of bounds");
            return NO;
        }
        rdna4_argument_summary_dispatch_cursor dispatch;
        status = rdna4_argument_summary_runtime_dispatch_init(
            &dispatch, job.source_count,
            RDNA4R.lim.maxComputeWorkGroupCount[0]);
        if (status != RDNA4_ARGUMENT_SUMMARY_OK) {
            if (error) *error = rdna4_argument_summary_runtime_error(
                status, @"invalid compute workgroup-count limit");
            return NO;
        }
        rdna4_argument_summary_dispatch part;
        while (rdna4_argument_summary_runtime_dispatch_next(
                   &dispatch, &part)) {
            if (!part.source_count || !part.group_count ||
                part.group_count >
                    RDNA4R.lim.maxComputeWorkGroupCount[0]) {
                if (error) *error = rdna4_argument_summary_runtime_error(
                    RDNA4_ARGUMENT_SUMMARY_INVALID,
                    @"dispatch partition exceeds the device limit");
                return NO;
            }
        }
    }
    return YES;
}

static void rdna4_argument_summary_barrier(
    VkCommandBuffer command, VkPipelineStageFlags source_stage,
    VkAccessFlags source_access, VkPipelineStageFlags destination_stage,
    VkAccessFlags destination_access) {
    VkMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = source_access,
        .dstAccessMask = destination_access,
    };
    vkCmdPipelineBarrier(
        command, source_stage, destination_stage, 0,
        1, &barrier, 0, NULL, 0, NULL);
}

static BOOL rdna4_argument_summary_vulkan_offset(
    RDNA4Buffer *buffer, uint64_t logical_offset, uint64_t size,
    VkDeviceSize *offset) {
    if (!buffer || !offset ||
        !rdna4_argument_summary_range_fits(
            buffer.length, logical_offset, size))
        return NO;
    uint64_t vk_offset = [buffer rdna4VkOffset];
    if (vk_offset > UINT64_MAX - logical_offset)
        return NO;
    *offset = (VkDeviceSize)(vk_offset + logical_offset);
    return YES;
}

_Static_assert(RDNA4_ARGUMENT_SUMMARY_STAGE_TRANSFER ==
                   VK_PIPELINE_STAGE_TRANSFER_BIT,
               "summary transfer-stage ABI");
_Static_assert(RDNA4_ARGUMENT_SUMMARY_STAGE_COMPUTE ==
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
               "summary compute-stage ABI");
_Static_assert(RDNA4_ARGUMENT_SUMMARY_STAGE_HOST == VK_PIPELINE_STAGE_HOST_BIT,
               "summary host-stage ABI");
_Static_assert(RDNA4_ARGUMENT_SUMMARY_ACCESS_TRANSFER_READ ==
                   VK_ACCESS_TRANSFER_READ_BIT,
               "summary transfer-read ABI");
_Static_assert(RDNA4_ARGUMENT_SUMMARY_ACCESS_TRANSFER_WRITE ==
                   VK_ACCESS_TRANSFER_WRITE_BIT,
               "summary transfer-write ABI");
_Static_assert(RDNA4_ARGUMENT_SUMMARY_ACCESS_SHADER_READ ==
                   VK_ACCESS_SHADER_READ_BIT,
               "summary shader-read ABI");
_Static_assert(RDNA4_ARGUMENT_SUMMARY_ACCESS_SHADER_WRITE ==
                   VK_ACCESS_SHADER_WRITE_BIT,
               "summary shader-write ABI");
_Static_assert(RDNA4_ARGUMENT_SUMMARY_ACCESS_HOST_READ ==
                   VK_ACCESS_HOST_READ_BIT,
               "summary host-read ABI");

typedef struct {
    RDNA4ArgumentSummaryTask *task;
    RDNA4ArgumentSummaryAttempt *attempt;
    VkCommandBuffer command;
} rdna4_argument_summary_sink_context;

static void rdna4_argument_summary_sink_bind(void *opaque) {
    rdna4_argument_summary_sink_context *context = opaque;
    vkCmdBindPipeline(
        context->command, VK_PIPELINE_BIND_POINT_COMPUTE,
        (VkPipeline)rdna4_argument_summary_pipeline.pipeline);
    vkCmdBindDescriptorSets(
        context->command, VK_PIPELINE_BIND_POINT_COMPUTE,
        (VkPipelineLayout)rdna4_argument_summary_pipeline.pipeline_layout,
        0, 1, &context->attempt->_descriptorSet, 0, NULL);
}

static void rdna4_argument_summary_sink_copy_dictionary(
    void *opaque, const rdna4_argument_summary_runtime_page *page) {
    rdna4_argument_summary_sink_context *context = opaque;
    RDNA4ArgumentSummaryTask *task = context->task;
    RDNA4ArgumentSummaryAttempt *attempt = context->attempt;
    VkDeviceSize source = 0;
    VkDeviceSize destination = 0;
    (void)rdna4_argument_summary_vulkan_offset(
        task->_dictionaryBuffer,
        task->_dictionaryOffset + page->dictionary_offset,
        page->dictionary_bytes, &source);
    (void)rdna4_argument_summary_vulkan_offset(
        attempt->_dictionaryScratch, 0, page->dictionary_bytes,
        &destination);
    VkBufferCopy copy = {
        .srcOffset = source,
        .dstOffset = destination,
        .size = page->dictionary_bytes,
    };
    vkCmdCopyBuffer(
        context->command, [task->_dictionaryBuffer vkBuffer],
        [attempt->_dictionaryScratch vkBuffer], 1, &copy);
}

static void rdna4_argument_summary_sink_fill_output(
    void *opaque, const rdna4_argument_summary_runtime_page *page,
    uint32_t bytes) {
    (void)page;
    rdna4_argument_summary_sink_context *context = opaque;
    VkDeviceSize offset = 0;
    (void)rdna4_argument_summary_vulkan_offset(
        context->attempt->_outputScratch, 0, bytes, &offset);
    vkCmdFillBuffer(
        context->command,
        [context->attempt->_outputScratch vkBuffer],
        offset, bytes, 0);
}

static void rdna4_argument_summary_sink_copy_source(
    void *opaque, const rdna4_argument_summary_runtime_job *job,
    const rdna4_argument_summary_source_copy *copy) {
    (void)job;
    rdna4_argument_summary_sink_context *context = opaque;
    RDNA4ArgumentSummaryTask *task = context->task;
    uint64_t logical = task->_sourceOffset + copy->source_offset;
    VkDeviceSize source = 0;
    VkDeviceSize destination = 0;
    (void)rdna4_argument_summary_vulkan_offset(
        task->_sourceBuffer, logical, copy->copy_bytes, &source);
    (void)rdna4_argument_summary_vulkan_offset(
        context->attempt->_sourceScratch, 0, copy->copy_bytes,
        &destination);
    VkBufferCopy region = {
        .srcOffset = source,
        .dstOffset = destination,
        .size = copy->copy_bytes,
    };
    vkCmdCopyBuffer(
        context->command, [task->_sourceBuffer vkBuffer],
        [context->attempt->_sourceScratch vkBuffer], 1, &region);
}

static void rdna4_argument_summary_sink_dispatch(
    void *opaque, const rdna4_argument_summary_runtime_job *job,
    const rdna4_argument_summary_dispatch *dispatch,
    const rdna4_argument_summary_push *push) {
    (void)job;
    rdna4_argument_summary_sink_context *context = opaque;
    vkCmdPushConstants(
        context->command,
        (VkPipelineLayout)rdna4_argument_summary_pipeline.pipeline_layout,
        VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof *push, push);
    vkCmdDispatch(
        context->command, dispatch->group_count, 1, 1);
}

static void rdna4_argument_summary_sink_barrier(
    void *opaque, uint32_t source_stage, uint32_t source_access,
    uint32_t destination_stage, uint32_t destination_access) {
    rdna4_argument_summary_sink_context *context = opaque;
    rdna4_argument_summary_barrier(
        context->command, (VkPipelineStageFlags)source_stage,
        (VkAccessFlags)source_access,
        (VkPipelineStageFlags)destination_stage,
        (VkAccessFlags)destination_access);
}

static void rdna4_argument_summary_sink_copy_output(
    void *opaque, const rdna4_argument_summary_runtime_page *page,
    uint32_t bytes) {
    rdna4_argument_summary_sink_context *context = opaque;
    RDNA4ArgumentSummaryTask *task = context->task;
    VkDeviceSize source = 0;
    VkDeviceSize destination = 0;
    (void)rdna4_argument_summary_vulkan_offset(
        context->attempt->_outputScratch, 0, bytes, &source);
    (void)rdna4_argument_summary_vulkan_offset(
        task->_outputBuffer,
        task->_outputOffset +
            (uint64_t)page->output_word_base * sizeof(uint32_t),
        bytes, &destination);
    VkBufferCopy copy = {
        .srcOffset = source,
        .dstOffset = destination,
        .size = bytes,
    };
    vkCmdCopyBuffer(
        context->command,
        [context->attempt->_outputScratch vkBuffer],
        [task->_outputBuffer vkBuffer], 1, &copy);
}

static rdna4_argument_summary_status rdna4_argument_summary_record(
    RDNA4ArgumentSummaryTask *task, RDNA4ArgumentSummaryAttempt *attempt,
    VkCommandBuffer command) {
    rdna4_argument_summary_sink_context context = {
        .task = task,
        .attempt = attempt,
        .command = command,
    };
    const rdna4_argument_summary_sink sink = {
        .context = &context,
        .bind = rdna4_argument_summary_sink_bind,
        .copy_dictionary = rdna4_argument_summary_sink_copy_dictionary,
        .fill_output = rdna4_argument_summary_sink_fill_output,
        .copy_source = rdna4_argument_summary_sink_copy_source,
        .dispatch = rdna4_argument_summary_sink_dispatch,
        .barrier = rdna4_argument_summary_sink_barrier,
        .copy_output = rdna4_argument_summary_sink_copy_output,
    };
    return rdna4_argument_summary_runtime_record(
        &task->_plan, &attempt->_completion, &sink,
        RDNA4R.lim.maxComputeWorkGroupCount[0]);
}

GARNET_INTERNAL BOOL rdna4_argument_summary_encode(
    RDNA4ArgumentSummaryTask *task, VkCommandBuffer command,
    RDNA4ArgumentSummaryAttempt **outAttempt, NSError **error) {
    if (outAttempt) *outAttempt = nil;
    if (!task || !command || !outAttempt) {
        if (error) *error = rdna4_argument_summary_runtime_error(
            RDNA4_ARGUMENT_SUMMARY_INVALID,
            @"encode needs a task, command buffer and attempt output");
        return NO;
    }
    if (!rdna4_argument_summary_preflight(task, error))
        return NO;
    if (task->_plan.job_count &&
        rdna4_argument_summary_ensure_pipeline(error) !=
            RDNA4_ARGUMENT_SUMMARY_OK)
        return NO;
    RDNA4ArgumentSummaryAttempt *attempt =
        [[RDNA4ArgumentSummaryAttempt alloc]
            initWithTask:task device:RDNA4R.dev error:error];
    if (!attempt) return NO;
    rdna4_argument_summary_status status =
        rdna4_argument_summary_record(task, attempt, command);
    if (status != RDNA4_ARGUMENT_SUMMARY_OK) {
        if (error) *error = rdna4_argument_summary_runtime_error(
            status, @"immutable summary plan failed during recording");
        return NO;
    }
    status =
        rdna4_argument_summary_completion_finish_encode(
            &attempt->_completion, true);
    if (status != RDNA4_ARGUMENT_SUMMARY_OK) {
        if (error) *error = rdna4_argument_summary_runtime_error(
            status, @"summary encode did not complete every job");
        return NO;
    }
    *outAttempt = attempt;
    return YES;
}

GARNET_INTERNAL rdna4_argument_summary_status
rdna4_argument_summary_attempt_complete(
    RDNA4ArgumentSummaryAttempt *attempt, BOOL fenceSucceeded,
    uint64_t currentGeneration) {
    if (!attempt) return RDNA4_ARGUMENT_SUMMARY_INVALID;
    return rdna4_argument_summary_completion_fence(
        &attempt->_completion, fenceSucceeded != NO, currentGeneration);
}

GARNET_INTERNAL rdna4_argument_summary_status
rdna4_argument_summary_attempt_publish(
    RDNA4ArgumentSummaryAttempt *attempt, uint64_t currentGeneration,
    rdna4_argument_summary_publication *publication) {
    if (!attempt) return RDNA4_ARGUMENT_SUMMARY_INVALID;
    rdna4_argument_summary_status status =
        rdna4_argument_summary_completion_publish(
        &attempt->_completion, &attempt->_task->_plan,
        currentGeneration, publication);
    if (status == RDNA4_ARGUMENT_SUMMARY_READY) {
        publication->source_offset = attempt->_task->_sourceOffset;
        publication->output_offset = attempt->_task->_outputOffset;
    }
    return status;
}

#endif

#endif
