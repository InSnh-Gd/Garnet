// test-queue-pipe.c — Host ASan/UBSan tests for Phase A3 bounded multi-CB
// queue pipelining, strict FIFO completion ordering, and submit-time superseded
// D-copy elision (garnet_queue_pipe.h).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "garnet_queue_pipe.h"

static int g_checks = 0, g_fails = 0;
#define CHECK(name, cond) do { \
    g_checks++; \
    if (!(cond)) { g_fails++; printf("FAIL: %s\n", name); } \
} while (0)

int main(void) {
    // 1. Eligibility rules
    CHECK("eligible: simple prepared single-segment CB",
          garnet_qp_eligible(0, 1, 0, 0, 1, 0) == 1);
    CHECK("ineligible: has MTLEvent/MTLFence",
          garnet_qp_eligible(1, 1, 0, 0, 1, 0) == 0);
    CHECK("ineligible: multi-segment CB",
          garnet_qp_eligible(0, 2, 0, 0, 1, 0) == 0);
    CHECK("ineligible: zero-segment CB",
          garnet_qp_eligible(0, 0, 0, 0, 1, 0) == 0);
    CHECK("ineligible: hostOnly segment",
          garnet_qp_eligible(0, 1, 1, 0, 1, 0) == 0);
    CHECK("ineligible: native-buffer route",
          garnet_qp_eligible(0, 1, 0, 1, 1, 0) == 0);
    CHECK("ineligible: unprepared CB",
          garnet_qp_eligible(0, 1, 0, 0, 0, 0) == 0);
    CHECK("ineligible: prepare error",
          garnet_qp_eligible(0, 1, 0, 0, 1, 1) == 0);

    // 2. Bounded 4-deep pipelining + barrier drain
    garnet_qp_t qp;
    garnet_qp_init(&qp, GARNET_QP_DEFAULT_MAX_INFLIGHT);
    CHECK("init: default max_inflight == 4", qp.max_inflight == 4u && qp.inflight_count == 0u);
    for (unsigned i = 0; i < 4; i++) {
        CHECK("can launch eligible up to cap 4", garnet_qp_can_launch(&qp, 1) == 1);
        garnet_qp_on_launch(&qp, 1);
    }
    CHECK("5th eligible blocked at cap 4", garnet_qp_can_launch(&qp, 1) == 0);
    CHECK("barrier blocked while 4 eligible in flight", garnet_qp_can_launch(&qp, 0) == 0);
    CHECK("stats: depth=4 pipelined=3", qp.max_observed_depth == 4u && qp.pipelined_submits == 3u);

    // Drain 3 -> still 1 in flight -> barrier STILL blocked, eligible allowed
    garnet_qp_on_complete(&qp, 0, 0);
    garnet_qp_on_complete(&qp, 0, 0);
    garnet_qp_on_complete(&qp, 0, 0);
    CHECK("1 in flight: eligible allowed, barrier still blocked",
          qp.inflight_count == 1u && garnet_qp_can_launch(&qp, 1) == 1 && garnet_qp_can_launch(&qp, 0) == 0);
    garnet_qp_on_complete(&qp, 0, 0);
    CHECK("0 in flight: barrier allowed", qp.inflight_count == 0u && garnet_qp_can_launch(&qp, 0) == 1);

    // Launch barrier -> blocks BOTH eligible and another barrier until complete
    garnet_qp_on_launch(&qp, 0);
    CHECK("barrier active blocks eligible and barrier",
          qp.barrier_active == 1 && garnet_qp_can_launch(&qp, 1) == 0 && garnet_qp_can_launch(&qp, 0) == 0);
    garnet_qp_on_complete(&qp, 1, 0);
    CHECK("barrier complete reopens pipelining",
          qp.barrier_active == 0 && qp.inflight_count == 0u && garnet_qp_can_launch(&qp, 1) == 1);

    // 3. Serial kill-switch (max_inflight = 1)
    garnet_qp_t sq;
    garnet_qp_init(&sq, GARNET_QP_SERIAL_MAX_INFLIGHT);
    CHECK("serial mode launches 1", garnet_qp_can_launch(&sq, 1) == 1);
    garnet_qp_on_launch(&sq, 1);
    CHECK("serial mode blocks 2nd", garnet_qp_can_launch(&sq, 1) == 0);
    garnet_qp_on_complete(&sq, 0, 0);
    CHECK("serial mode allows next after complete", garnet_qp_can_launch(&sq, 1) == 1);

    // 4. Fault isolation: once a CB fails, stop overlapping launches
    garnet_qp_t fq;
    garnet_qp_init(&fq, GARNET_QP_DEFAULT_MAX_INFLIGHT);
    garnet_qp_on_launch(&fq, 1);
    garnet_qp_on_launch(&fq, 1);
    garnet_qp_on_complete(&fq, 0, 1); // first CB failed!
    CHECK("faulted with 1 still in flight blocks new launches",
          fq.faulted == 1 && garnet_qp_can_launch(&fq, 1) == 0);
    garnet_qp_on_complete(&fq, 0, 0); // drain last in-flight
    CHECK("faulted with 0 in flight allows ordered error drain",
          fq.inflight_count == 0u && garnet_qp_can_launch(&fq, 1) == 1);

    // 5. Submit-time superseded D-copy elision
    CHECK("elide when superseded on valid slot with dispCopyVk",
          garnet_qp_should_elide_dcopy(1, 0, 0, 1, 0) == 1);
    CHECK("elide when newer pending CB writes same sid",
          garnet_qp_should_elide_dcopy(1, 2, 0, 0, 1) == 1);
    CHECK("no elide when neither superseded nor newer pending",
          garnet_qp_should_elide_dcopy(1, 1, 0, 0, 0) == 0);
    CHECK("no elide when no dispCopyVk",
          garnet_qp_should_elide_dcopy(0, 1, 0, 1, 1) == 0);
    CHECK("no elide when slot < 0",
          garnet_qp_should_elide_dcopy(1, -1, 0, 1, 1) == 0);
    CHECK("no elide when D2 damage chaining is ON (protects base slot)",
          garnet_qp_should_elide_dcopy(1, 1, 1, 1, 1) == 0);

    // 6. Strict FIFO completion drain under random out-of-order fence wakeups
    srand(42);
    garnet_qp_t sim;
    garnet_qp_init(&sim, GARNET_QP_DEFAULT_MAX_INFLIGHT);
    unsigned next_submit = 0, next_deliver = 0;
    unsigned inflight_ids[4] = {0};
    int inflight_ready[4] = {0};
    unsigned inflight_n = 0;
    int order_ok = 1;
    while (next_deliver < 1000) {
        while (next_submit < 1000 && garnet_qp_can_launch(&sim, 1)) {
            garnet_qp_on_launch(&sim, 1);
            inflight_ids[inflight_n] = next_submit++;
            inflight_ready[inflight_n] = 0;
            inflight_n++;
        }
        // Randomly complete any in-flight fence out of order
        if (inflight_n > 0) {
            unsigned pick = (unsigned)(rand() % (int)inflight_n);
            inflight_ready[pick] = 1;
            // Drain strictly from FIFO head
            while (inflight_n > 0 && inflight_ready[0]) {
                if (inflight_ids[0] != next_deliver) order_ok = 0;
                next_deliver++;
                garnet_qp_on_complete(&sim, 0, 0);
                for (unsigned k = 1; k < inflight_n; k++) {
                    inflight_ids[k - 1] = inflight_ids[k];
                    inflight_ready[k - 1] = inflight_ready[k];
                }
                inflight_n--;
            }
        }
    }
    CHECK("1000 out-of-order fence completions deliver strictly FIFO",
          order_ok && next_deliver == 1000 && sim.inflight_count == 0u && sim.max_observed_depth == 4u);

    // 7. Adaptive FINAL D-copy coalesce deferral
    CHECK("defer: default 250 us when armed WS FINAL has no pair_gap yet",
          garnet_qp_final_defer_ns(1, 1, 1, 0, 1, 0, 0, 0, 0, 1000000ULL, 1050000ULL) == 200000ULL);
    CHECK("defer: adaptive window tracks pair_gap + 150 us clamped [200, 800] us",
          garnet_qp_final_defer_ns(1, 1, 1, 0, 1, 0, 0, 0, 60000ULL, 1000000ULL, 1010000ULL) == 200000ULL &&
          garnet_qp_final_defer_ns(1, 1, 1, 0, 1, 0, 0, 0, 400000ULL, 1000000ULL, 1050000ULL) == 500000ULL &&
          garnet_qp_final_defer_ns(1, 1, 1, 0, 1, 0, 0, 0, 2000000ULL, 1000000ULL, 1100000ULL) == 700000ULL);
    CHECK("defer: zero once elapsed >= window",
          garnet_qp_final_defer_ns(1, 1, 1, 0, 1, 0, 0, 0, 60000ULL, 1000000ULL, 1210000ULL) == 0ULL);
    CHECK("defer: zero when non-WS, non-FINAL, syncCommit, disarmed, damageOn, or already superseded/pendingNewer",
          garnet_qp_final_defer_ns(0, 1, 1, 0, 1, 0, 0, 0, 60000ULL, 1000000ULL, 1010000ULL) == 0ULL &&
          garnet_qp_final_defer_ns(1, 0, 1, 0, 1, 0, 0, 0, 60000ULL, 1000000ULL, 1010000ULL) == 0ULL &&
          garnet_qp_final_defer_ns(1, 1, 0, 0, 1, 0, 0, 0, 60000ULL, 1000000ULL, 1010000ULL) == 0ULL &&
          garnet_qp_final_defer_ns(1, 1, 1, 1, 1, 0, 0, 0, 60000ULL, 1000000ULL, 1010000ULL) == 0ULL &&
          garnet_qp_final_defer_ns(1, 1, 1, 0, 0, 0, 0, 0, 60000ULL, 1000000ULL, 1010000ULL) == 0ULL &&
          garnet_qp_final_defer_ns(1, 1, 1, 0, 1, 1, 0, 0, 60000ULL, 1000000ULL, 1010000ULL) == 0ULL &&
          garnet_qp_final_defer_ns(1, 1, 1, 0, 1, 0, 1, 0, 60000ULL, 1000000ULL, 1010000ULL) == 0ULL &&
          garnet_qp_final_defer_ns(1, 1, 1, 0, 1, 0, 0, 1, 60000ULL, 1000000ULL, 1010000ULL) == 0ULL);

    printf("test-queue-pipe: %d checks, %d failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
