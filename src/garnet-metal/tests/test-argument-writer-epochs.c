#include "garnet_argument_epochs.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "ARGUMENT_WRITER_EPOCH_FAIL %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

#define MAX_INTENTS 8
#define MAX_PIECES 16
#define MAX_COMPLETED 32

typedef struct {
    uint64_t serial;
    uint64_t submitted_epoch;
    uint64_t host_generation;
    rdna4_ae_range range;
    rdna4_ae_range write_ranges[MAX_PIECES];
    size_t count;
    rdna4_ae_range host_overlaps[MAX_PIECES];
    size_t host_overlap_count;
    unsigned candidate;
    bool fill_only;
    bool active;
    bool submitted;
} intent;

typedef struct {
    uint64_t serial;
    uint64_t submitted_epoch;
    rdna4_ae_range range;
    unsigned candidate;
    bool fill_only;
    bool exclusive;
} completed;

typedef struct {
    intent intents[MAX_INTENTS];
    size_t intent_count;
    completed completed[MAX_COMPLETED];
    size_t completed_count;
    uint64_t serial;
    uint64_t submitted_epoch;
    uint64_t host_generation;
    unsigned possible;
    bool old_owner;
    bool host_owner;
    unsigned applied;
} model;

static void model_init(model *m) {
    memset(m, 0, sizeof(*m));
}

static size_t subtract_ranges(rdna4_ae_range *ranges, size_t count, rdna4_ae_range cut) {
    rdna4_ae_range next[MAX_PIECES];
    size_t next_count = 0;
    for (size_t i = 0; i < count; ++i) {
        rdna4_ae_range pieces[2];
        size_t n = rdna4_ae_range_subtract(ranges[i], cut, pieces);
        CHECK(next_count + n <= MAX_PIECES);
        for (size_t j = 0; j < n; ++j) next[next_count++] = pieces[j];
    }
    memcpy(ranges, next, next_count * sizeof(*next));
    return next_count;
}

static void add_range(rdna4_ae_range *ranges, size_t *count, rdna4_ae_range range) {
    if (!range.length) return;
    CHECK(*count < MAX_PIECES);
    ranges[(*count)++] = range;
    for (size_t i = 1; i < *count; ++i) {
        for (size_t j = i; j > 0 &&
             ranges[j - 1].location > ranges[j].location; --j) {
            rdna4_ae_range swap = ranges[j - 1];
            ranges[j - 1] = ranges[j];
            ranges[j] = swap;
        }
    }
    size_t dst = 0;
    for (size_t i = 0; i < *count; ++i) {
        if (dst && ranges[i].location <= ranges[dst - 1].location +
                                             ranges[dst - 1].length) {
            uint64_t end = ranges[dst - 1].location + ranges[dst - 1].length;
            uint64_t other = ranges[i].location + ranges[i].length;
            ranges[dst - 1].length = (end > other ? end : other) -
                                     ranges[dst - 1].location;
        } else {
            ranges[dst++] = ranges[i];
        }
    }
    *count = dst;
}

static intent *model_register(model *m, rdna4_ae_range range, unsigned candidate,
                              bool fill_only) {
    CHECK(m->intent_count < MAX_INTENTS);
    CHECK(rdna4_ae_range_valid(range, UINT64_C(1) << 20));
    CHECK(range.length != 0);
    intent *it = &m->intents[m->intent_count++];
    memset(it, 0, sizeof(*it));
    it->serial = ++m->serial;
    it->host_generation = m->host_generation;
    it->range = range;
    it->write_ranges[0] = range;
    it->count = 1;
    it->candidate = candidate;
    it->fill_only = fill_only;
    it->active = true;
    return it;
}

static void model_submit(model *m, intent *it) {
    CHECK(it->active);
    CHECK(!it->submitted);
    it->submitted_epoch = ++m->submitted_epoch;
    it->submitted = true;
}

static void model_refine(model *m) {
    for (;;) {
        bool selected = false;
        rdna4_ae_range span = {0, 0};
        bool group[MAX_COMPLETED] = {false};
        for (size_t seed = 0; seed < m->completed_count && !selected; ++seed) {
            memset(group, 0, sizeof(group));
            group[seed] = true;
            span = m->completed[seed].range;
            bool expanded;
            do {
                expanded = false;
                for (size_t i = 0; i < m->completed_count; ++i) {
                    if (group[i] ||
                        !rdna4_ae_range_intersects(span, m->completed[i].range)) continue;
                    group[i] = true;
                    uint64_t end = span.location + span.length;
                    uint64_t other_end = m->completed[i].range.location +
                                         m->completed[i].range.length;
                    uint64_t begin = span.location < m->completed[i].range.location ?
                        span.location : m->completed[i].range.location;
                    span = (rdna4_ae_range){ begin, (end > other_end ? end : other_end) - begin };
                    expanded = true;
                }
            } while (expanded);
            bool blocked = false;
            for (size_t i = 0; i < m->intent_count; ++i)
                if (m->intents[i].active &&
                    rdna4_ae_range_intersects(m->intents[i].range, span)) {
                    blocked = true;
                    break;
                }
            if (!blocked) selected = true;
        }
        if (!selected) return;

        unsigned candidates = 0;
        bool all_fill = true;
        bool has_protected = false;
        for (size_t i = 0; i < m->completed_count; ++i) {
            if (!group[i]) continue;
            candidates |= m->completed[i].candidate;
            all_fill = all_fill && m->completed[i].fill_only;
            has_protected = has_protected || !m->completed[i].exclusive;
        }
        if (all_fill && !has_protected) {
            m->old_owner = false;
            m->possible = 0;
        } else if (!all_fill) {
            m->possible |= candidates;
        }
        m->applied++;

        size_t dst = 0;
        for (size_t i = 0; i < m->completed_count; ++i)
            if (!group[i]) m->completed[dst++] = m->completed[i];
        m->completed_count = dst;
    }
}

static void model_complete(model *m, intent *it) {
    if (!it->active || !it->submitted) return;
    it->active = false;
    for (size_t i = 0; i < it->count; ++i) {
        rdna4_ae_range exclusive[MAX_PIECES] = {it->write_ranges[i]};
        size_t exclusive_count = 1;
        for (size_t j = 0; j < it->host_overlap_count; ++j)
            exclusive_count = subtract_ranges(exclusive, exclusive_count,
                                              it->host_overlaps[j]);
        for (size_t j = 0; j < exclusive_count; ++j) {
            CHECK(m->completed_count < MAX_COMPLETED);
            m->completed[m->completed_count++] = (completed){
                .serial = it->serial,
                .submitted_epoch = it->submitted_epoch,
                .range = exclusive[j],
                .candidate = it->candidate,
                .fill_only = it->fill_only,
                .exclusive = true,
            };
        }
        for (size_t j = 0; j < it->host_overlap_count; ++j) {
            rdna4_ae_range overlap =
                rdna4_ae_range_intersection(it->write_ranges[i], it->host_overlaps[j]);
            if (!overlap.length) continue;
            CHECK(m->completed_count < MAX_COMPLETED);
            m->completed[m->completed_count++] = (completed){
                .serial = it->serial,
                .submitted_epoch = it->submitted_epoch,
                .range = overlap,
                .candidate = it->candidate,
                .fill_only = it->fill_only,
                .exclusive = false,
            };
        }
    }
    model_refine(m);
}

static void model_abandon(model *m, intent *it) {
    if (!it->active) return;
    it->active = false;
    model_refine(m);
}

static void model_host_store(model *m, rdna4_ae_range range) {
    m->host_generation++;
    for (size_t i = 0; i < m->intent_count; ++i) {
        intent *it = &m->intents[i];
        if (!it->active) continue;
        rdna4_ae_range overlap = rdna4_ae_range_intersection(it->range, range);
        if (overlap.length)
            add_range(it->host_overlaps, &it->host_overlap_count, overlap);
    }
    size_t dst = 0;
    for (size_t i = 0; i < m->completed_count; ++i) {
        completed original = m->completed[i];
        rdna4_ae_range pieces[2];
        size_t n = rdna4_ae_range_subtract(original.range, range, pieces);
        for (size_t j = 0; j < n; ++j) {
            CHECK(dst < MAX_COMPLETED);
            m->completed[dst] = original;
            m->completed[dst++].range = pieces[j];
        }
        rdna4_ae_range overlap = rdna4_ae_range_intersection(original.range, range);
        if (overlap.length) {
            CHECK(dst < MAX_COMPLETED);
            m->completed[dst] = original;
            m->completed[dst].range = overlap;
            m->completed[dst++].exclusive = false;
        }
    }
    m->completed_count = dst;
    m->host_owner = true;
    m->old_owner = false;
    model_refine(m);
}

static void test_later_host_setter_wins(void) {
    model m;
    model_init(&m);
    m.old_owner = true;
    intent *writer = model_register(&m, (rdna4_ae_range){0, 8}, 1, false);
    model_submit(&m, writer);
    model_host_store(&m, (rdna4_ae_range){0, 8});
    CHECK(writer->active);
    CHECK(writer->host_overlap_count == 1);
    model_complete(&m, writer);
    CHECK(m.host_owner);
    CHECK(m.possible == 1);
    CHECK(m.applied == 1);
    CHECK(m.completed_count == 0);
}

static void test_partial_host_mask_keeps_other_ranges(void) {
    model m;
    model_init(&m);
    intent *writer = model_register(&m, (rdna4_ae_range){0, 32}, 1, false);
    model_submit(&m, writer);
    model_host_store(&m, (rdna4_ae_range){8, 8});
    CHECK(writer->host_overlap_count == 1);
    CHECK(writer->host_overlaps[0].location == 8 &&
          writer->host_overlaps[0].length == 8);
    model_complete(&m, writer);
    CHECK(m.completed_count == 0);
    CHECK(m.applied == 3);
    CHECK(m.possible == 1);
    CHECK(m.host_owner);
}

static void test_overlapping_completion_waits(void) {
    model m;
    model_init(&m);
    m.old_owner = true;
    intent *first = model_register(&m, (rdna4_ae_range){0, 16}, 1, true);
    intent *second = model_register(&m, (rdna4_ae_range){8, 16}, 2, true);
    model_submit(&m, first);
    model_submit(&m, second);
    model_complete(&m, second);
    CHECK(m.completed_count == 1);
    CHECK(m.applied == 0);
    CHECK(m.old_owner);
    model_complete(&m, first);
    CHECK(m.completed_count == 0);
    CHECK(m.applied == 1);
    CHECK(!m.old_owner);
}

static void test_nonoverlap_completion_refines(void) {
    model m;
    model_init(&m);
    intent *first = model_register(&m, (rdna4_ae_range){0, 8}, 1, false);
    intent *second = model_register(&m, (rdna4_ae_range){32, 8}, 2, false);
    model_submit(&m, first);
    model_submit(&m, second);
    model_complete(&m, first);
    CHECK(m.completed_count == 0);
    CHECK(m.applied == 1);
    CHECK(m.possible == 1);
    CHECK(second->active);
}

static void test_abandon_only_releases_own_blocker(void) {
    model m;
    model_init(&m);
    m.old_owner = true;
    intent *first = model_register(&m, (rdna4_ae_range){0, 16}, 1, false);
    intent *second = model_register(&m, (rdna4_ae_range){8, 8}, 2, true);
    model_submit(&m, first);
    model_submit(&m, second);
    model_complete(&m, first);
    CHECK(m.completed_count == 1);
    CHECK(m.applied == 0);
    model_abandon(&m, second);
    CHECK(m.completed_count == 0);
    CHECK(m.applied == 1);
    CHECK(m.possible == 1);
    CHECK(m.old_owner);
}

static void test_pending_fill_keeps_old_owner(void) {
    model m;
    model_init(&m);
    m.old_owner = true;
    m.possible = 4;
    intent *fill = model_register(&m, (rdna4_ae_range){0, 64}, 0, true);
    CHECK(m.old_owner && m.possible == 4);
    model_complete(&m, fill);
    CHECK(m.old_owner && m.possible == 4);
    model_submit(&m, fill);
    model_complete(&m, fill);
    CHECK(!m.old_owner && m.possible == 0);
}

static void test_copy_visibility_classification(void) {
    CHECK(!rdna4_ae_copy_source_fully_scanned(false, false, false));
    CHECK(!rdna4_ae_copy_source_fully_scanned(true, false, false));
    CHECK(!rdna4_ae_copy_source_fully_scanned(true, true, true));
    CHECK(rdna4_ae_copy_source_fully_scanned(true, true, false));
    CHECK(rdna4_ae_copy_fill_only(true, true, false));
    CHECK(!rdna4_ae_copy_fill_only(false, true, false));
    CHECK(!rdna4_ae_copy_fill_only(true, false, false));
    CHECK(!rdna4_ae_copy_fill_only(true, true, true));

    model m;
    model_init(&m);
    m.old_owner = true;
    m.possible = 4;
    bool fill_only = rdna4_ae_copy_fill_only(false, true, false);
    intent *copy = model_register(&m, (rdna4_ae_range){0, 64}, 8, fill_only);
    model_submit(&m, copy);
    model_complete(&m, copy);
    CHECK(m.old_owner);
    CHECK(m.possible == (4u | 8u));

    model known;
    model_init(&known);
    known.old_owner = true;
    known.possible = 4;
    fill_only = rdna4_ae_copy_fill_only(true, true, false);
    CHECK(fill_only);
    intent *visible = model_register(&known, (rdna4_ae_range){0, 64}, 0, fill_only);
    model_submit(&known, visible);
    model_complete(&known, visible);
    CHECK(!known.old_owner);
    CHECK(known.possible == 0);
}

static void test_unsubmitted_abandon_preserves_old(void) {
    model m;
    model_init(&m);
    m.old_owner = true;
    m.possible = 4;
    intent *fill = model_register(&m, (rdna4_ae_range){0, 64}, 0, true);
    model_abandon(&m, fill);
    CHECK(m.old_owner);
    CHECK(m.possible == 4);
    CHECK(m.applied == 0);
    CHECK(m.completed_count == 0);
}

static void test_range_helpers(void) {
    CHECK(rdna4_ae_range_valid((rdna4_ae_range){10, 5}, 15));
    CHECK(!rdna4_ae_range_valid((rdna4_ae_range){10, 6}, 15));
    CHECK(!rdna4_ae_range_intersects((rdna4_ae_range){0, 0}, (rdna4_ae_range){0, 8}));
    rdna4_ae_range pieces[2] = {{0, 0}, {0, 0}};
    size_t count = rdna4_ae_range_subtract((rdna4_ae_range){0, 32},
                                           (rdna4_ae_range){8, 8}, pieces);
    CHECK(count == 2);
    CHECK(pieces[0].location == 0 && pieces[0].length == 8);
    CHECK(pieces[1].location == 16 && pieces[1].length == 16);
}

int main(void) {
    test_range_helpers();
    test_later_host_setter_wins();
    test_partial_host_mask_keeps_other_ranges();
    test_overlapping_completion_waits();
    test_nonoverlap_completion_refines();
    test_abandon_only_releases_own_blocker();
    test_pending_fill_keeps_old_owner();
    test_copy_visibility_classification();
    test_unsubmitted_abandon_preserves_old();
    puts("ARGUMENT_WRITER_EPOCH_PASS cases=9 "
         "range_split=1 completion_order=1 pending_fill=1 abandoned=1 "
         "completion_requires_submit=1 copy_visibility=1");
    return 0;
}
