#ifndef RDNA4_ARGUMENT_EPOCHS_H
#define RDNA4_ARGUMENT_EPOCHS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t location;
    uint64_t length;
} rdna4_ae_range;

static inline bool rdna4_ae_range_valid(rdna4_ae_range range, uint64_t limit) {
    return range.location <= limit && range.length <= limit - range.location;
}

static inline bool rdna4_ae_range_intersects(rdna4_ae_range a, rdna4_ae_range b) {
    uint64_t aend = a.location + a.length;
    uint64_t bend = b.location + b.length;
    return a.length && b.length && a.location < bend && b.location < aend;
}

static inline rdna4_ae_range rdna4_ae_range_intersection(rdna4_ae_range a,
                                                        rdna4_ae_range b) {
    uint64_t begin = a.location > b.location ? a.location : b.location;
    uint64_t aend = a.location + a.length;
    uint64_t bend = b.location + b.length;
    uint64_t end = aend < bend ? aend : bend;
    rdna4_ae_range empty = { begin, 0 };
    if (begin >= end) return empty;
    rdna4_ae_range result = { begin, end - begin };
    return result;
}

// Removes cut from range. The inputs must not overflow. Returns 0, 1, or 2 pieces.
static inline size_t rdna4_ae_range_subtract(rdna4_ae_range range,
                                             rdna4_ae_range cut,
                                             rdna4_ae_range out[2]) {
    rdna4_ae_range overlap = rdna4_ae_range_intersection(range, cut);
    if (!overlap.length) {
        out[0] = range;
        return range.length ? 1 : 0;
    }
    size_t count = 0;
    if (range.location < overlap.location)
        out[count++] = (rdna4_ae_range){
            range.location, overlap.location - range.location
        };
    uint64_t range_end = range.location + range.length;
    uint64_t overlap_end = overlap.location + overlap.length;
    if (overlap_end < range_end)
        out[count++] = (rdna4_ae_range){ overlap_end, range_end - overlap_end };
    return count;
}

static inline bool rdna4_ae_copy_source_fully_scanned(bool has_contents,
                                                      bool shared_storage,
                                                      bool pending_writer) {
    return has_contents && shared_storage && !pending_writer;
}

// A copy may retire the destination's old owners only when the complete
// source range was host-visible and every destination token word was scanned.
// Whole aligned words keep the proof from relying on bytes outside the copy.
static inline bool rdna4_ae_copy_fill_only(bool source_fully_scanned,
                                           bool whole_token_words,
                                           bool found_candidates) {
    return source_fully_scanned && whole_token_words && !found_candidates;
}

#endif
