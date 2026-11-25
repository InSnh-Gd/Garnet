// garnet_irq_wait_pure.h — Pure C++17 policy for MSI CP EOP IRQ-driven fence wakeups.
// Host-tested under ASan+UBSan by tests/garnet_irq_wait_test.cpp.
#pragma once

#include <stdint.h>

namespace rdna4native {
namespace irqwait {

constexpr uint32_t kLatchUnset = 0u;
constexpr uint32_t kLatchOff   = 1u;
constexpr uint32_t kLatchOn    = 2u;

// Boot-arg `garnet-irq-wait`: default ON when absent or non-zero; `=0` disables IRQ wait.
constexpr uint32_t latch_value(bool present, uint32_t val) {
    return (present && val == 0u) ? kLatchOff : kLatchOn;
}
constexpr bool latch_is_on(uint32_t latch) {
    return latch == kLatchOn;
}

// IRQ wait is active only when MSI was armed, the IH ring is enabled with CPU delivery,
// and the kill-switch `garnet-irq-wait=0` was not passed.
constexpr bool active(bool wantInterrupts, bool ihEnabled, bool ihCpuIntr, bool bootArgOn) {
    return wantInterrupts && ihEnabled && ihCpuIntr && bootArgOn;
}

constexpr uint64_t kFastSpinNs    = 50000ull;     // 50 us spin before sleeping on IRQ
constexpr uint64_t kLegacySpinNs  = 5000000ull;   // 5 ms legacy polled spin
constexpr uint64_t kIrqFallbackNs = 2000000ull;   // 2 ms max bounded sleep fallback
constexpr uint64_t kMinSleepNs    = 50000ull;     // avoid sub-50us sleep overhead

enum WaitAction : uint32_t {
    kActDelay1us = 0u,
    kActSleep1ms = 1u,
    kActIrqWait  = 2u,
};

struct WaitStep {
    WaitAction action;
    uint64_t   sleepNs;
};

constexpr WaitStep next_step(bool irqActive, uint64_t elapsedNs, uint64_t remainingNs) {
    if (!irqActive) {
        return elapsedNs < kLegacySpinNs ? WaitStep{ kActDelay1us, 0ull }
                                         : WaitStep{ kActSleep1ms, 1000000ull };
    }
    if (elapsedNs < kFastSpinNs || remainingNs < kMinSleepNs) {
        return WaitStep{ kActDelay1us, 0ull };
    }
    const uint64_t bound = remainingNs < kIrqFallbackNs ? remainingNs : kIrqFallbackNs;
    return WaitStep{ kActIrqWait, bound };
}

// PACKET3(RELEASE_MEM = 0x49, count = 6) header and DW2 INT_SEL field (bits [25:24]).
constexpr uint32_t kReleaseMemHeader = (3u << 30) | (6u << 16) | (0x49u << 8);
constexpr uint32_t kIntSelMask       = 3u << 24;
constexpr uint32_t kIntSelSendInt    = 2u << 24;   // kPM4RMIntSelSendInt (SEND_DATA_AND_WRITE_CONFIRM)

constexpr bool has_trailing_release_mem(const uint32_t *dw, uint32_t n) {
    return dw != nullptr && n >= 8u && dw[n - 8u] == kReleaseMemHeader;
}

constexpr uint32_t dw2_with_eop_int(uint32_t dw2) {
    return (dw2 & ~kIntSelMask) | kIntSelSendInt;
}

// IH source IDs that should wake fence/ring waiters:
//   181       = CP end-of-pipe (EOP)
//   0         = UTCL2 / VM fault
//   183..185  = CP bad-opcode / protected-reg / error
constexpr bool should_wake_on_iv(uint8_t srcId) {
    return srcId == 181u || srcId == 0u || (srcId >= 183u && srcId <= 185u);
}

// Epoch check under the leaf wait lock prevents lost wakeups between condition check and sleep.
constexpr bool should_sleep_for_epoch(uint64_t epochBeforeCheck, uint64_t epochUnderLock) {
    return epochBeforeCheck == epochUnderLock;
}

} // namespace irqwait
} // namespace rdna4native
