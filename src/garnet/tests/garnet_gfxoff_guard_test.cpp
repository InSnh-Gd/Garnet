#include "amd/garnet_gfxoff_pure.h"

#include <cstdio>

namespace {
int gChecks = 0;
int gFail   = 0;

void expect(bool cond, const char *msg) {
    ++gChecks;
    if (!cond) {
        ++gFail;
        std::printf("FAIL: %s\n", msg);
    }
}

// Simulated kernel driver wiring mirroring amdgpu::gfx_off_ctrl + ScopedGfxOffGuard.
struct FakeSmu {
    bool smuReady     { true };
    bool failDisallow { false };
    bool failAllow    { false };
    int  disallowSent { 0 };
    int  allowSent    { 0 };
    rdna4gfxoff::GuardState state {};

    int ctrl(bool allow) {
        using namespace rdna4gfxoff;
        const Action act = state.request(allow, smuReady);
        if (act == Action::None) return 0;
        if (act == Action::Underflow) return -2;
        if (act == Action::Faulted) return -3;
        if (act == Action::SendDisallow) {
            disallowSent++;
            const bool ok = !failDisallow;
            state.commit(act, ok);
            return ok ? 0 : -1;
        }
        if (act == Action::SendAllow) {
            allowSent++;
            const bool ok = !failAllow;
            state.commit(act, ok);
            return ok ? 0 : -1;
        }
        return -4;
    }
};

struct FakeScopedGuard {
    FakeSmu *smu;
    int      rc;
    bool     held;
    explicit FakeScopedGuard(FakeSmu &s) : smu(&s), rc(s.ctrl(false)), held(rc == 0) {}
    ~FakeScopedGuard() { if (held) (void)smu->ctrl(true); }
    bool ok() const { return rc == 0; }
};

} // namespace

int main() {
    using namespace rdna4gfxoff;

    // 1. Boot-arg policy: absent defaults OFF, =0 kills, =1 opts in.
    expect(!gfxoff_kill_switch_active(false, 0u), "garnet-gfxoff absent does not set kill-switch");
    expect(gfxoff_kill_switch_active(true, 0u), "garnet-gfxoff=0 activates kill-switch");
    expect(!gfxoff_kill_switch_active(true, 1u), "garnet-gfxoff=1 does not set kill-switch");
    expect(!gfxoff_hw_opt_in(false, 0u), "garnet-gfxoff absent keeps hw AllowGfxOff OFF");
    expect(!gfxoff_hw_opt_in(true, 0u), "garnet-gfxoff=0 keeps hw AllowGfxOff OFF");
    expect(gfxoff_hw_opt_in(true, 1u), "garnet-gfxoff=1 opts into hw AllowGfxOff");

    // 2. Default boot (absent): guard tracks nesting inertly with zero SMU traffic.
    {
        FakeSmu smu {};
        smu.state.configure(false, 0u);
        expect(!smu.state.hw_enabled && smu.state.hw_disallowed, "default boot starts hw_enabled=false");
        expect(smu.ctrl(false) == 0 && smu.state.disallow_count == 1u, "inert disallow increments count");
        expect(smu.ctrl(false) == 0 && smu.state.disallow_count == 2u, "inert nested disallow increments count");
        expect(smu.ctrl(true) == 0 && smu.state.disallow_count == 1u, "inert allow decrements count");
        expect(smu.ctrl(true) == 0 && smu.state.disallow_count == 0u, "inert outer allow returns to 0");
        expect(smu.disallowSent == 0 && smu.allowSent == 0, "default boot emits zero SMU messages");
        // Underflow protection even when inert.
        expect(smu.ctrl(true) == -2 && smu.state.disallow_count == 0u && smu.state.underflow_count == 1u,
               "inert underflow refused without wrap");
    }

    // 3. Kill-switch garnet-gfxoff=0 refuses arming and never emits SMU traffic.
    {
        FakeSmu smu {};
        smu.state.configure(true, 0u);
        expect(smu.state.kill_switch, "garnet-gfxoff=0 sets kill_switch");
        expect(!smu.state.arm_hw_allowed(), "garnet-gfxoff=0 refuses arm_hw_allowed()");
        {
            FakeScopedGuard g1(smu);
            FakeScopedGuard g2(smu);
            expect(g1.ok() && g2.ok() && smu.state.disallow_count == 2u, "kill-switch RAII nesting safe");
        }
        expect(smu.state.disallow_count == 0u && smu.disallowSent == 0 && smu.allowSent == 0,
               "kill-switch emits zero SMU messages");
    }

    // 4. Active hardware GFXOFF (garnet-gfxoff=1): 0->1 Disallow, nested no-op, 1->0 Allow.
    {
        FakeSmu smu {};
        smu.state.configure(true, 1u);
        expect(smu.state.arm_hw_allowed(), "garnet-gfxoff=1 arms hw GFXOFF tracking");
        expect(smu.state.hw_enabled && !smu.state.hw_disallowed, "armed state marks GFXOFF allowed");

        {
            FakeScopedGuard outer(smu);
            expect(outer.ok() && smu.disallowSent == 1 && smu.allowSent == 0 && smu.state.hw_disallowed,
                   "0 -> 1 sends exactly one DisallowGfxOff");
            expect(smu.state.disallow_count == 1u, "outer depth is 1");
            {
                FakeScopedGuard inner1(smu);
                FakeScopedGuard inner2(smu);
                expect(inner1.ok() && inner2.ok() && smu.disallowSent == 1 && smu.allowSent == 0,
                       "nested disallow sends zero extra DisallowGfxOff");
                expect(smu.state.disallow_count == 3u, "nested depth reaches 3");
            }
            expect(smu.state.disallow_count == 1u && smu.allowSent == 0 && smu.state.hw_disallowed,
                   "inner releases do not send AllowGfxOff prematurely");
        }
        expect(smu.state.disallow_count == 0u && smu.allowSent == 1 && !smu.state.hw_disallowed,
               "1 -> 0 sends exactly one AllowGfxOff");
        expect(smu.state.disallow_ops == 1u && smu.state.allow_ops == 1u,
               "transition counters match 1 disallow + 1 allow");

        // Second cycle works identically.
        {
            FakeScopedGuard g(smu);
            expect(g.ok() && smu.disallowSent == 2 && smu.allowSent == 1, "second cycle disallows cleanly");
        }
        expect(smu.disallowSent == 2 && smu.allowSent == 2 && !smu.state.hw_disallowed,
               "second cycle re-allows cleanly");

        // Unmatched allow() at refcount 0: underflow protection, zero SMU traffic.
        expect(smu.ctrl(true) == -2, "unmatched allow returns underflow error");
        expect(smu.state.disallow_count == 0u && smu.state.underflow_count == 1u && smu.allowSent == 2,
               "underflow does not wrap refcount or send AllowGfxOff");
    }

    // 5. Error fail-safe: DisallowGfxOff failure latches faulted, returns error, never silently re-allows.
    {
        FakeSmu smu {};
        smu.state.configure(true, 1u);
        expect(smu.state.arm_hw_allowed(), "arm succeeds before fault test");
        smu.failDisallow = true;
        {
            FakeScopedGuard g(smu);
            expect(!g.ok() && !g.held, "failed DisallowGfxOff surfaces error and marks guard unheld");
            expect(smu.state.faulted && smu.state.disallow_count == 0u && smu.disallowSent == 1,
                   "failed DisallowGfxOff latches faulted without incrementing count");
        }
        expect(smu.allowSent == 0, "failed RAII guard destructor does not send AllowGfxOff");
        // Subsequent attempts fail closed even if SMU recovers.
        smu.failDisallow = false;
        expect(smu.ctrl(false) == -3 && smu.disallowSent == 1, "latched fault fails closed on subsequent acquires");
    }

    // 6. Error fail-safe: SMU not ready while GFXOFF active fails closed (never proceeds unsafely).
    {
        FakeSmu smu {};
        smu.state.configure(true, 1u);
        expect(smu.state.arm_hw_allowed(), "arm succeeds before offline SMU test");
        smu.smuReady = false;
        FakeScopedGuard g(smu);
        expect(!g.ok() && smu.state.faulted && smu.disallowSent == 0,
               "offline SMU while GFXOFF active fails closed without touching GC MMIO");
    }

    // 7. Fail-safe on AllowGfxOff failure: stays disallowed safely.
    {
        FakeSmu smu {};
        smu.state.configure(true, 1u);
        expect(smu.state.arm_hw_allowed(), "arm succeeds before AllowGfxOff failure test");
        smu.failAllow = true;
        {
            FakeScopedGuard g(smu);
            expect(g.ok() && smu.disallowSent == 1, "disallow succeeds");
        }
        expect(smu.allowSent == 1 && smu.state.hw_disallowed && smu.state.disallow_count == 0u,
               "failed AllowGfxOff leaves hw_disallowed=true (safe)");
        // Next acquire sees hw_disallowed=true -> no duplicate DisallowGfxOff needed.
        smu.failAllow = false;
        {
            FakeScopedGuard g2(smu);
            expect(g2.ok() && smu.disallowSent == 1, "already disallowed skips redundant DisallowGfxOff");
        }
        expect(smu.allowSent == 2 && !smu.state.hw_disallowed, "subsequent AllowGfxOff succeeds cleanly");
    }

    // 8. Teardown disable_hw: requests DisallowGfxOff only if currently allowed, disables permanently.
    {
        GuardState st {};
        st.configure(true, 1u);
        expect(st.arm_hw_allowed(), "arm for teardown");
        expect(st.disable_hw() == Action::SendDisallow, "disable_hw while allowed requests DisallowGfxOff");
        expect(!st.hw_enabled && st.hw_disallowed, "disable_hw disarms state");
        expect(st.disable_hw() == Action::None, "second disable_hw is a no-op");
    }

    std::printf("garnet_gfxoff_guard_test: %d checks, %d failed\n", gChecks, gFail);
    return gFail ? 1 : 0;
}
