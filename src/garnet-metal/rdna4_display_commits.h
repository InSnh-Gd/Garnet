#ifndef RDNA4_DISPLAY_COMMITS_H
#define RDNA4_DISPLAY_COMMITS_H

#include "rdna4_dispflip.h"

// Intrusive entries live in their command buffer, under the scanout mutex.
typedef struct rdna4_display_commit {
    struct rdna4_display_commit *next;
    uint64_t order;
    uint32_t sid;
} rdna4_display_commit;

static inline void rdna4dc_commit_add(rdna4_display_commit **head, rdna4_display_commit *entry,
                                     uint32_t sid, uint64_t order) {
    if (entry->sid || !sid || !order) return;
    entry->sid = sid; entry->order = order; entry->next = *head; *head = entry;
}
static inline void rdna4dc_commit_remove(rdna4_display_commit **head, rdna4_display_commit *entry) {
    if (!entry->sid) return;
    for (; *head; head = &(*head)->next) if (*head == entry) {
        *head = entry->next; entry->next = NULL; entry->sid = 0; entry->order = 0; return;
    }
}
static inline int rdna4dc_commit_newer(const rdna4_display_commit *head, uint32_t sid, uint64_t order) {
    for (; head; head = head->next) if (head->sid == sid && head->order > order) return 1;
    return 0;
}
static inline void rdna4dc_commit_invalidate(rdna4df_t *state, uint32_t sid, uint64_t order) {
    for (int slot = 0; slot < RDNA4DF_MAX_SLOTS; slot++)
        if (state->inflight[slot] && state->sub_sid[slot] == sid && state->sub_commit[slot] < order)
            state->commit_sup[slot] = 1;
    if (state->retry_slot >= 0 && state->retry_sid == sid && state->retry_commit < order) {
        rdna4df_retry_clear(state);
        state->retry_dropped++;
    }
}
#endif
