#ifndef RDNA4_ARGUMENT_SUMMARY_H
#define RDNA4_ARGUMENT_SUMMARY_H

/*
 * Pure-C planning ABI for the Private live-token summary shader.
 *
 * The shader is partial 5.4 groundwork: it finds every byte-aligned occurrence
 * of a candidate token in a source chunk. It does not create a queue, submit
 * work, retire any owner, or mutate persistent owner state. Root/lifetime code
 * must reuse the existing segment/fence/checkpoint path, run all source chunks
 * and dictionary pages, and publish only a READY result.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RDNA4_ARGUMENT_SUMMARY_SCHEMA_VERSION 1u
#define RDNA4_ARGUMENT_SUMMARY_TOKEN_PREFIX 0x5244u
#define RDNA4_ARGUMENT_SUMMARY_TOKEN_PREFIX_SHIFT 16u
#define RDNA4_ARGUMENT_SUMMARY_OVERLAP_BYTES 7u
#define RDNA4_ARGUMENT_SUMMARY_WORKGROUP_SIZE 64u
#define RDNA4_ARGUMENT_SUMMARY_DEFAULT_CHUNK_BYTES (1024u * 1024u)
#define RDNA4_ARGUMENT_SUMMARY_DEFAULT_DICTIONARY_PAGE 1024u

/*
 * The shader addresses bytes inside one chunk with uint fields. Leave room for
 * the seven-byte read overlap and round-up to a whole source word.
 */
#define RDNA4_ARGUMENT_SUMMARY_MAX_CHUNK_BYTES \
    (UINT32_MAX - RDNA4_ARGUMENT_SUMMARY_OVERLAP_BYTES - 3u)

enum {
    RDNA4_ARGUMENT_SUMMARY_BINDING_SOURCE = 0,
    RDNA4_ARGUMENT_SUMMARY_BINDING_DICTIONARY = 1,
    RDNA4_ARGUMENT_SUMMARY_BINDING_OUTPUT = 2,
};

/* Must match summary.comp's std430 push block exactly. */
typedef struct {
    /* Byte start relative to binding 0 (used by >65535-group partitioning). */
    uint32_t source_begin;
    uint32_t source_count;
    /* Readable bytes in binding 0, including up to seven overlap bytes. */
    uint32_t source_end;
    /* Global index of binding 1's first token, a multiple of 32. */
    uint32_t dictionary_begin;
    /* Page-local token count. Binding 1's search indices are always 0..count. */
    uint32_t dictionary_count;
    uint32_t dictionary_page;
    uint32_t dictionary_page_count;
    /* First global output word represented by page-local binding 2. */
    uint32_t output_word_base;
} rdna4_argument_summary_push;

/* Dictionary entry ABI: sorted by high word first, then low word. */
typedef struct {
    uint32_t lo;
    uint32_t hi;
} rdna4_argument_summary_candidate;

typedef enum {
    RDNA4_ARGUMENT_SUMMARY_OK = 0,
    RDNA4_ARGUMENT_SUMMARY_INVALID,
    RDNA4_ARGUMENT_SUMMARY_OVERFLOW,
    RDNA4_ARGUMENT_SUMMARY_INCOMPLETE,
    RDNA4_ARGUMENT_SUMMARY_STALE,
    RDNA4_ARGUMENT_SUMMARY_FAILED,
    RDNA4_ARGUMENT_SUMMARY_OBSOLETE,
    RDNA4_ARGUMENT_SUMMARY_COMPLETE,
    RDNA4_ARGUMENT_SUMMARY_READY,
} rdna4_argument_summary_status;

typedef enum {
    RDNA4_ARGUMENT_SUMMARY_PLAN_ACTIVE = 0,
    RDNA4_ARGUMENT_SUMMARY_PLAN_FAILED,
    RDNA4_ARGUMENT_SUMMARY_PLAN_OBSOLETE,
} rdna4_argument_summary_plan_state;

typedef struct {
    uint64_t generation;
    uint64_t source_size;
    uint64_t chunk_count;
    uint64_t job_count;
    uint32_t chunk_bytes;
    uint32_t candidate_count;
    uint32_t dictionary_page_size;
    uint32_t dictionary_page_count;
    rdna4_argument_summary_plan_state state;
} rdna4_argument_summary_plan;

typedef struct {
    uint64_t generation;
    uint64_t job_index;
    uint64_t chunk_index;
    uint64_t source_offset;
    /* Word-rounded readable binding size; the caller must provide that backing. */
    uint32_t source_binding_bytes;
    rdna4_argument_summary_push push;
} rdna4_argument_summary_job;

/*
 * READY is the only authorization to replace a stored owner set. This ticket
 * describes a complete result; it does not retain or release any resource.
 */
typedef struct {
    uint64_t generation;
    uint64_t source_size;
    uint64_t completed_jobs;
    uint64_t source_offset;
    uint64_t output_offset;
    uint32_t candidate_count;
    uint32_t output_word_count;
    uint32_t output_bytes;
} rdna4_argument_summary_publication;

typedef char rdna4_argument_summary_push_must_be_32_bytes[
    sizeof(rdna4_argument_summary_push) == 32u ? 1 : -1];

static inline bool rdna4_argument_summary_candidate_valid(
    rdna4_argument_summary_candidate token) {
    return (token.hi >> RDNA4_ARGUMENT_SUMMARY_TOKEN_PREFIX_SHIFT) ==
           RDNA4_ARGUMENT_SUMMARY_TOKEN_PREFIX;
}

static inline int rdna4_argument_summary_candidate_compare(
    rdna4_argument_summary_candidate a,
    rdna4_argument_summary_candidate b) {
    if (a.hi != b.hi) return a.hi < b.hi ? -1 : 1;
    if (a.lo != b.lo) return a.lo < b.lo ? -1 : 1;
    return 0;
}

static inline uint32_t rdna4_argument_summary_output_words(
    uint32_t candidate_count) {
    return (uint32_t)(((uint64_t)candidate_count + 31u) / 32u);
}

static inline bool rdna4_argument_summary_job_bitmap_words(
    uint64_t job_count, size_t *word_count) {
    if (!word_count) return false;
    uint64_t full_words = job_count / 32u;
    size_t words = (size_t)full_words;
    if ((uint64_t)words != full_words) return false;
    if (job_count % 32u) {
        if (words == SIZE_MAX) return false;
        ++words;
    }
    *word_count = words;
    return true;
}

static inline rdna4_argument_summary_status
rdna4_argument_summary_plan_init(
    rdna4_argument_summary_plan *plan, uint64_t source_size,
    uint32_t chunk_bytes, uint32_t candidate_count,
    uint32_t dictionary_page_size, uint64_t generation) {
    if (!plan || !chunk_bytes || (chunk_bytes & 3u) ||
        chunk_bytes > RDNA4_ARGUMENT_SUMMARY_MAX_CHUNK_BYTES ||
        !dictionary_page_size || (dictionary_page_size & 31u))
        return RDNA4_ARGUMENT_SUMMARY_INVALID;

    uint64_t chunk_count =
        source_size ? 1u + (source_size - 1u) / chunk_bytes : 0u;
    uint64_t page_count64 =
        candidate_count ? 1u + ((uint64_t)candidate_count - 1u) /
                                   dictionary_page_size
                        : 0u;
    if (page_count64 > UINT32_MAX)
        return RDNA4_ARGUMENT_SUMMARY_OVERFLOW;
    uint64_t job_count = 0;
    if (chunk_count && page_count64) {
        if (page_count64 > UINT64_MAX / chunk_count)
            return RDNA4_ARGUMENT_SUMMARY_OVERFLOW;
        job_count = chunk_count * page_count64;
    }

    *plan = (rdna4_argument_summary_plan){
        .generation = generation,
        .source_size = source_size,
        .chunk_count = chunk_count,
        .job_count = job_count,
        .chunk_bytes = chunk_bytes,
        .candidate_count = candidate_count,
        .dictionary_page_size = dictionary_page_size,
        .dictionary_page_count = (uint32_t)page_count64,
        .state = RDNA4_ARGUMENT_SUMMARY_PLAN_ACTIVE,
    };
    return RDNA4_ARGUMENT_SUMMARY_OK;
}

/*
 * Job order is source chunk outer, dictionary page inner. Every byte start is
 * covered exactly once. Non-final chunks advertise seven readable overlap
 * bytes after the start range so a token crossing the boundary is not lost.
 */
static inline rdna4_argument_summary_status
rdna4_argument_summary_plan_job(
    const rdna4_argument_summary_plan *plan, uint64_t job_index,
    rdna4_argument_summary_job *job) {
    if (!plan || !job || !plan->chunk_count ||
        !plan->dictionary_page_count || job_index >= plan->job_count)
        return RDNA4_ARGUMENT_SUMMARY_INVALID;

    uint32_t page_index = (uint32_t)(job_index %
        plan->dictionary_page_count);
    uint64_t chunk_index = job_index / plan->dictionary_page_count;
    if (chunk_index >= plan->chunk_count ||
        chunk_index > UINT64_MAX / plan->chunk_bytes)
        return RDNA4_ARGUMENT_SUMMARY_OVERFLOW;
    uint64_t source_offset = chunk_index * plan->chunk_bytes;
    if (source_offset >= plan->source_size)
        return RDNA4_ARGUMENT_SUMMARY_INVALID;

    uint64_t remaining = plan->source_size - source_offset;
    uint64_t source_count = remaining < plan->chunk_bytes ?
        remaining : plan->chunk_bytes;
    uint64_t after_start_range = remaining - source_count;
    uint64_t overlap = after_start_range <
            RDNA4_ARGUMENT_SUMMARY_OVERLAP_BYTES ?
        after_start_range : RDNA4_ARGUMENT_SUMMARY_OVERLAP_BYTES;
    uint64_t source_end = source_count + overlap;
    uint64_t binding_bytes = (source_end + 3u) & ~UINT64_C(3);

    uint64_t dictionary_begin =
        (uint64_t)page_index * plan->dictionary_page_size;
    if (dictionary_begin >= plan->candidate_count)
        return RDNA4_ARGUMENT_SUMMARY_INVALID;
    uint64_t dictionary_remaining =
        (uint64_t)plan->candidate_count - dictionary_begin;
    uint64_t dictionary_count =
        dictionary_remaining < plan->dictionary_page_size ?
        dictionary_remaining : plan->dictionary_page_size;
    if (source_count > UINT32_MAX || source_end > UINT32_MAX ||
        binding_bytes > UINT32_MAX)
        return RDNA4_ARGUMENT_SUMMARY_OVERFLOW;

    *job = (rdna4_argument_summary_job){
        .generation = plan->generation,
        .job_index = job_index,
        .chunk_index = chunk_index,
        .source_offset = source_offset,
        .source_binding_bytes = (uint32_t)binding_bytes,
        .push = {
            .source_begin = 0,
            .source_count = (uint32_t)source_count,
            .source_end = (uint32_t)source_end,
            .dictionary_begin = (uint32_t)dictionary_begin,
            .dictionary_count = (uint32_t)dictionary_count,
            .dictionary_page = page_index,
            .dictionary_page_count = plan->dictionary_page_count,
            .output_word_base = (uint32_t)(dictionary_begin >> 5u),
        },
    };
    return RDNA4_ARGUMENT_SUMMARY_OK;
}

/*
 * The completion bitmap is caller-owned and serialized by the lifetime owner.
 * A stale completion invalidates this plan and never marks new work complete.
 */
static inline rdna4_argument_summary_status
rdna4_argument_summary_plan_complete(
    rdna4_argument_summary_plan *plan,
    const rdna4_argument_summary_job *job, uint64_t generation,
    uint32_t *completed_words, size_t word_count) {
    if (!plan || !job || !completed_words ||
        job->job_index >= plan->job_count)
        return RDNA4_ARGUMENT_SUMMARY_INVALID;
    if (plan->state == RDNA4_ARGUMENT_SUMMARY_PLAN_FAILED)
        return RDNA4_ARGUMENT_SUMMARY_FAILED;
    if (plan->state == RDNA4_ARGUMENT_SUMMARY_PLAN_OBSOLETE)
        return RDNA4_ARGUMENT_SUMMARY_OBSOLETE;
    if (generation != plan->generation || job->generation != plan->generation) {
        plan->state = RDNA4_ARGUMENT_SUMMARY_PLAN_OBSOLETE;
        return RDNA4_ARGUMENT_SUMMARY_STALE;
    }

    size_t needed = 0;
    if (!rdna4_argument_summary_job_bitmap_words(plan->job_count, &needed))
        return RDNA4_ARGUMENT_SUMMARY_OVERFLOW;
    if (word_count < needed)
        return RDNA4_ARGUMENT_SUMMARY_INVALID;
    size_t word = (size_t)(job->job_index / 32u);
    completed_words[word] |= UINT32_C(1) << (job->job_index % 32u);
    return RDNA4_ARGUMENT_SUMMARY_OK;
}

static inline rdna4_argument_summary_status
rdna4_argument_summary_plan_fail(
    rdna4_argument_summary_plan *plan, uint64_t generation) {
    if (!plan) return RDNA4_ARGUMENT_SUMMARY_INVALID;
    if (generation != plan->generation) {
        plan->state = RDNA4_ARGUMENT_SUMMARY_PLAN_OBSOLETE;
        return RDNA4_ARGUMENT_SUMMARY_STALE;
    }
    plan->state = RDNA4_ARGUMENT_SUMMARY_PLAN_FAILED;
    return RDNA4_ARGUMENT_SUMMARY_FAILED;
}

static inline rdna4_argument_summary_status
rdna4_argument_summary_plan_obsolete(rdna4_argument_summary_plan *plan) {
    if (!plan) return RDNA4_ARGUMENT_SUMMARY_INVALID;
    plan->state = RDNA4_ARGUMENT_SUMMARY_PLAN_OBSOLETE;
    return RDNA4_ARGUMENT_SUMMARY_OBSOLETE;
}

/*
 * Returns READY only when every chunk/page job is complete and the live
 * generation still matches the generation captured at plan creation. All other
 * results are non-publishable and leave existing owners untouched.
 */
static inline rdna4_argument_summary_status
rdna4_argument_summary_plan_publish(
    rdna4_argument_summary_plan *plan, uint64_t current_generation,
    const uint32_t *completed_words, size_t word_count,
    rdna4_argument_summary_publication *publication) {
    if (!plan || !publication) return RDNA4_ARGUMENT_SUMMARY_INVALID;
    *publication = (rdna4_argument_summary_publication){0};
    if (plan->state == RDNA4_ARGUMENT_SUMMARY_PLAN_FAILED)
        return RDNA4_ARGUMENT_SUMMARY_FAILED;
    if (plan->state == RDNA4_ARGUMENT_SUMMARY_PLAN_OBSOLETE)
        return RDNA4_ARGUMENT_SUMMARY_OBSOLETE;
    if (current_generation != plan->generation) {
        plan->state = RDNA4_ARGUMENT_SUMMARY_PLAN_OBSOLETE;
        return RDNA4_ARGUMENT_SUMMARY_STALE;
    }

    size_t needed = 0;
    if (!rdna4_argument_summary_job_bitmap_words(plan->job_count, &needed))
        return RDNA4_ARGUMENT_SUMMARY_OVERFLOW;
    if (needed && !completed_words)
        return RDNA4_ARGUMENT_SUMMARY_INVALID;
    if (word_count < needed)
        return RDNA4_ARGUMENT_SUMMARY_INVALID;
    for (size_t word = 0; word < needed; ++word) {
        uint64_t first_job = (uint64_t)word * 32u;
        uint64_t jobs_left = plan->job_count - first_job;
        uint32_t mask = jobs_left >= 32u ? UINT32_MAX :
            (UINT32_C(1) << jobs_left) - 1u;
        if ((completed_words[word] & mask) != mask)
            return RDNA4_ARGUMENT_SUMMARY_INCOMPLETE;
    }

    *publication = (rdna4_argument_summary_publication){
        .generation = plan->generation,
        .source_size = plan->source_size,
        .completed_jobs = plan->job_count,
        .candidate_count = plan->candidate_count,
        .output_word_count =
            rdna4_argument_summary_output_words(plan->candidate_count),
    };
    return RDNA4_ARGUMENT_SUMMARY_READY;
}

#endif
