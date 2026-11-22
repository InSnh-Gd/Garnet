/*
 * Private-buffer sampler-owner publication for the argument-summary GPU pass.
 *
 * Include after rdna4_argument.inc and rdna4_argument_summary.inc. Encoding
 * snapshots one complete buffer's current owner candidates, but publication is
 * ordered by the command segment's accepted submission epoch, never by the
 * encode-time order. The command buffer keeps the returned attempt through its
 * existing fence; this file creates no queue, submit, or wait.
 */

#ifndef RDNA4_ARGUMENT_SUMMARY_OWNERS_INC
#define RDNA4_ARGUMENT_SUMMARY_OWNERS_INC

#include "rdna4_argument_summary.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct {
    uint64_t next_capture_serial;
    uint64_t published_epoch;
} rdna4_argument_summary_owner_state;

static inline rdna4_argument_summary_status
rdna4_argument_summary_owner_next_capture(
    rdna4_argument_summary_owner_state *state, uint64_t *capture_serial) {
    if (!state || !capture_serial ||
        state->next_capture_serial == UINT64_MAX)
        return RDNA4_ARGUMENT_SUMMARY_OVERFLOW;
    *capture_serial = ++state->next_capture_serial;
    return RDNA4_ARGUMENT_SUMMARY_OK;
}

/*
 * A segment epoch is assigned only after its submission is accepted. A later
 * completed writer invalidates an older summary even when the older callback
 * runs last. Encoded-only writers do not advance last_completed_epoch.
 */
static inline bool rdna4_argument_summary_owner_publish_admissible(
    uint64_t accepted_epoch, uint64_t latest_summary_epoch,
    uint64_t last_completed_epoch, uint64_t capture_host_generation,
    uint64_t current_host_generation, bool host_generation_saturated) {
    return accepted_epoch && accepted_epoch != UINT64_MAX &&
        accepted_epoch > latest_summary_epoch &&
        last_completed_epoch <= accepted_epoch &&
        capture_host_generation != UINT64_MAX &&
        !host_generation_saturated &&
        capture_host_generation == current_host_generation;
}

static inline bool rdna4_argument_summary_owner_completed_prunable(
    uint64_t write_epoch, uint64_t accepted_epoch,
    uint64_t last_completed_epoch, bool all_candidates_captured,
    bool ordered_before_summary) {
    return all_candidates_captured && ordered_before_summary && write_epoch &&
        write_epoch <= accepted_epoch &&
        last_completed_epoch <= accepted_epoch;
}

/*
 * Capture-time completion already happened before the summary was encoded.
 * A writer that was still active at capture is also ordered before this
 * summary only when it belongs to this segment and carries the segment's
 * accepted submission epoch; encode order alone proves nothing across queues.
 */
static inline bool rdna4_argument_summary_owner_ordered_before_summary(
    bool completed_at_capture, bool in_segment,
    uint64_t write_epoch, uint64_t summary_epoch) {
    return completed_at_capture ||
        (in_segment && write_epoch && write_epoch == summary_epoch);
}

/*
 * A stale/obsolete generation is a normal no-publish result and leaves the
 * previous owners untouched for a later summary. Every other unexpected
 * completion/publish status is a bounded failure; overflow, invalid metadata
 * and incomplete jobs must never be silently treated as optional work.
 */
static inline bool rdna4_argument_summary_owner_status_needs_error(
    rdna4_argument_summary_status status,
    rdna4_argument_summary_status expected) {
    return status != expected &&
        status != RDNA4_ARGUMENT_SUMMARY_STALE &&
        status != RDNA4_ARGUMENT_SUMMARY_OBSOLETE;
}

static inline size_t rdna4_argument_summary_owner_candidate_index(
    const uint64_t *tokens, size_t count, uint64_t token) {
    size_t begin = 0, end = count;
    while (begin < end) {
        size_t middle = begin + (end - begin) / 2u;
        if (tokens[middle] < token)
            begin = middle + 1u;
        else
            end = middle;
    }
    return begin;
}

static inline bool rdna4_argument_summary_owner_candidate_found(
    const uint32_t *output_words, size_t output_word_count,
    size_t candidate_index) {
    if (!output_words) return false;
    size_t word = candidate_index / 32u;
    return word < output_word_count &&
        (output_words[word] & (UINT32_C(1) << (candidate_index % 32u))) != 0;
}

static inline bool rdna4_argument_summary_owner_token_found(
    const uint64_t *tokens, size_t candidate_count,
    const uint32_t *output_words, size_t output_word_count,
    uint64_t token) {
    if (!tokens) return false;
    size_t index = rdna4_argument_summary_owner_candidate_index(
        tokens, candidate_count, token);
    return index < candidate_count && tokens[index] == token &&
        rdna4_argument_summary_owner_candidate_found(
            output_words, output_word_count, index);
}

static inline bool rdna4_argument_summary_owner_token_captured(
    const uint64_t *tokens, size_t candidate_count, uint64_t token) {
    if (!tokens || !token) return false;
    size_t index = rdna4_argument_summary_owner_candidate_index(
        tokens, candidate_count, token);
    return index < candidate_count && tokens[index] == token;
}

static inline bool rdna4_argument_summary_owner_exact_range_retained(
    bool token_captured) {
    /*
     * The GPU result has no offsets. A captured candidate's old slot or range
     * cannot prove where its found token now lives; only uncaptured owners are
     * exact enough to retain.
     */
    return !token_captured;
}

/*
 * A Private transfer by itself is not summary work. The GPU pass is useful
 * only when the transfer can add, remove or refine sampler ownership state.
 */
static inline bool rdna4_argument_summary_owner_state_needed(
    bool private_target, bool incoming_candidates, bool slot_candidates,
    bool possible_candidates, bool active_writer_candidates,
    bool completed_candidates) {
    return private_target &&
        (incoming_candidates || slot_candidates || possible_candidates ||
         active_writer_candidates || completed_candidates);
}

static inline bool rdna4_argument_summary_owner_ranges_overlap(
    uint64_t left_start, uint64_t left_length,
    uint64_t right_start, uint64_t right_length) {
    if (!left_length || !right_length) return false;
    return left_start <= right_start ?
        right_start - left_start < left_length :
        left_start - right_start < right_length;
}

#if !defined(RDNA4_ARGUMENT_SUMMARY_OWNERS_TEST)

@interface RDNA4ArgumentSummaryOwnerState : NSObject
@property uint64_t nextCaptureSerial;
@end
@implementation RDNA4ArgumentSummaryOwnerState
@end

static char rdna4_argument_summary_owner_state_key;
static RDNA4ArgumentSummaryOwnerState *
rdna4_argument_summary_owner_state_for_buffer(id buffer, BOOL create) {
    RDNA4ArgumentSummaryOwnerState *state =
        objc_getAssociatedObject(buffer, &rdna4_argument_summary_owner_state_key);
    if (!state && create) {
        state = [RDNA4ArgumentSummaryOwnerState new];
        objc_setAssociatedObject(buffer, &rdna4_argument_summary_owner_state_key,
                                 state, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }
    return state;
}

@interface RDNA4ArgumentSummaryOwnerCapture : NSObject {
@public
    RDNA4Buffer *buffer;
    RDNA4ArgumentSummaryTask *task;
    RDNA4ArgumentSummaryAttempt *attempt;
    NSArray<NSNumber *> *tokens;
    NSArray *tokenObjects;
    NSSet<NSNumber *> *completedSerials;
    NSSet<NSNumber *> *segmentSerials;
    uint64_t *tokenValues;
    size_t tokenCount;
    uint64_t captureSerial;
    uint64_t hostGeneration;
}
@end
@implementation RDNA4ArgumentSummaryOwnerCapture
- (void)dealloc {
    free(tokenValues);
}
@end

@interface RDNA4ArgumentSummaryOwnerAttempt : NSObject {
@public
    NSArray<RDNA4ArgumentSummaryOwnerCapture *> *captures;
    BOOL finished;
}
- (BOOL)rdna4FinishWithSubmissionEpoch:(uint64_t)submissionEpoch
                        fenceSucceeded:(BOOL)fenceSucceeded
                                 error:(NSError **)error;
@end

static NSError *rdna4_argument_summary_owner_error(
    NSString *message) {
    return rdna4_err(68, [NSString stringWithFormat:
        @"argument summary owner: %@", message]);
}

static NSError *rdna4_argument_summary_owner_status_error(
    NSString *phase, rdna4_argument_summary_status status) {
    return rdna4_argument_summary_owner_error(
        [NSString stringWithFormat:@"%@ returned status %d", phase,
                                   (int)status]);
}

static uint64_t rdna4_argument_summary_owner_token_for_object(
    id device, id object) {
    if (!object) return 0;
    id identity = rdna4_argument_token_identity(object, NO);
    if (!identity) return 0;
    rdna4_argument_tokens_init();
    uint64_t token = 0;
    [rdna4_argument_tokens_lock lock];
    rdna4_argument_token_prune_locked();
    for (NSNumber *key in rdna4_argument_tokens) {
        RDNA4ArgumentResourceToken *entry = rdna4_argument_tokens[key];
        if (entry.texture) continue;
        if (entry.identity == identity && entry.device == device) {
            token = key.unsignedLongLongValue;
            break;
        }
    }
    [rdna4_argument_tokens_lock unlock];
    return token;
}

static BOOL rdna4_argument_summary_owner_add_candidate(
    NSMutableDictionary<NSNumber *, id> *candidates, uint64_t token,
    id object) {
    if (!object || (token >> 48u) != RDNA4_ARGUMENT_SUMMARY_TOKEN_PREFIX)
        return NO;
    NSNumber *key = @(token);
    id previous = candidates[key];
    if (!previous) candidates[key] = object;
    return YES;
}

/*
 * Read one intent and the persistent owners under the destination buffer's
 * lock. Only candidate-bearing state that can affect this intent's envelope is
 * relevant; unrelated transfer records do not turn ordinary buffer traffic
 * into summary work.
 */
static BOOL rdna4_argument_summary_owner_single_intent_needed(
    RDNA4ArgumentWriteIntent *intent) {
    RDNA4Buffer *buffer = intent ? intent->buffer : nil;
    if (!buffer || [buffer storageMode] != MTLStorageModePrivate) return NO;
    BOOL needed = NO;
    @synchronized (buffer) {
        RDNA4ArgumentSamplerOwners *owners =
            rdna4_argument_sampler_owners(buffer, NO);
        if (!owners) return NO;
        bool incoming = intent->candidates.count != 0;
        bool slots = false, possible = false;
        bool writers = false, completed = false;
        NSRange range = intent->range;
        for (NSNumber *key in owners.slots) {
            uint64_t offset = key.unsignedLongLongValue;
            if (rdna4_argument_summary_owner_ranges_overlap(
                    offset, sizeof(uint64_t), range.location, range.length)) {
                slots = true;
                break;
            }
        }
        for (NSValue *key in owners.possible) {
            NSRange candidateRange = key.rangeValue;
            if ([owners.possible[key] count] &&
                rdna4_argument_summary_owner_ranges_overlap(
                    candidateRange.location, candidateRange.length,
                    range.location, range.length)) {
                possible = true;
                break;
            }
        }
        for (RDNA4ArgumentWriteIntent *writer in owners.writers) {
            if (!writer->active || !writer->candidates.count ||
                !rdna4_argument_summary_owner_ranges_overlap(
                    writer->range.location, writer->range.length,
                    range.location, range.length))
                continue;
            writers = true;
            break;
        }
        for (RDNA4ArgumentCompletedWrite *write in owners.completed) {
            if (!write->candidates.count ||
                !rdna4_argument_summary_owner_ranges_overlap(
                    write->range.location, write->range.length,
                    range.location, range.length))
                continue;
            completed = true;
            break;
        }
        needed = rdna4_argument_summary_owner_state_needed(
            true, incoming, slots, possible, writers, completed);
    }
    return needed;
}

static BOOL rdna4_argument_summary_owners_needed(NSArray *intents) {
    if (![intents isKindOfClass:[NSArray class]]) return NO;
    for (id token in intents) {
        if (![token isKindOfClass:
                RDNA4ArgumentWriteIntent.class])
            continue;
        if (rdna4_argument_summary_owner_single_intent_needed(token))
            return YES;
    }
    return NO;
}

/*
 * Called with the buffer lock held. The dictionary covers the complete
 * persistent buffer, not only the ranges selected by this dispatch.
 */
static BOOL rdna4_argument_summary_owner_snapshot_locked(
    RDNA4Buffer *buffer,
    NSArray<NSNumber *> * __strong *outTokens,
    NSArray * __strong *outObjects,
    NSSet<NSNumber *> * __strong *outCompletedSerials,
    uint64_t *outHostGeneration, uint64_t *outCaptureSerial,
    NSError **error) {
    RDNA4ArgumentSamplerOwners *owners =
        rdna4_argument_sampler_owners(buffer, NO);
    if (!owners) {
        if (error) *error = rdna4_argument_summary_owner_error(
            @"write intent has no sampler-owner state");
        return NO;
    }
    if (owners.hostGenerationSaturated ||
        owners.hostGeneration == UINT64_MAX) {
        if (error) *error = rdna4_argument_summary_owner_error(
            @"host generation is saturated");
        return NO;
    }
    RDNA4ArgumentSummaryOwnerState *state =
        rdna4_argument_summary_owner_state_for_buffer(buffer, YES);
    rdna4_argument_summary_owner_state pure = {
        .next_capture_serial = state.nextCaptureSerial,
        .published_epoch = owners.latestSummaryEpoch,
    };
    uint64_t serial = 0;
    if (rdna4_argument_summary_owner_next_capture(
            &pure, &serial) != RDNA4_ARGUMENT_SUMMARY_OK) {
        if (error) *error = rdna4_argument_summary_owner_error(
            @"capture serial is exhausted");
        return NO;
    }

    NSMutableDictionary<NSNumber *, id> *candidates =
        [NSMutableDictionary dictionary];
    for (NSNumber *key in owners.slots) {
        NSArray *slot = owners.slots[key];
        if (slot.count != 2 ||
            !rdna4_argument_summary_owner_add_candidate(
                candidates, [slot[0] unsignedLongLongValue], slot[1])) {
            if (error) *error = rdna4_argument_summary_owner_error(
                @"persistent slot has an invalid token or sampler");
            return NO;
        }
    }
    /*
     * Owner ranges carry sampler objects but not their tokens. Resolve the
     * stable token once, from the same identity registry used by the setters.
     */
    for (NSValue *key in owners.possible) {
        for (id object in owners.possible[key]) {
            uint64_t token = rdna4_argument_summary_owner_token_for_object(
                buffer.device, object);
            if (!token ||
                !rdna4_argument_summary_owner_add_candidate(
                    candidates, token, object)) {
                if (error) *error = rdna4_argument_summary_owner_error(
                    @"possible sampler has no stable token");
                return NO;
            }
        }
    }
    for (RDNA4ArgumentWriteIntent *intent in owners.writers) {
        if (!intent->active) continue;
        for (id object in intent->candidates) {
            uint64_t token = rdna4_argument_summary_owner_token_for_object(
                buffer.device, object);
            if (!token ||
                !rdna4_argument_summary_owner_add_candidate(
                    candidates, token, object)) {
                if (error) *error = rdna4_argument_summary_owner_error(
                    @"pending writer sampler has no stable token");
                return NO;
            }
        }
    }
    for (RDNA4ArgumentCompletedWrite *write in owners.completed) {
        for (id object in write->candidates) {
            uint64_t token = rdna4_argument_summary_owner_token_for_object(
                buffer.device, object);
            if (!token ||
                !rdna4_argument_summary_owner_add_candidate(
                    candidates, token, object)) {
                if (error) *error = rdna4_argument_summary_owner_error(
                    @"completed writer sampler has no stable token");
                return NO;
            }
        }
    }
    NSArray<NSNumber *> *ordered = [candidates.allKeys
        sortedArrayUsingComparator:^NSComparisonResult(NSNumber *left, NSNumber *right) {
            uint64_t a = left.unsignedLongLongValue;
            uint64_t b = right.unsignedLongLongValue;
            return a < b ? NSOrderedAscending :
                a > b ? NSOrderedDescending : NSOrderedSame;
        }];
    NSMutableArray *objects = [NSMutableArray arrayWithCapacity:ordered.count];
    for (NSNumber *key in ordered) [objects addObject:candidates[key]];
    NSMutableSet<NSNumber *> *completedSerials = [NSMutableSet set];
    for (RDNA4ArgumentCompletedWrite *write in owners.completed)
        [completedSerials addObject:@(write->serial)];

    state.nextCaptureSerial = pure.next_capture_serial;
    *outTokens = ordered;
    *outObjects = objects;
    *outCompletedSerials = completedSerials;
    *outHostGeneration = owners.hostGeneration;
    *outCaptureSerial = serial;
    return YES;
}

static RDNA4ArgumentSummaryOwnerCapture *
rdna4_argument_summary_owner_capture(
    RDNA4Buffer *buffer, NSSet<NSNumber *> *segmentSerials,
    VkCommandBuffer command, NSError **error) {
    NSArray<NSNumber *> *tokens = nil;
    NSArray *objects = nil;
    NSSet<NSNumber *> *completedSerials = nil;
    uint64_t hostGeneration = 0, captureSerial = 0;
    @synchronized (buffer) {
        if (!rdna4_argument_summary_owner_snapshot_locked(
                buffer, &tokens, &objects, &completedSerials,
                &hostGeneration, &captureSerial, error))
            return nil;
    }
    if (tokens.count > UINT32_MAX) {
        if (error) *error = rdna4_argument_summary_owner_error(
            @"candidate dictionary exceeds 32-bit indexing");
        return nil;
    }
    NSUInteger dictionaryBytes =
        tokens.count * sizeof(rdna4_argument_summary_candidate);
    if (tokens.count &&
        dictionaryBytes / sizeof(rdna4_argument_summary_candidate) !=
            tokens.count) {
        if (error) *error = rdna4_argument_summary_owner_error(
            @"candidate dictionary size overflows");
        return nil;
    }
    NSError *allocationError = nil;
    RDNA4Buffer *dictionary = nil;
    if (dictionaryBytes) {
        dictionary = [[RDNA4Buffer alloc]
            initWithDevice:buffer.device length:dictionaryBytes
                   options:MTLResourceStorageModeShared
                     error:&allocationError];
        if (!dictionary) {
            if (error) *error = allocationError ?:
                rdna4_argument_summary_owner_error(
                    @"candidate dictionary allocation failed");
            return nil;
        }
        rdna4_argument_summary_candidate *entries =
            (rdna4_argument_summary_candidate *)dictionary.contents;
        for (NSUInteger index = 0; index < tokens.count; ++index) {
            uint64_t token = tokens[index].unsignedLongLongValue;
            entries[index] = (rdna4_argument_summary_candidate){
                .lo = (uint32_t)token,
                .hi = (uint32_t)(token >> 32u),
            };
        }
    }
    uint32_t outputWords = rdna4_argument_summary_output_words(
        (uint32_t)tokens.count);
    NSUInteger outputBytes =
        outputWords ? (NSUInteger)outputWords * sizeof(uint32_t) :
        sizeof(uint32_t);
    RDNA4Buffer *output = [[RDNA4Buffer alloc]
        initWithDevice:buffer.device length:outputBytes
               options:MTLResourceStorageModeShared
                 error:&allocationError];
    if (!output) {
        if (error) *error = allocationError ?:
            rdna4_argument_summary_owner_error(
                @"summary output allocation failed");
        return nil;
    }
    RDNA4ArgumentSummaryTask *task = rdna4_argument_summary_task_new(
        buffer, 0, buffer.length, dictionary, 0,
        (uint32_t)tokens.count, output, 0, captureSerial, NULL, 0, 0,
        &allocationError);
    if (!task) {
        if (error) *error = allocationError ?:
            rdna4_argument_summary_owner_error(
                @"summary task creation failed");
        return nil;
    }
    RDNA4ArgumentSummaryAttempt *attempt = nil;
    if (!rdna4_argument_summary_encode(task, command, &attempt,
                                       &allocationError)) {
        if (error) *error = allocationError ?:
            rdna4_argument_summary_owner_error(
                @"summary recording failed; abort the segment");
        return nil;
    }
    RDNA4ArgumentSummaryOwnerCapture *capture =
        [RDNA4ArgumentSummaryOwnerCapture new];
    capture->tokenValues = calloc(tokens.count, sizeof(uint64_t));
    if (tokens.count && !capture->tokenValues) {
        if (error) *error = rdna4_argument_summary_owner_error(
            @"token index allocation failed");
        return nil;
    }
    for (NSUInteger index = 0; index < tokens.count; ++index)
        capture->tokenValues[index] = tokens[index].unsignedLongLongValue;
    capture->tokenCount = tokens.count;
    capture->buffer = buffer;
    capture->task = task;
    capture->attempt = attempt;
    capture->tokens = tokens;
    capture->tokenObjects = objects;
    capture->completedSerials = completedSerials;
    capture->segmentSerials = [segmentSerials copy];
    capture->captureSerial = captureSerial;
    capture->hostGeneration = hostGeneration;
    return capture;
}

static BOOL rdna4_argument_summary_owner_apply(
    RDNA4ArgumentSummaryOwnerCapture *capture, uint64_t submissionEpoch,
    NSError **error) {
    RDNA4Buffer *buffer = capture->buffer;
    @synchronized (buffer) {
        RDNA4ArgumentSamplerOwners *owners =
            rdna4_argument_sampler_owners(buffer, NO);
        if (!owners) return YES;
        if (!rdna4_argument_summary_owner_publish_admissible(
                submissionEpoch, owners.latestSummaryEpoch,
                owners.lastCompletedEpoch, capture->hostGeneration,
                owners.hostGeneration,
                owners.hostGenerationSaturated != NO))
            return YES;

        uint32_t wordCount = rdna4_argument_summary_output_words(
            (uint32_t)capture->tokens.count);
        NSUInteger outputBytes =
            (NSUInteger)wordCount * sizeof(uint32_t);
        RDNA4Buffer *summaryOutput = capture->task.outputBuffer;
        if (!summaryOutput ||
            outputBytes > [(id<MTLBuffer>)summaryOutput length] ||
            (outputBytes && !summaryOutput.contents)) {
            if (error) *error = rdna4_argument_summary_owner_error(
                @"summary output is unavailable");
            return NO;
        }
        const uint32_t *words = (const uint32_t *)summaryOutput.contents;
        NSArray<NSNumber *> *tokens = capture->tokens;
        NSArray *objects = capture->tokenObjects;
        NSMutableSet *foundObjects = [NSMutableSet set];
        for (NSUInteger index = 0; index < tokens.count; ++index) {
            if (rdna4_argument_summary_owner_candidate_found(
                    words, wordCount, index))
                [foundObjects addObject:objects[index]];
        }
        /*
         * The output is a whole-buffer membership bitset, not an offset map.
         * Every covered exact range/possible set therefore loses its old
         * placement; only found candidates survive and they are re-added as
         * whole-buffer possible. Owners absent from this capture are retained.
         */
        for (NSNumber *key in owners.slots.allKeys) {
            NSArray *slot = owners.slots[key];
            uint64_t token = slot.count ? [slot[0] unsignedLongLongValue] : 0;
            if (!slot.count ||
                !rdna4_argument_summary_owner_exact_range_retained(
                    rdna4_argument_summary_owner_token_captured(
                        capture->tokenValues, capture->tokenCount, token)))
                [owners.slots removeObjectForKey:key];
        }
        for (NSValue *key in owners.possible.allKeys) {
            NSMutableSet *retained = [NSMutableSet set];
            for (id object in owners.possible[key]) {
                uint64_t token =
                    rdna4_argument_summary_owner_token_for_object(
                        buffer.device, object);
                if (!token ||
                    rdna4_argument_summary_owner_exact_range_retained(
                        rdna4_argument_summary_owner_token_captured(
                            capture->tokenValues, capture->tokenCount, token)))
                    [retained addObject:object];
            }
            if (retained.count) {
                owners.possible[key] = retained;
            } else {
                [owners.possible removeObjectForKey:key];
            }
        }
        if (foundObjects.count)
            rdna4_argument_sampler_add_possible_locked(
                owners, NSMakeRange(0, buffer.length), foundObjects);

        if (owners.lastCompletedEpoch <= submissionEpoch) {
            NSMutableArray *drop = [NSMutableArray array];
            for (RDNA4ArgumentCompletedWrite *write in owners.completed) {
                BOOL allCaptured = YES;
                for (id object in write->candidates) {
                    uint64_t token =
                        rdna4_argument_summary_owner_token_for_object(
                            buffer.device, object);
                    if (!token ||
                        !rdna4_argument_summary_owner_token_captured(
                            capture->tokenValues, capture->tokenCount,
                            token)) {
                        allCaptured = NO;
                        break;
                    }
                }
                BOOL orderedBeforeSummary =
                    rdna4_argument_summary_owner_ordered_before_summary(
                        [capture->completedSerials containsObject:
                            @(write->serial)],
                        [capture->segmentSerials containsObject:
                            @(write->serial)],
                        write->submissionEpoch, submissionEpoch);
                if (rdna4_argument_summary_owner_completed_prunable(
                        write->submissionEpoch, submissionEpoch,
                        owners.lastCompletedEpoch, allCaptured,
                        orderedBeforeSummary))
                    [drop addObject:write];
            }
            [owners.completed removeObjectsInArray:drop];
        }
        owners.latestSummaryEpoch = submissionEpoch;
        return YES;
    }
}

@implementation RDNA4ArgumentSummaryOwnerAttempt
- (BOOL)rdna4FinishWithSubmissionEpoch:(uint64_t)submissionEpoch
                        fenceSucceeded:(BOOL)fenceSucceeded
                                 error:(NSError **)error {
    if (finished) return YES;
    finished = YES;
    if (!fenceSucceeded) {
        captures = @[];
        return YES;
    }
    BOOL succeeded = YES;
    for (RDNA4ArgumentSummaryOwnerCapture *capture in captures) {
        rdna4_argument_summary_status status =
            rdna4_argument_summary_attempt_complete(
                capture->attempt, YES, capture->captureSerial);
        if (status != RDNA4_ARGUMENT_SUMMARY_COMPLETE) {
            if (!rdna4_argument_summary_owner_status_needs_error(
                    status, RDNA4_ARGUMENT_SUMMARY_COMPLETE))
                continue;
            if (error) *error = rdna4_argument_summary_owner_status_error(
                @"summary completion", status);
            succeeded = NO;
            break;
        }
        rdna4_argument_summary_publication publication = {0};
        status = rdna4_argument_summary_attempt_publish(
            capture->attempt, capture->captureSerial, &publication);
        if (status != RDNA4_ARGUMENT_SUMMARY_READY) {
            if (!rdna4_argument_summary_owner_status_needs_error(
                    status, RDNA4_ARGUMENT_SUMMARY_READY))
                continue;
            if (error) *error = rdna4_argument_summary_owner_status_error(
                @"summary publication", status);
            succeeded = NO;
            break;
        }
        if (!rdna4_argument_summary_owner_apply(
                capture, submissionEpoch, error)) {
            succeeded = NO;
            break;
        }
    }
    /*
     * Finish is the final reader of every capture. Releasing the complete
     * dictionary/task/output graph here avoids extending its lifetime through
     * an autoreleased owner-attempt wrapper after the segment clears its strong
     * attempt array.
     */
    captures = @[];
    return succeeded;
}
@end

static BOOL rdna4_argument_summary_owners_finish(
    id attempt, BOOL fenceSucceeded, uint64_t submissionEpoch,
    NSError **error) {
    if (!attempt) return YES;
    if (![attempt isKindOfClass:
            RDNA4ArgumentSummaryOwnerAttempt.class]) {
        if (error) *error = rdna4_argument_summary_owner_error(
            @"finish received a non-summary owner attempt");
        return NO;
    }
    return [(RDNA4ArgumentSummaryOwnerAttempt *)attempt
        rdna4FinishWithSubmissionEpoch:submissionEpoch
                        fenceSucceeded:fenceSucceeded
                                 error:error];
}

static BOOL rdna4_argument_summary_owners_encode(
    NSArray<RDNA4ArgumentWriteIntent *> *intents,
    VkCommandBuffer command,
    NSArray * __strong *outAttempts,
    NSError **error) {
    if (outAttempts) *outAttempts = @[];
    if (!command) {
        if (error) *error = rdna4_argument_summary_owner_error(
            @"encode needs a command buffer");
        return NO;
    }
    NSMutableArray *attempts =
        [NSMutableArray array];
    NSMutableArray<RDNA4ArgumentSummaryOwnerCapture *> *created =
        [NSMutableArray array];
    NSMutableArray<RDNA4Buffer *> *buffers = [NSMutableArray array];
    NSMutableArray<NSMutableArray<RDNA4ArgumentWriteIntent *> *> *groups =
        [NSMutableArray array];
    for (RDNA4ArgumentWriteIntent *intent in intents) {
        RDNA4Buffer *buffer = intent ? intent->buffer : nil;
        if (!buffer) {
            if (error) *error = rdna4_argument_summary_owner_error(
                @"write intent lost its destination buffer");
            return NO;
        }
        if (!intent->serial) {
            if (error) *error = rdna4_argument_summary_owner_error(
                @"write intent has no serial");
            return NO;
        }
        if ([buffer storageMode] != MTLStorageModePrivate) continue;
        NSUInteger index = [buffers indexOfObjectIdenticalTo:buffer];
        if (index == NSNotFound) {
            [buffers addObject:buffer];
            [groups addObject:[NSMutableArray array]];
            index = buffers.count - 1u;
        }
        [groups[index] addObject:intent];
    }
    for (NSUInteger index = 0; index < buffers.count; ++index) {
        RDNA4Buffer *buffer = buffers[index];
        NSArray<RDNA4ArgumentWriteIntent *> *group = groups[index];
        if (!rdna4_argument_summary_owners_needed(group)) continue;
        NSMutableSet<NSNumber *> *segmentSerials = [NSMutableSet set];
        for (RDNA4ArgumentWriteIntent *intent in group)
            [segmentSerials addObject:@(intent->serial)];
        RDNA4ArgumentSummaryOwnerCapture *capture =
            rdna4_argument_summary_owner_capture(
                buffer, segmentSerials, command, error);
        if (!capture) {
            /*
             * Some summary commands may already be recorded. The caller must
             * abort this segment; no owner is published before its fence.
             */
            return NO;
        }
        [created addObject:capture];
    }
    if (!created.count) return YES;
    for (RDNA4ArgumentSummaryOwnerCapture *capture in created) {
        RDNA4ArgumentSummaryOwnerAttempt *attempt =
            [RDNA4ArgumentSummaryOwnerAttempt new];
        attempt->captures = @[capture];
        [attempts addObject:attempt];
    }
    if (outAttempts) *outAttempts = attempts;
    return YES;
}

#endif

#endif
