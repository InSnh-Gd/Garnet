// garnet_queue_pipe.h — Pure Phase A3 state machine for bounded multi-CB
// pipelining per RDNA4CommandQueue and submit-time superseded D-copy elision.
// Zero ObjC, zero Vulkan, zero locking; host-tested in test-queue-pipe.c.
#ifndef GARNET_QUEUE_PIPE_H
#define GARNET_QUEUE_PIPE_H

#include <stdint.h>
#include <string.h>

#define GARNET_QP_DEFAULT_MAX_INFLIGHT 4u
#define GARNET_QP_SERIAL_MAX_INFLIGHT  1u
#define GARNET_QP_CAP                  8u
#define GARNET_SERIAL_QUEUE_FILE       "/private/tmp/garnet-serial-queue"

typedef struct {
    unsigned max_inflight;
    unsigned inflight_count;
    int barrier_active;
    int faulted;
    unsigned max_observed_depth;
    uint64_t pipelined_submits;
    uint64_t barrier_waits;
    uint64_t elided_dcopies;
} garnet_qp_t;

static inline void garnet_qp_init(garnet_qp_t *qp, unsigned max_inflight) {
    memset(qp, 0, sizeof *qp);
    if (max_inflight < 1u) max_inflight = 1u;
    if (max_inflight > GARNET_QP_CAP) max_inflight = GARNET_QP_CAP;
    qp->max_inflight = max_inflight;
}

static inline int garnet_qp_eligible(int has_events, unsigned seg_count, int host_only,
                                     int native_eligible, int prepared, int has_prepare_err) {
    return !has_events && seg_count == 1u && !host_only && !native_eligible && prepared && !has_prepare_err;
}

static inline int garnet_qp_can_launch(const garnet_qp_t *qp, int head_eligible) {
    if (!qp) return 0;
    if (qp->faulted) return qp->inflight_count == 0u;
    if (qp->barrier_active) return 0;
    if (!head_eligible) return qp->inflight_count == 0u;
    return qp->inflight_count < qp->max_inflight;
}

static inline void garnet_qp_on_launch(garnet_qp_t *qp, int eligible) {
    if (!qp) return;
    qp->inflight_count++;
    if (!eligible) qp->barrier_active = 1;
    else if (qp->inflight_count > 1u) qp->pipelined_submits++;
    if (qp->inflight_count > qp->max_observed_depth) qp->max_observed_depth = qp->inflight_count;
}

static inline void garnet_qp_on_complete(garnet_qp_t *qp, int was_barrier, int failed) {
    if (!qp) return;
    if (qp->inflight_count > 0u) qp->inflight_count--;
    if (was_barrier) qp->barrier_active = 0;
    if (failed) qp->faulted = 1;
}

// Submit-time superseded D-copy elision: omit dispCopyVk when a newer CB for
// the same display surface is already committed/submitted/pending, provided D2
// damage chaining is OFF (when D2 is ON, eliding a chain base would poison
// later partial frames).
static inline int garnet_qp_should_elide_dcopy(int has_disp_copy_vk, int slot, int damage_on,
                                               int is_superseded, int pending_newer_same_sid) {
    return has_disp_copy_vk && slot >= 0 && !damage_on && (is_superseded || pending_newer_same_sid);
}

#endif
