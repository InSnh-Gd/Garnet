#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "rdna4_display_commits.h"

int main(void) {
    rdna4_display_commit entries[128] = {{0}}, *head = NULL;
    unsigned sid[128] = {0}; uint64_t order[128] = {0}, clock = 0;
    srand(495);
    for (unsigned step = 0; step < 100000; step++) {
        unsigned i = (unsigned)rand() % 128;
        if (sid[i]) {
            rdna4dc_commit_remove(&head, &entries[i]); sid[i] = 0; order[i] = 0;
        } else {
            sid[i] = (unsigned)rand() % 32 + 1; order[i] = ++clock;
            rdna4dc_commit_add(&head, &entries[i], sid[i], order[i]);
            rdna4dc_commit_add(&head, &entries[i], sid[i], order[i]); // duplicate commit is idempotent
        }
        unsigned surface = (unsigned)rand() % 32 + 1;
        uint64_t stamp = clock ? (unsigned)rand() % clock : 0;
        int expected = 0;
        for (unsigned n = 0; n < 128; n++) expected |= sid[n] == surface && order[n] > stamp;
        assert(rdna4dc_commit_newer(head, surface, stamp) == expected);
        unsigned actual = 0, count = 0;
        for (unsigned n = 0; n < 128; n++) count += sid[n] != 0;
        for (rdna4_display_commit *p = head; p; p = p->next) assert(++actual <= count);
        assert(actual == count);
    }
    for (unsigned i = 0; i < 128; i++) rdna4dc_commit_remove(&head, &entries[i]);
    assert(!head);
    rdna4df_t state; rdna4df_init(&state, 0); rdna4df_activate(&state, 3);
    uint32_t flags[3] = {4,4,4};
    int slot = rdna4df_pick(&state, flags); rdna4df_submit(&state, slot, 8, 10); state.sub_commit[slot] = 1;
    rdna4dc_commit_invalidate(&state, 7, 2);
    assert(!rdna4df_is_superseded(&state, slot));
    rdna4dc_commit_invalidate(&state, 8, 1);
    assert(!rdna4df_is_superseded(&state, slot));
    rdna4dc_commit_invalidate(&state, 8, 2);
    assert(!rdna4df_complete_surf(&state, slot, 0, 10, 8) && state.superseded == 1);
    slot = rdna4df_pick(&state, flags); rdna4df_submit(&state, slot, 8, 11); state.sub_commit[slot] = 2;
    assert(rdna4df_complete_surf(&state, slot, 0, 11, 8));
    rdna4df_retry_note(&state, slot, 8, 11, 2, RDNA4DF_C_FINAL, -EBUSY);
    rdna4dc_commit_invalidate(&state, 7, 3);
    assert(state.retry_slot == slot && state.retry_dropped == 0);
    rdna4dc_commit_invalidate(&state, 8, 1);
    assert(state.retry_slot == slot && state.retry_dropped == 0);
    rdna4dc_commit_invalidate(&state, 8, 2);
    assert(state.retry_slot == slot && state.retry_dropped == 0);
    rdna4dc_commit_invalidate(&state, 8, 3);
    assert(state.retry_slot == -1 && state.retry_dropped == 1);
    puts("DISPLAY_COMMITS_PASS 100000 independent list/oracle steps; same-surface ordering, release and retry checks");
    return 0;
}
