// Host ASan+UBSan regression test for rdna4gfxoff::GuardState, 100 ms delayed-idle
// AllowGfxOff hysteresis, SDMA7 zero-MMIO doorbell policy, and production guard pinning.
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>

#include "amd/garnet_gfxoff_pure.h"

static int gRun = 0, gFail = 0;

static void check(bool ok, const char *name) {
    gRun++;
    if (!ok) {
        gFail++;
        std::printf("FAIL  %s\n", name);
    }
}

static std::string slurp(const std::string &path) {
    std::ifstream f(path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static void test_boot_policy() {
    using namespace rdna4gfxoff;
    check(kGfxOffIdleDelayMs == 100u, "idle hysteresis default is 100 ms");
    check(!gfxoff_hw_opt_in(false, 0u), "absent boot-arg keeps GFXOFF opt-in off");
    check(!gfxoff_hw_opt_in(true, 0u), "garnet-gfxoff=0 disables opt-in");
    check(gfxoff_hw_opt_in(true, 1u), "garnet-gfxoff=1 opts in");
    check(!gfxoff_hw_opt_in(true, 2u), "an unrecognized boot value leaves hardware GFXOFF off");
    check(gfxoff_kill_switch_active(true, 0u), "garnet-gfxoff=0 activates kill-switch");
    check(!gfxoff_kill_switch_active(false, 0u), "absent boot-arg does not set kill-switch");

    check(sdma_use_bar2_doorbell_only(true, 0x1000ull, 0x2000ull),
          "SDMA7 uses BAR2-only doorbell when aperture covers [offs, offs+8)");
    check(!sdma_use_bar2_doorbell_only(false, 0x1000ull, 0x2000ull),
          "SDMA7 falls back only if BAR2 is unmapped");
    check(!sdma_use_bar2_doorbell_only(true, 0x1ffcul, 0x2000ull),
          "SDMA7 rejects out-of-range BAR2 doorbell");
    check(!sdma_use_bar2_doorbell_only(true, UINT64_MAX - 2ull, UINT64_MAX),
          "SDMA7 overflow-safe bounds check");
}

static void test_hysteresis_and_coalescing() {
    using namespace rdna4gfxoff;
    GuardState g;
    g.configure(true, 1u, /*hysteresis=*/true);
    check(g.arm_while_awake(), "arm succeeds when opt-in and awake");
    check(g.hw_enabled && g.hw_disallowed && !g.pending_allow, "arming does not send AllowGfxOff at boot");

    // First guard doesn't need a mailbox round trip because no Allow was sent.
    Action a = g.request(false, true);
    check(a == Action::None && g.disallow_count == 1u && g.hw_disallowed, "first acquire keeps GC awake");

    // Nested 1 -> 2 -> 1 -> 2 -> 1 causes zero SMU traffic
    check(g.request(false, true) == Action::None && g.disallow_count == 2u, "nested acquire 1->2");
    check(g.request(true, true) == Action::None && g.disallow_count == 1u, "nested release 2->1");
    check(g.disallow_ops == 0u && g.allow_ops == 0u, "no SMU ops on nested transitions");

    // 1 -> 0 schedules 100 ms delayed AllowGfxOff instead of synchronous SendAllow
    a = g.request(true, true);
    check(a == Action::ScheduleAllow && g.pending_allow && g.idle_timer_arms == 1u,
          "1->0 with hysteresis schedules deferred AllowGfxOff");
    check(g.hw_disallowed && g.allow_ops == 0u, "GC remains disallowed while timer pending");

    // Rapid re-acquire before 100 ms cancels pending timer with zero SMU traffic
    a = g.request(false, true);
    check(a == Action::None && !g.pending_allow && g.disallow_count == 1u && g.idle_timer_cancels == 1u,
          "re-acquire during 100 ms window cancels deferred allow with zero SMU traffic");
    check(g.disallow_ops == 0u, "zero DisallowGfxOff sent during coalesced burst");

    // Release again, then let 100 ms timer fire
    a = g.request(true, true);
    check(a == Action::ScheduleAllow && g.pending_allow, "second 1->0 schedules timer");
    g.allow_deadline = 250u;
    check(g.on_idle_timer(true, 249u) == Action::None && g.pending_allow,
          "callback blocked before cancellation cannot consume the newer deadline");
    a = g.on_idle_timer(true, 250u);
    check(a == Action::SendAllow && !g.pending_allow, "idle timer triggers SendAllow");
    g.commit(a, true);
    check(!g.hw_disallowed && g.allow_ops == 1u, "AllowGfxOff committed after 100 ms idle");

    // Stale timer firing while re-acquired or faulted is ignored safely
    a = g.request(false, true);
    check(a == Action::SendDisallow, "next 0->1 after allow sends DisallowGfxOff");
    g.commit(a, true);
    check(g.disallow_count == 1u && g.hw_disallowed && g.disallow_ops == 1u, "disallowed committed");
    check(g.on_idle_timer(true, 300u) == Action::None, "stale timer ignored while guard held");
}

static void test_underflow_and_fault_latch() {
    using namespace rdna4gfxoff;
    GuardState g;
    g.configure(true, 1u, /*hysteresis=*/false);
    check(g.arm_while_awake(), "arm succeeds");

    // Unmatched release at refcount 0
    check(g.request(true, true) == Action::Underflow && g.underflow_count == 1u && g.disallow_count == 0u,
          "underflow at refcount 0 refused without wrapping");

    // Disallow failure latches faulted (fail-closed)
    (void)g.request(false, true);
    g.commit(g.request(true, true), true);
    Action a = g.request(false, true);
    check(a == Action::SendDisallow, "0->1 sends Disallow");
    g.commit(a, false);
    check(g.faulted && g.fault_count == 1u && g.disallow_count == 0u,
          "failed Disallow latches faulted and leaves disallow_count 0");
    check(g.request(false, true) == Action::Faulted, "subsequent acquire stays faulted");
    check(g.on_idle_timer(true, 100u) == Action::None, "idle timer refuses when faulted");
    check(!g.arm_while_awake(), "re-arming refused once faulted");

    // Teardown while active
    GuardState g2;
    g2.configure(true, 1u, false);
    check(g2.arm_while_awake(), "g2 armed");
    (void)g2.request(false, true);
    g2.commit(g2.request(true, true), true);
    check(g2.disable_hw() == Action::SendDisallow && !g2.hw_enabled && !g2.hw_disallowed,
          "teardown while GFXOFF active returns SendDisallow");
    g2.finish_disable(true);
    check(g2.disable_hw() == Action::None, "second teardown is idempotent");
}

static void test_unconfirmed_power_transition() {
    using namespace rdna4gfxoff;
    GuardState state;
    state.configure(true, 1u, false);
    state.arm_while_awake();
    (void)state.request(false, true);
    state.commit(state.request(true, true), false);
    check(!state.hw_disallowed && state.request(false, true) == Action::SendDisallow,
          "failed Allow leaves GC unproven until a new Disallow is acknowledged");
    check(state.disable_hw() == Action::SendDisallow && !state.hw_disallowed,
          "teardown cannot assume GC is awake before Disallow completes");
    check(state.disable_hw() == Action::SendDisallow,
          "unconfirmed teardown remains unsafe on a second attempt");
    state.finish_disable(false);
    check(state.request(false, true) == Action::Faulted,
          "lost Disallow acknowledgement blocks GC even after deactivating GFXOFF");
}

static void test_production_pins(const std::string &srcRoot) {
    const std::string s1b = slurp(srcRoot + "/src/amd/native_s1b.cpp");
    const std::string vm  = slurp(srcRoot + "/src/amd/garnet_session_vm.cpp");
    const std::string rs  = slurp(srcRoot + "/src/amd/garnet_ring_submit.cpp");
    const std::string drv = slurp(srcRoot + "/src/GarnetDriver.cpp");
    const std::string fc  = slurp(srcRoot + "/src/dcn/garnet_sdma_fastcopy.cpp");
    const std::string ct  = slurp(srcRoot + "/src/amd/compute_test.cpp");
    const std::string sdma = slurp(srcRoot + "/src/amd/sdma_v7_0.cpp");

    check(s1b.find("ScopedGfxOffGuard") != std::string::npos, "native_s1b.cpp uses ScopedGfxOffGuard");
    check(vm.find("ScopedGfxOffGuard") != std::string::npos, "garnet_session_vm.cpp uses ScopedGfxOffGuard");
    check(rs.find("ScopedGfxOffGuard") != std::string::npos, "garnet_ring_submit.cpp uses ScopedGfxOffGuard");
    check(drv.find("ScopedGfxOffGuard") != std::string::npos, "GarnetDriver.cpp uses ScopedGfxOffGuard");
    check(fc.find("ScopedGfxOffGuard") != std::string::npos, "garnet_sdma_fastcopy.cpp uses ScopedGfxOffGuard");
    check(ct.find("ScopedGfxOffGuard") != std::string::npos, "compute_test.cpp uses ScopedGfxOffGuard");
    check(sdma.find("sdma_use_bar2_doorbell_only") != std::string::npos, "sdma_v7_0.cpp uses BAR2-only doorbell policy");
}

int main(int argc, char **argv) {
    test_boot_policy();
    test_hysteresis_and_coalescing();
    test_underflow_and_fault_latch();
    test_unconfirmed_power_transition();
    if (argc >= 2) test_production_pins(argv[1]);
    std::printf("garnet_gfxoff_guard_test: %d/%d checks passed, %d failed\n", gRun - gFail, gRun, gFail);
    return gFail ? 1 : 0;
}
