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

// Adaptive launch deferral for a WindowServer FINAL CB carrying dispCopyVk while
// paired FINAL->COMP compositing is armed. Gives the paired COMP commit a short
// bounded window to arrive so FINAL's wasted 4K dispCopyVk is elided before submit.
#define GARNET_QP_FINAL_COALESCE_MIN_NS     200000ULL
#define GARNET_QP_FINAL_COALESCE_DEFAULT_NS 250000ULL
#define GARNET_QP_FINAL_COALESCE_PAD_NS     150000ULL
#define GARNET_QP_FINAL_COALESCE_MAX_NS     800000ULL

static inline uint64_t garnet_qp_final_defer_ns(int is_ws, int has_disp_copy_vk, int is_final,
                                                int sync_commit, int hold_armed, int damage_on,
                                                int pending_newer_same_sid, int is_superseded,
                                                uint64_t pair_gap_ns, uint64_t tcommit, uint64_t now) {
    if (!is_ws || !has_disp_copy_vk || !is_final || sync_commit || !hold_armed || damage_on ||
        pending_newer_same_sid || is_superseded || !tcommit)
        return 0u;
    uint64_t win = pair_gap_ns ? (pair_gap_ns + GARNET_QP_FINAL_COALESCE_PAD_NS)
                               : GARNET_QP_FINAL_COALESCE_DEFAULT_NS;
    if (win < GARNET_QP_FINAL_COALESCE_MIN_NS) win = GARNET_QP_FINAL_COALESCE_MIN_NS;
    if (win > GARNET_QP_FINAL_COALESCE_MAX_NS) win = GARNET_QP_FINAL_COALESCE_MAX_NS;
    const uint64_t elapsed = (now >= tcommit) ? (now - tcommit) : 0u;
    return (elapsed < win) ? (win - elapsed) : 0u;
}

// Active scanout flip (RDNA4DF_ACTIVE) publishes frames asynchronously from the GPU
// fence completion handler on the single FIFO Vulkan queue, so WindowServer's
// commitAndWaitUntilSubmitted only needs waitUntilCompleted in fallback/non-active
// scanout mode, on slotless late copies, or when forced via ws_completion_all.
static inline int garnet_qp_ws_needs_completion_wait(int is_ws, int no_ws_completion,
                                                     int ws_completion_all, int is_frame,
                                                     int scanout_active, int dlate) {
    if (!is_ws || no_ws_completion) return 0;
    if (ws_completion_all) return 1;
    return is_frame && (!scanout_active || dlate);
}

// Compute encoder barrier policy (Wave 9):
//   MTLDispatchTypeSerial (0): defer a compute-only barrier to the next direct
//   dispatch, skipping DRAW_INDIRECT / PFP_SYNC_ME / WB_L2 so 4 MiB GL2 and
//   32 MiB Infinity Cache (MALL) stay warm. Indirect dispatches and endEncoding
//   keep rdna4_full_barrier.
//   MTLDispatchTypeConcurrent (1): omit implicit inter-dispatch barriers; the
//   caller orders dependencies with memoryBarrierWithScope: / Resources:.
#define GARNET_COMP_DISPATCH_SERIAL     0u
#define GARNET_COMP_DISPATCH_CONCURRENT 1u

static inline unsigned garnet_comp_norm_dispatch_type(uint64_t dispatch_type) {
    return dispatch_type == GARNET_COMP_DISPATCH_CONCURRENT
        ? GARNET_COMP_DISPATCH_CONCURRENT : GARNET_COMP_DISPATCH_SERIAL;
}

static inline int garnet_comp_needs_pre_barrier(unsigned dispatch_type, int pending_serial,
                                                int is_indirect) {
    return dispatch_type == GARNET_COMP_DISPATCH_SERIAL && pending_serial && !is_indirect;
}

// MTLBarrierScope: Buffers=1<<0, Textures=1<<1, RenderTargets=1<<2.
// Compute-local scopes only need compute-to-compute ordering; RenderTargets or
// unknown higher bits fall back to rdna4_full_barrier.
static inline int garnet_comp_scope_needs_full_barrier(uint64_t scope) {
    return (scope & ~3ULL) != 0ULL;
}

#endif
