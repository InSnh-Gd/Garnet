// garnet_gfxoff_pure.h — Pure refcounted GC MMIO GFXOFF guard state machine.
// Header-only, C++17, zero kernel/IOKit headers, zero floating point.
//
// Boot-arg policy (`garnet-gfxoff`):
//   absent          -> hardware AllowGfxOff stays OFF (safe default), guard tracks nesting inertly
//   garnet-gfxoff=0 -> explicit emergency kill-switch: forces GFXOFF off and blocks arming
//   garnet-gfxoff=1 -> opt-in hardware AllowGfxOff protected by balanced Disallow/Allow refcount
//
// Safety invariants:
//   - Nested disallow calls increment refcount without spamming SMU mailbox (only 0 -> 1 sends DisallowGfxOff).
//   - Nested allow calls decrement refcount without re-allowing early (only 1 -> 0 sends AllowGfxOff).
//   - Unmatched allow at refcount 0 returns Underflow, never wraps, never sends AllowGfxOff.
//   - If DisallowGfxOff fails or SMU is offline while GFXOFF is active, latches faulted (fail-safe)
//     so callers fail closed instead of touching powered-down GC MMIO, and never silently re-allow.
#pragma once

#include <stdint.h>

namespace rdna4gfxoff {

inline bool gfxoff_kill_switch_active(bool present, uint32_t value) {
    return present && value == 0u;
}

inline bool gfxoff_hw_opt_in(bool present, uint32_t value) {
    return present && value != 0u;
}

enum class Action : uint8_t {
    None         = 0, // No SMU mailbox traffic needed.
    SendDisallow = 1, // Refcount 0 -> 1: caller must send PPSMC::DisallowGfxOff and call commit().
    SendAllow    = 2, // Refcount 1 -> 0: caller must send PPSMC::AllowGfxOff and call commit().
    Underflow    = 3, // allow() called with refcount == 0: refused, no SMU traffic.
    Faulted      = 4, // Disallow failed or SMU offline while GFXOFF active: fail closed.
};

struct GuardState {
    bool     hw_enabled      { false }; // true only when opt-in boot AllowGfxOff succeeded and !kill_switch
    bool     kill_switch     { false }; // true when garnet-gfxoff=0 explicitly specified
    bool     hw_disallowed   { true };  // true when GC is currently kept awake (safe default)
    bool     faulted         { false }; // sticky fail-safe latch on DisallowGfxOff / SMU-not-ready failure
    uint32_t disallow_count  { 0u };    // active nested disallow depth
    uint32_t disallow_ops    { 0u };    // successful 0 -> 1 hardware DisallowGfxOff transitions
    uint32_t allow_ops       { 0u };    // successful 1 -> 0 hardware AllowGfxOff transitions
    uint32_t underflow_count { 0u };    // refused unmatched allow() calls
    uint32_t fault_count     { 0u };    // DisallowGfxOff / SMU-not-ready failure count

    void configure(bool bootArgPresent, uint32_t bootArgValue) {
        kill_switch     = gfxoff_kill_switch_active(bootArgPresent, bootArgValue);
        hw_enabled      = false;
        hw_disallowed   = true;
        faulted         = false;
        disallow_count  = 0u;
        disallow_ops    = 0u;
        allow_ops       = 0u;
        underflow_count = 0u;
        fault_count     = 0u;
    }

    // Arm hardware GFXOFF tracking after boot-time PPSMC::AllowGfxOff succeeded.
    // Refuses if kill-switch is active, previously faulted, or any disallow guard is held.
    bool arm_hw_allowed() {
        if (kill_switch || faulted || disallow_count != 0u) return false;
        hw_enabled    = true;
        hw_disallowed = false;
        return true;
    }

    // Teardown / permanent disable for remainder of boot.
    // Returns SendDisallow if hardware currently allows GFXOFF.
    Action disable_hw() {
        const bool needDisallow = hw_enabled && !hw_disallowed;
        hw_enabled    = false;
        hw_disallowed = true;
        return needDisallow ? Action::SendDisallow : Action::None;
    }

    // Request GFXOFF disallowed (allow == false) or re-allowed (allow == true).
    // When SendDisallow or SendAllow is returned, the caller MUST invoke commit(action, smuOk).
    Action request(bool allow, bool smuReady) {
        if (!allow) {
            if (kill_switch || !hw_enabled) {
                if (disallow_count != UINT32_MAX) disallow_count++;
                return Action::None;
            }
            if (faulted) {
                fault_count++;
                return Action::Faulted;
            }
            if (disallow_count > 0u) {
                if (disallow_count != UINT32_MAX) disallow_count++;
                return Action::None;
            }
            if (!smuReady) {
                faulted = true;
                fault_count++;
                return Action::Faulted;
            }
            if (!hw_disallowed) return Action::SendDisallow;
            disallow_count = 1u;
            return Action::None;
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
            return Action::SendAllow;
        }
        return Action::None;
    }

    void commit(Action action, bool smuOk) {
        if (action == Action::SendDisallow) {
            if (smuOk) {
                hw_disallowed = true;
                if (disallow_count != UINT32_MAX) disallow_count++;
                disallow_ops++;
            } else {
                // Fail-safe: latch faulted and do NOT increment disallow_count so
                // the caller knows the acquire failed and skips matching release.
                faulted = true;
                fault_count++;
            }
        } else if (action == Action::SendAllow) {
            if (smuOk) {
                hw_disallowed = false;
                allow_ops++;
            } else {
                // Fail-safe: stay disallowed on AllowGfxOff failure.
                hw_disallowed = true;
            }
        }
    }
};

} // namespace rdna4gfxoff
