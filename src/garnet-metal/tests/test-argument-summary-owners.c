/*
 * Pure ordering checks for rdna4_argument_summary_owners.inc.
 *
 * The accepted submission epoch, not encode order or capture serial, orders
 * summary publication. The ObjC bridge is compiled and exercised separately
 * with the production driver.
 */

#define RDNA4_ARGUMENT_SUMMARY_OWNERS_TEST 1
#include "garnet_argument_summary_owners.m"

#include <stdio.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "ARGUMENT_SUMMARY_OWNERS_FAIL %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
    ++checks; \
} while (0)

static unsigned checks;

static int test_capture_serial_saturation(void) {
    rdna4_argument_summary_owner_state state = {0};
    uint64_t serial = 0;
    CHECK(rdna4_argument_summary_owner_next_capture(
              &state, &serial) == RDNA4_ARGUMENT_SUMMARY_OK);
    CHECK(serial == 1 && state.next_capture_serial == 1);
    CHECK(rdna4_argument_summary_owner_next_capture(
              &state, &serial) == RDNA4_ARGUMENT_SUMMARY_OK);
    CHECK(serial == 2 && state.next_capture_serial == 2);
    state.next_capture_serial = UINT64_MAX;
    CHECK(rdna4_argument_summary_owner_next_capture(
              &state, &serial) == RDNA4_ARGUMENT_SUMMARY_OVERFLOW);
    CHECK(rdna4_argument_summary_owner_next_capture(
              NULL, &serial) == RDNA4_ARGUMENT_SUMMARY_OVERFLOW);
    return 0;
}

static int test_reverse_queue_acceptance_order(void) {
    /*
     * Queue 2's writer was encoded first but accepted as epoch 20. Queue 1's
     * summary was encoded second but accepted as epoch 10. The older summary
     * callback must not erase the later accepted writer's completed owner.
     */
    const uint64_t summary_epoch = 10;
    const uint64_t writer_epoch = 20;
    CHECK(!rdna4_argument_summary_owner_publish_admissible(
        summary_epoch, 0, writer_epoch, 7, 7, false));
    CHECK(!rdna4_argument_summary_owner_publish_admissible(
        summary_epoch, 0, 0, 7, 8, false));
    CHECK(!rdna4_argument_summary_owner_publish_admissible(
        summary_epoch, 0, 0, 7, 7, true));
    CHECK(!rdna4_argument_summary_owner_publish_admissible(
        summary_epoch, summary_epoch, 0, 7, 7, false));
    CHECK(!rdna4_argument_summary_owner_publish_admissible(
        0, 0, 0, 7, 7, false));
    CHECK(!rdna4_argument_summary_owner_publish_admissible(
        UINT64_MAX, 0, 0, 7, 7, false));
    CHECK(!rdna4_argument_summary_owner_publish_admissible(
        10, 0, 0, UINT64_MAX, UINT64_MAX, false));

    /* An encoded-only later writer never changes last_completed_epoch. */
    CHECK(rdna4_argument_summary_owner_publish_admissible(
        10, 0, 0, 7, 7, false));
    /* A later completed writer does publish once its own epoch is used. */
    CHECK(rdna4_argument_summary_owner_publish_admissible(
        writer_epoch, 10, writer_epoch, 7, 7, false));
    /* A delayed older callback cannot overwrite it. */
    CHECK(!rdna4_argument_summary_owner_publish_admissible(
        summary_epoch, writer_epoch, writer_epoch, 7, 7, false));
    return 0;
}

static int test_completed_pruning_epochs(void) {
    CHECK(rdna4_argument_summary_owner_completed_prunable(
        5, 10, 10, true, true));
    CHECK(rdna4_argument_summary_owner_completed_prunable(
        10, 10, 10, true, true));
    /*
     * The summary is recorded after every intent in its segment, so a
     * same-segment completed writer at the same accepted epoch is ordered
     * before the summary and can be pruned.
     */
    CHECK(rdna4_argument_summary_owner_completed_prunable(
        2, 2, 2, true, true));
    CHECK(!rdna4_argument_summary_owner_completed_prunable(
        11, 10, 20, true, true));
    CHECK(!rdna4_argument_summary_owner_completed_prunable(
        5, 10, 20, true, true));
    CHECK(!rdna4_argument_summary_owner_completed_prunable(
        0, 10, 10, true, true));
    /*
     * A post-capture writer accepted/completed before this summary but absent
     * from its candidate dictionary must retain its completed record.
     */
    CHECK(!rdna4_argument_summary_owner_completed_prunable(
        1, 2, 1, false, true));
    /* Even a fully captured token from a record completed after capture stays. */
    CHECK(!rdna4_argument_summary_owner_completed_prunable(
        1, 2, 1, true, false));
    /* Another queue's post-capture completion has no segment-order proof. */
    CHECK(!rdna4_argument_summary_owner_completed_prunable(
        2, 2, 2, true, false));
    return 0;
}

static int test_summary_order_proof(void) {
    CHECK(rdna4_argument_summary_owner_ordered_before_summary(
        true, false, 0, 2));
    /*
     * A writer still active at capture becomes completed only after the
     * summary was encoded. This segment's serial plus the shared accepted
     * epoch proves command order without using encode/capture serials.
     */
    CHECK(rdna4_argument_summary_owner_ordered_before_summary(
        false, true, 2, 2));
    CHECK(!rdna4_argument_summary_owner_ordered_before_summary(
        false, true, 1, 2));
    CHECK(!rdna4_argument_summary_owner_ordered_before_summary(
        false, true, 0, 2));
    /* Another queue's post-capture completion has no segment-order proof. */
    CHECK(!rdna4_argument_summary_owner_ordered_before_summary(
        false, false, 2, 2));
    return 0;
}

static int test_completion_status_classification(void) {
    CHECK(!rdna4_argument_summary_owner_status_needs_error(
        RDNA4_ARGUMENT_SUMMARY_COMPLETE,
        RDNA4_ARGUMENT_SUMMARY_COMPLETE));
    CHECK(!rdna4_argument_summary_owner_status_needs_error(
        RDNA4_ARGUMENT_SUMMARY_READY,
        RDNA4_ARGUMENT_SUMMARY_READY));
    CHECK(!rdna4_argument_summary_owner_status_needs_error(
        RDNA4_ARGUMENT_SUMMARY_STALE,
        RDNA4_ARGUMENT_SUMMARY_COMPLETE));
    CHECK(!rdna4_argument_summary_owner_status_needs_error(
        RDNA4_ARGUMENT_SUMMARY_OBSOLETE,
        RDNA4_ARGUMENT_SUMMARY_READY));
    CHECK(rdna4_argument_summary_owner_status_needs_error(
        RDNA4_ARGUMENT_SUMMARY_INVALID,
        RDNA4_ARGUMENT_SUMMARY_COMPLETE));
    CHECK(rdna4_argument_summary_owner_status_needs_error(
        RDNA4_ARGUMENT_SUMMARY_OVERFLOW,
        RDNA4_ARGUMENT_SUMMARY_COMPLETE));
    CHECK(rdna4_argument_summary_owner_status_needs_error(
        RDNA4_ARGUMENT_SUMMARY_INCOMPLETE,
        RDNA4_ARGUMENT_SUMMARY_READY));
    CHECK(rdna4_argument_summary_owner_status_needs_error(
        RDNA4_ARGUMENT_SUMMARY_FAILED,
        RDNA4_ARGUMENT_SUMMARY_READY));
    /* A wrong-stage success is not a silent skip either. */
    CHECK(rdna4_argument_summary_owner_status_needs_error(
        RDNA4_ARGUMENT_SUMMARY_COMPLETE,
        RDNA4_ARGUMENT_SUMMARY_READY));
    CHECK(rdna4_argument_summary_owner_status_needs_error(
        RDNA4_ARGUMENT_SUMMARY_READY,
        RDNA4_ARGUMENT_SUMMARY_COMPLETE));
    return 0;
}

static int test_candidate_bitset_is_stable(void) {
    const uint64_t tokens[] = {
        UINT64_C(0x5244000000000001),
        UINT64_C(0x5244000000000011),
        UINT64_C(0x5244000000000021),
        UINT64_C(0x5244000000000031),
    };
    const uint32_t words[] = {
        (UINT32_C(1) << 0) | (UINT32_C(1) << 3),
        UINT32_C(1) << 1,
    };
    CHECK(rdna4_argument_summary_owner_candidate_index(
              tokens, 4, tokens[0]) == 0);
    CHECK(rdna4_argument_summary_owner_candidate_index(
              tokens, 4, tokens[3]) == 3);
    CHECK(rdna4_argument_summary_owner_candidate_index(
              tokens, 4, UINT64_C(0x5244000000000000)) == 0);
    CHECK(rdna4_argument_summary_owner_candidate_index(
              tokens, 4, UINT64_C(0x5244000000000040)) == 4);
    CHECK(rdna4_argument_summary_owner_candidate_found(
              words, 2, 0));
    CHECK(rdna4_argument_summary_owner_candidate_found(
              words, 2, 33));
    CHECK(!rdna4_argument_summary_owner_candidate_found(
              words, 2, 1));
    CHECK(!rdna4_argument_summary_owner_candidate_found(
              words, 1, 33));
    CHECK(rdna4_argument_summary_owner_token_found(
              tokens, 4, words, 2, tokens[0]));
    CHECK(rdna4_argument_summary_owner_token_found(
              tokens, 4, words, 2, tokens[3]));
    CHECK(!rdna4_argument_summary_owner_token_found(
              tokens, 4, words, 2, tokens[1]));
    CHECK(!rdna4_argument_summary_owner_token_found(
              tokens, 4, words, 2, UINT64_C(0x5244000000000040)));
    return 0;
}

static int test_relocated_and_uncaptured_candidates(void) {
    const uint64_t captured[] = {
        UINT64_C(0x5244000000000001),
        UINT64_C(0x5244000000000021),
    };
    const uint64_t relocated[] = {
        UINT64_C(0x5244000000000021),
        UINT64_C(0x5244000000000031),
    };
    CHECK(rdna4_argument_summary_owner_token_captured(
              captured, 2, relocated[0]));
    CHECK(!rdna4_argument_summary_owner_token_captured(
              captured, 2, relocated[1]));
    /*
     * The summary found relocated[0], but only at an unknown offset. Its old
     * exact slot/range is not exact-range retainable; the publication path
     * clears it and adds the found object as whole-buffer possible.
     */
    const uint32_t found[] = {UINT32_C(1) << 0};
    CHECK(rdna4_argument_summary_owner_token_found(
              relocated, 2, found, 1, relocated[0]));
    CHECK(rdna4_argument_summary_owner_token_captured(
              captured, 2, relocated[0]));
    CHECK(!rdna4_argument_summary_owner_exact_range_retained(
        rdna4_argument_summary_owner_token_captured(
            captured, 2, relocated[0])));

    /* A token first observed after capture is preserved for the next round. */
    CHECK(!rdna4_argument_summary_owner_token_captured(
              captured, 2, relocated[1]));
    CHECK(rdna4_argument_summary_owner_exact_range_retained(
        rdna4_argument_summary_owner_token_captured(
            captured, 2, relocated[1])));
    CHECK(!rdna4_argument_summary_owner_completed_prunable(
        1, 2, 1,
        rdna4_argument_summary_owner_token_captured(
            captured, 2, relocated[1]), true));
    return 0;
}

static int test_summary_work_predicate(void) {
    CHECK(!rdna4_argument_summary_owner_state_needed(
        false, true, true, true, true, true));
    CHECK(!rdna4_argument_summary_owner_state_needed(
        true, false, false, false, false, false));
    CHECK(rdna4_argument_summary_owner_state_needed(
        true, true, false, false, false, false));
    CHECK(rdna4_argument_summary_owner_state_needed(
        true, false, true, false, false, false));
    CHECK(rdna4_argument_summary_owner_state_needed(
        true, false, false, true, false, false));
    CHECK(rdna4_argument_summary_owner_state_needed(
        true, false, false, false, true, false));
    CHECK(rdna4_argument_summary_owner_state_needed(
        true, false, false, false, false, true));
    /*
     * A Private fill with no incoming token is still summary work while it
     * clears an existing slot/possible owner. Once the last owner is gone, an
     * otherwise candidate-free Private transfer stays off the summary path.
     */
    CHECK(rdna4_argument_summary_owner_state_needed(
        true, false, true, false, false, false));
    CHECK(!rdna4_argument_summary_owner_state_needed(
        true, false, false, false, false, false));
    return 0;
}

static int test_owner_range_relevance(void) {
    CHECK(rdna4_argument_summary_owner_ranges_overlap(10, 8, 10, 1));
    CHECK(rdna4_argument_summary_owner_ranges_overlap(10, 8, 17, 1));
    CHECK(rdna4_argument_summary_owner_ranges_overlap(17, 1, 10, 8));
    CHECK(!rdna4_argument_summary_owner_ranges_overlap(10, 8, 18, 1));
    CHECK(!rdna4_argument_summary_owner_ranges_overlap(18, 1, 10, 8));
    CHECK(!rdna4_argument_summary_owner_ranges_overlap(10, 0, 10, 1));
    CHECK(!rdna4_argument_summary_owner_ranges_overlap(10, 1, 10, 0));
    CHECK(rdna4_argument_summary_owner_ranges_overlap(
        UINT64_MAX - 4, 5, UINT64_MAX, 1));
    return 0;
}

int main(void) {
    if (test_capture_serial_saturation()) return 1;
    if (test_reverse_queue_acceptance_order()) return 1;
    if (test_completed_pruning_epochs()) return 1;
    if (test_summary_order_proof()) return 1;
    if (test_completion_status_classification()) return 1;
    if (test_candidate_bitset_is_stable()) return 1;
    if (test_relocated_and_uncaptured_candidates()) return 1;
    if (test_summary_work_predicate()) return 1;
    if (test_owner_range_relevance()) return 1;
    printf("ARGUMENT_SUMMARY_OWNERS_PASS checks=%u "
           "accepted_epoch_order=1 reverse_queue=1 "
           "encoded_only_nonblocking=1 host_generation_guard=1 "
           "relocated_token=1 postcapture_uncaptured=1 "
           "no_sampler_fastpath=1 private_fill_retire=1 "
           "segment_completed_order=1 strict_status=1\n",
           checks);
    return 0;
}
