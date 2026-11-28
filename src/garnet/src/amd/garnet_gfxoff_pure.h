// garnet_gfxoff_pure.h — Pure refcounted GC MMIO GFXOFF guard state machine,
// 100 ms delayed-idle AllowGfxOff hysteresis, and SDMA7 zero-MMIO doorbell policy.
// Header-only, C++17, zero kernel/IOKit headers, zero floating point.
//
// Boot-arg policy (`garnet-gfxoff`):
//   absent          -> hardware AllowGfxOff stays OFF (opt-in until RX 9060 XT soak)
//   garnet-gfxoff=0 -> explicit emergency kill-switch: forces GFXOFF off and blocks arming
//   garnet-gfxoff=1 -> opt-in hardware AllowGfxOff protected by balanced Disallow/Allow refcount
//
// Safety invariants:
//   - Nested disallow calls increment refcount without spamming SMU mailbox (only 0 -> 1 sends DisallowGfxOff).
//   - 1 -> 0 release debounces via ~100 ms idle timer (ScheduleAllow) so rapid VM flushes coalesce;
//     a subsequent 0 -> 1 acquire before the timer fires cancels pending_allow with zero SMU traffic.
//   - Unmatched allow at refcount 0 returns Underflow, never wraps, never sends AllowGfxOff.
//   - If DisallowGfxOff fails or SMU is offline while GFXOFF is active, latches faulted (fail-safe)
//     so callers fail closed instead of touching powered-down GC MMIO, and never silently re-allow.
#pragma once

#include <stdint.h>

namespace rdna4gfxoff {

constexpr uint32_t kGfxOffIdleDelayMs = 100u; // matches Linux AMD_GFXOFF_DELAY_MS

inline bool gfxoff_kill_switch_active(bool present, uint32_t value) {
    return present && value == 0u;
}

inline bool gfxoff_hw_opt_in(bool present, uint32_t value) {
    return present && value == 1u;
}

// Pure SDMA7 doorbell policy: when BAR2 covers [offs, offs + 8), sysmem wptr +
// NBIO HDP flush + BAR2 WDOORBELL64 wake GFXOFF in hardware with zero GC MMIO
// and zero SMU lock dependency under gScanoutLock.
inline bool sdma_use_bar2_doorbell_only(bool bar2Mapped, uint64_t offs, uint64_t bar2Size) {
    return bar2Mapped && offs <= bar2Size && (bar2Size - offs) >= 8ull;
}

enum class Action : uint8_t {
    None          = 0, // No SMU mailbox or timer action needed.
    SendDisallow  = 1, // 0 -> 1 while hw_disallowed == false: send PPSMC::DisallowGfxOff and commit().
    SendAllow     = 2, // Synchronous or timer-fired allow: send PPSMC::AllowGfxOff and commit().
    Underflow     = 3, // allow() called with refcount == 0: refused, no SMU traffic.
    Faulted       = 4, // Disallow failed or SMU offline while GFXOFF active: fail closed.
    ScheduleAllow = 5, // 1 -> 0 with hysteresis enabled: arm 100 ms delayed-idle timer.
};

struct GuardState {
    bool     hw_enabled         { false }; // opt-in with a working guard and idle timer
    bool     kill_switch        { false }; // true when garnet-gfxoff=0 explicitly specified
    bool     hw_disallowed      { true };  // false also covers an unknown result from AllowGfxOff
    bool     faulted            { false }; // sticky fail-safe latch on DisallowGfxOff / SMU-not-ready failure
    bool     hysteresis_enabled { true };  // true: 1 -> 0 schedules 100 ms delayed AllowGfxOff
    bool     pending_allow      { false }; // true while delayed AllowGfxOff timer is armed
    uint64_t allow_deadline     { 0u };
    uint32_t disallow_count     { 0u };    // active nested disallow depth
    uint32_t disallow_ops       { 0u };    // successful hw_disallowed=false -> true DisallowGfxOff transitions
    uint32_t allow_ops          { 0u };    // successful AllowGfxOff transitions
    uint32_t idle_timer_arms    { 0u };    // 1 -> 0 transitions that armed the delayed-idle timer
    uint32_t idle_timer_cancels { 0u };    // 0 -> 1 transitions coalesced while timer was pending
    uint32_t underflow_count    { 0u };    // refused unmatched allow() calls
    uint32_t fault_count        { 0u };    // DisallowGfxOff / SMU-not-ready failure count

    void configure(bool bootArgPresent, uint32_t bootArgValue, bool hysteresis = true) {
        kill_switch        = gfxoff_kill_switch_active(bootArgPresent, bootArgValue);
        hw_enabled         = false;
        hw_disallowed      = true;
        faulted            = false;
        hysteresis_enabled = hysteresis;
        pending_allow      = false;
        allow_deadline     = 0u;
        disallow_count     = 0u;
        disallow_ops       = 0u;
        allow_ops          = 0u;
        idle_timer_arms    = 0u;
        idle_timer_cancels = 0u;
        underflow_count    = 0u;
        fault_count        = 0u;
    }

    bool arm_while_awake() {
        if (!hw_disallowed || kill_switch || faulted) return false;
        hw_enabled = true;
        return true;
    }

    // Teardown / permanent disable for remainder of boot.
    // Clears any pending delayed allow and returns SendDisallow if hardware currently allows GFXOFF.
    Action disable_hw() {
        hw_enabled    = false;
        pending_allow = false;
        return hw_disallowed ? Action::None : Action::SendDisallow;
    }

    void finish_disable(bool smuOk) {
        hw_disallowed = smuOk;
        if (!smuOk) {
            faulted = true;
            fault_count++;
        }
    }

    // Request GFXOFF disallowed (allow == false) or re-allowed (allow == true).
    // When SendDisallow, SendAllow, or ScheduleAllow is returned:
    //   - SendDisallow / SendAllow: caller MUST invoke commit(action, smuOk).
    //   - ScheduleAllow: caller arms the ~100 ms idle timer to later run on_idle_timer().
    Action request(bool allow, bool smuReady) {
        if (!allow) {
            if (faulted) {
                fault_count++;
                return Action::Faulted;
            }
            if (disallow_count > 0u) {
                if (disallow_count != UINT32_MAX) disallow_count++;
                return Action::None;
            }
            if (hw_disallowed) {
                // GC is already kept awake (e.g. pending_allow hysteresis window): cancel pending allow
                // and take the guard with zero SMU mailbox traffic.
                if (pending_allow) {
                    pending_allow = false;
                    idle_timer_cancels++;
                }
                disallow_count = 1u;
                return Action::None;
            }
            if (!smuReady) {
                faulted = true;
                fault_count++;
                return Action::Faulted;
            }
            return Action::SendDisallow;
        }

        // allow == true
        if (disallow_count == 0u) {
            underflow_count++;
            return Action::Underflow;
        }
        disallow_count--;
        if (kill_switch || !hw_enabled || faulted) return Action::None;
        if (disallow_count == 0u && hw_disallowed) {
            if (!smuReady) {
                // Fail-safe: stay disallowed rather than pretending AllowGfxOff succeeded.
                return Action::None;
            }
            if (hysteresis_enabled) {
                pending_allow = true;
                idle_timer_arms++;
                return Action::ScheduleAllow;
            }
            return Action::SendAllow;
        }
        return Action::None;
    }

    // An invocation already waiting on the SMU lock can outlive its scheduled epoch.
    Action on_idle_timer(bool smuReady, uint64_t now) {
        if (kill_switch || !hw_enabled || faulted || !pending_allow) {
            pending_allow = false;
            return Action::None;
        }
        if (now < allow_deadline) return Action::None;
        if (disallow_count > 0u || !hw_disallowed || !smuReady) {
            pending_allow = false;
            return Action::None;
        }
        pending_allow = false;
        return Action::SendAllow;
    }

    void commit(Action action, bool smuOk) {
        if (action == Action::SendDisallow) {
            if (smuOk) {
                hw_disallowed = true;
                pending_allow = false;
                if (disallow_count != UINT32_MAX) disallow_count++;
                disallow_ops++;
            } else {
                // Fail-safe: latch faulted and do NOT increment disallow_count so
                // the caller knows the acquire failed and skips matching release.
                faulted = true;
                pending_allow = false;
                fault_count++;
            }
        } else if (action == Action::SendAllow) {
            hw_disallowed = false;
            pending_allow = false;
            if (smuOk) {
                allow_ops++;
            }
        }
    }
};

} // namespace rdna4gfxoff
