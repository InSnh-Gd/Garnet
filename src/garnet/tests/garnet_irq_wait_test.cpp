// Host ASan+UBSan regression tests for garnet_irq_wait_pure.h (Stage 2A).
#include "garnet_irq_wait_pure.h"
#include "amdgpu_pm4.h"
#include "cp_pm4_gfx12.h"
#include "garnet_submit_pure.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

using namespace rdna4native::irqwait;

static int gFails = 0;

static void check(bool cond, const char *msg) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        gFails++;
    }
}

static void test_latch_and_active() {
    check(latch_value(false, 0u) == kLatchOn, "absent boot-arg defaults ON");
    check(latch_value(true, 1u) == kLatchOn, "garnet-irq-wait=1 is ON");
    check(latch_value(true, 42u) == kLatchOn, "nonzero boot-arg is ON");
    check(latch_value(true, 0u) == kLatchOff, "garnet-irq-wait=0 disables IRQ wait");
    check(latch_is_on(kLatchOn) && !latch_is_on(kLatchOff) && !latch_is_on(kLatchUnset), "latch_is_on");

    check(active(true, true, true, true), "active only when all four prerequisites hold");
    check(!active(false, true, true, true), "inactive without wantInterrupts");
    check(!active(true, false, true, true), "inactive without ihEnabled");
    check(!active(true, true, false, true), "inactive without ihCpuIntr");
    check(!active(true, true, true, false), "inactive when garnet-irq-wait=0");
}

static void test_wait_schedule() {
    // Legacy polled mode when IRQ wait is inactive.
    WaitStep s = next_step(false, 0ull, 2000000000ull);
    check(s.action == kActDelay1us && s.sleepNs == 0ull, "legacy <5ms spins 1us");
    s = next_step(false, 4999999ull, 1000000000ull);
    check(s.action == kActDelay1us, "legacy 4.999ms still spins 1us");
    s = next_step(false, 5000000ull, 1000000000ull);
    check(s.action == kActSleep1ms && s.sleepNs == 1000000ull, "legacy >=5ms sleeps 1ms");

    // IRQ-active mode: 50us fast spin, then bounded 2ms IRQ sleep.
    s = next_step(true, 0ull, 2000000000ull);
    check(s.action == kActDelay1us, "irq-active 0us spins briefly");
    s = next_step(true, 49999ull, 2000000000ull);
    check(s.action == kActDelay1us, "irq-active 49.999us still spins");
    s = next_step(true, 50000ull, 2000000000ull);
    check(s.action == kActIrqWait && s.sleepNs == kIrqFallbackNs, "irq-active >=50us sleeps up to 2ms fallback");
    s = next_step(true, 500000ull, 300000ull);
    check(s.action == kActIrqWait && s.sleepNs == 300000ull, "irq-active clamps sleep to remaining timeout");
    s = next_step(true, 500000ull, 49999ull);
    check(s.action == kActDelay1us, "irq-active spins when remaining < minSleep");
}

static void test_eop_packet_patching() {
    static_assert(kReleaseMemHeader == 0xC0064900u, "RELEASE_MEM count=6 header matches CP PM4");
    check(kReleaseMemHeader == amdgpu::cp_p3(amdgpu::kP3_RELEASE_MEM, 6), "header matches cp_p3");

    const uint64_t va[1] = { 0xffff800100001000ull };
    const uint32_t by[1] = { 64u };
    uint32_t pkt[32] = {};
    const uint32_t n = rdna4native::s1c::build_submit(
        pkt, 32u, va, by, 1u, 8u, true, 0x8400001000ull, 0x8400002000ull, 0x123456789abcdef0ull);
    check(n == 23u, "1 IB + user fence + seqno = 23 dwords");
    check(has_trailing_release_mem(pkt, n), "trailing packet recognized as RELEASE_MEM");
    check(!has_trailing_release_mem(pkt, 7u), "short buffer rejected");
    check(!has_trailing_release_mem(nullptr, 8u), "null buffer rejected");

    const uint32_t dw2Before = pkt[n - 6u];
    const uint32_t dw2Expected = amdgpu::pm4_release_mem_dw2(amdgpu::kPM4RMDataSel64, amdgpu::kPM4RMIntSelSendInt);
    const uint32_t dw2After = dw2_with_eop_int(dw2Before);
    check(dw2Before == amdgpu::pm4_release_mem_dw2(amdgpu::kPM4RMDataSel64, amdgpu::kPM4RMIntSelNone),
          "build_submit default keeps INT_SEL=0");
    check(dw2After == dw2Expected, "dw2_with_eop_int sets INT_SEL=2 preserving DATA_SEL=2");
    check(((dw2After >> 29) & 7u) == 2u && ((dw2After >> 24) & 3u) == 2u, "DATA_SEL=2 INT_SEL=2");

    // User fence (earlier RELEASE_MEM at n-16) stays INT_SEL=0 when only trailing slot is patched.
    check(((pkt[n - 14u] >> 24) & 3u) == 0u, "user fence keeps INT_SEL=0");

    // Compute ring build_compute_submit also ends with a patchable trailing RELEASE_MEM.
    uint32_t cpkt[32] = {};
    const uint32_t cn = rdna4native::s1c::build_compute_submit(
        cpkt, 32u, va, by, 1u, 9u, true, 0x8400001000ull, (1ull << 62) | 5ull, 0x8400002000ull, 5ull);
    check(cn == 20u, "1 compute IB + user fence + seqno = 20 dwords");
    check(has_trailing_release_mem(cpkt, cn), "compute submit trailing packet recognized as RELEASE_MEM");
    check(dw2_with_eop_int(cpkt[cn - 6u]) == dw2Expected, "compute trailing RELEASE_MEM patches cleanly to INT_SEL=2");
    check(((cpkt[cn - 14u] >> 24) & 3u) == 0u, "compute user fence keeps INT_SEL=0");
}

static void test_iv_wake_and_epoch() {
    check(should_wake_on_iv(181u), "CP EOP wakes waiters");
    check(should_wake_on_iv(0u), "VM fault wakes waiters");
    check(should_wake_on_iv(183u) && should_wake_on_iv(184u) && should_wake_on_iv(185u), "CP errors wake waiters");
    check(!should_wake_on_iv(1u) && !should_wake_on_iv(180u) && !should_wake_on_iv(182u) && !should_wake_on_iv(255u),
          "unrelated IVs do not wake waiters");

    check(should_sleep_for_epoch(10ull, 10ull), "same epoch allows sleep");
    check(!should_sleep_for_epoch(10ull, 11ull), "advanced epoch skips sleep (no lost wakeup)");
}

int main() {
    test_latch_and_active();
    test_wait_schedule();
    test_eop_packet_patching();
    test_iv_wake_and_epoch();
    if (gFails != 0) {
        fprintf(stderr, "garnet_irq_wait_test: %d failure(s)\n", gFails);
        return 1;
    }
    printf("garnet_irq_wait_test: ALL PASS\n");
    return 0;
}
